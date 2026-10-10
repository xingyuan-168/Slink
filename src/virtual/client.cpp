#include "client.h"
#include "payload.h"
#include <algorithm>
#include <cstring>
#include <system_error>

namespace gwm::vd {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
ipc::Reply failure(DWORD error, const wchar_t* stage = L"恢复辅助程序通信失败") {
    ipc::Reply reply;
    reply.error = error;
    reply.state = ipc::State::Unavailable;
    wchar_t system[384]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, system, DWORD(std::size(system)), nullptr);
    _snwprintf_s(reply.message, _TRUNCATE, L"%s（%lu）：%s", stage, error, system);
    return reply;
}
DWORD io(HANDLE pipe, void* data, DWORD bytes, bool writing, DWORD timeout) {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return GetLastError();
    OVERLAPPED async{};
    async.hEvent = event.value;
    DWORD transferred{};
    const BOOL complete = writing ? WriteFile(pipe, data, bytes, &transferred, &async) :
        ReadFile(pipe, data, bytes, &transferred, &async);
    if (!complete) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) return error;
        const DWORD wait = WaitForSingleObject(event.value, timeout);
        if (wait != WAIT_OBJECT_0) {
            const DWORD reason = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelIoEx(pipe, &async);
            // OVERLAPPED and buffers must survive cancellation completion.
            GetOverlappedResult(pipe, &async, &transferred, TRUE);
            return reason;
        }
        if (!GetOverlappedResult(pipe, &async, &transferred, FALSE)) return GetLastError();
    }
    return transferred == bytes ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
DWORD serverIdentity(HANDLE pipe, const std::wstring& helper, std::wstring& detail) {
    detail = L"读取辅助进程编号失败";
    ULONG pid{};
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return GetLastError();
    detail = L"打开辅助进程身份失败";
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.value) return GetLastError();
    DWORD count = 32768;
    std::wstring path(count, L'\0');
    detail = L"读取辅助进程路径失败";
    if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &count)) return GetLastError();
    path.resize(count);
    // Packaged desktop hosts can redirect LocalAppData. The process image then
    // reports the physical package path although CreateFile(helper) resolves to
    // the same verified file. Compare open-file identities, not path spellings.
    detail = L"打开辅助程序文件身份失败";
    Handle expected{CreateFileW(helper.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (expected.value == INVALID_HANDLE_VALUE) return GetLastError();
    Handle actual{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (actual.value == INVALID_HANDLE_VALUE) return GetLastError();
    FILE_ID_INFO expectedId{}, actualId{};
    if (!GetFileInformationByHandleEx(expected.value, FileIdInfo, &expectedId, sizeof(expectedId)) ||
        !GetFileInformationByHandleEx(actual.value, FileIdInfo, &actualId, sizeof(actualId))) return GetLastError();
    detail = L"辅助程序文件身份不匹配";
    return expectedId.VolumeSerialNumber == actualId.VolumeSerialNumber &&
        std::memcmp(&expectedId.FileId, &actualId.FileId, sizeof(expectedId.FileId)) == 0 ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
}

ipc::Reply RecoveryClient::call(ipc::Request request, DWORD timeoutMs) {
    lastReply_ = exchange(request, timeoutMs);
    return lastReply_;
}
ipc::Reply RecoveryClient::exchange(ipc::Request request, DWORD timeoutMs) {
    try {
        request.processId = GetCurrentProcessId();
        const auto pipeName = ipc::BrokerPipeName();
        Handle pipe;
        const ULONGLONG openDeadline = GetTickCount64() +
            (request.operation == ipc::Operation::Ping ? 0 : (std::min)(timeoutMs, DWORD(3000)));
        for (;;) {
            pipe.value = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe.value != INVALID_HANDLE_VALUE) break;
            const DWORD error = GetLastError();
            // A single-client pipe has a brief absent interval between replies.
            // WaitNamedPipe can also return FILE_NOT_FOUND during that interval.
            if ((error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_NOT_CONNECTED) ||
                GetTickCount64() >= openDeadline) return failure(error, L"连接恢复辅助程序失败");
            if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(pipeName.c_str(), 50);
            else Sleep(10);
        }
        std::wstring identityDetail;
        if (const DWORD error = serverIdentity(pipe.value, helper_, identityDetail)) return failure(error, identityDetail.c_str());
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr)) return failure(GetLastError());
        if (const DWORD error = io(pipe.value, &request, sizeof(request), true, timeoutMs)) return failure(error);
        ipc::Reply reply;
        if (const DWORD error = io(pipe.value, &reply, sizeof(reply), false, timeoutMs)) return failure(error);
        uint32_t acknowledged = ipc::ReplyAcknowledged;
        if (const DWORD error = io(pipe.value, &acknowledged, sizeof(acknowledged), true, 2000)) return failure(error);
        if (reply.magic != ipc::Magic || reply.version != ipc::Version || reply.hiddenCount > ipc::MaxWindows ||
            static_cast<uint32_t>(reply.state) > static_cast<uint32_t>(ipc::State::Stopped) ||
            reply.message[ipc::MessageChars - 1] != L'\0') return failure(ERROR_INVALID_DATA);
        return reply;
    } catch (const std::system_error& error) { return failure(DWORD(error.code().value())); }
    catch (...) { return failure(ERROR_GEN_FAILURE); }
}

DWORD RecoveryClient::connect(HINSTANCE module, std::span<const uint64_t> windows) {
    if (registered_) return ERROR_ALREADY_INITIALIZED;
    if (windows.size() > ipc::MaxWindows) return ERROR_INVALID_PARAMETER;
    try {
        helper_ = PrepareRuntimePayload(module);
        auto ping = operation(ipc::Operation::Ping);
        if (ping.error == ERROR_FILE_NOT_FOUND || ping.error == ERROR_PIPE_NOT_CONNECTED) {
            // Keep the verified executable immutable until CreateProcess opens it.
            Handle binary{CreateFileW(helper_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            if (binary.value == INVALID_HANDLE_VALUE) return GetLastError();
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            auto command = L"\"" + helper_ + L"\" --broker";
            if (!CreateProcessW(helper_.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return GetLastError();
            CloseHandle(process.hThread);
            Handle child{process.hProcess};
            const ULONGLONG deadline = GetTickCount64() + 5000;
            do {
                ping = operation(ipc::Operation::Ping);
                if (!ping.error || (ping.error != ERROR_FILE_NOT_FOUND && ping.error != ERROR_PIPE_NOT_CONNECTED &&
                    ping.error != ERROR_PIPE_BUSY)) break;
                // This worker-only wait is interruptible by helper startup failure.
                if (WaitForSingleObject(child.value, 50) == WAIT_OBJECT_0) {
                    // A competing host can win the singleton mutex; its pipe may
                    // appear shortly after the second process exits.
                    Sleep(50);
                }
            } while (GetTickCount64() < deadline);
        }
        if (ping.error) return ping.error;
        ipc::Request request;
        request.operation = ipc::Operation::Register;
        request.count = static_cast<uint32_t>(windows.size());
        std::copy(windows.begin(), windows.end(), request.windows);
        auto reply = call(request);
        const ULONGLONG deadline = GetTickCount64() + 10000;
        // A durable journal write can outlive the broker's short reply budget.
        // Register is idempotent for the same immutable whitelist. Do not lose
        // the lease merely because its first response says work is pending.
        while (reply.error == ERROR_IO_PENDING && GetTickCount64() < deadline) {
            // Even a timed-out caller must later Release this possible lease.
            registered_ = true;
            Sleep(50);
            reply = call(request);
        }
        if (reply.error) return reply.error;
        registered_ = true;
        return ERROR_SUCCESS;
    } catch (const std::system_error& error) {
        lastReply_ = failure(DWORD(error.code().value()), L"准备恢复辅助程序失败");
        return lastReply_.error;
    }
    catch (...) { return ERROR_GEN_FAILURE; }
}
ipc::Reply RecoveryClient::operation(ipc::Operation action, uint64_t window, DWORD timeoutMs) {
    ipc::Request request;
    request.operation = action;
    request.window = window;
    return call(request, timeoutMs);
}
ipc::Reply RecoveryClient::initialize() { return operation(ipc::Operation::Initialize); }
ipc::Reply RecoveryClient::query() { return operation(ipc::Operation::State); }
ipc::Reply RecoveryClient::hide(uint64_t window) { return operation(ipc::Operation::Hide, window, 30000); }
ipc::Reply RecoveryClient::show(uint64_t window) { return operation(ipc::Operation::Show, window); }
ipc::Reply RecoveryClient::release() {
    auto reply = operation(ipc::Operation::Release, 0, 120000);
    if ((!reply.error || (reply.error == ERROR_NOT_FOUND && reply.state == ipc::State::Stopped)) && !reply.remaining) {
        registered_ = false;
        reply.error = ERROR_SUCCESS;
    }
    return reply;
}
}

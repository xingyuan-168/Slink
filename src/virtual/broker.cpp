#include "broker.h"
#include "protocol.h"
#include "window_parking.h"
#include "recovery_prompt.h"

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace gwm::vd {
namespace {
constexpr DWORD kMaxJournalBytes = 8 * 1024 * 1024;
constexpr size_t kMaxHosts = 64;
constexpr DWORD kIoDeadlineMs = 2000;
constexpr DWORD kInstallerDeadlineMs = 120000;
constexpr uint32_t kJournalMagic = 0x4a4d5747;
constexpr uint32_t kJournalVersion = 1;

[[noreturn]] void fail(DWORD error) { throw std::system_error(static_cast<int>(error ? error : ERROR_GEN_FAILURE), std::system_category()); }
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : value_(handle) {}
    ~Handle() { if (*this) CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { Handle previous(std::exchange(value_, std::exchange(other.value_, nullptr))); }
        return *this;
    }
    Handle(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_{};
};
struct LocalMemory {
    void* value{};
    ~LocalMemory() { if (value) LocalFree(value); }
};
std::wstring sidText(PSID sid) {
    wchar_t* value{};
    if (!IsValidSid(sid) || !ConvertSidToStringSidW(sid, &value)) fail(ERROR_INVALID_SID);
    LocalMemory release{value}; return value;
}
std::wstring tokenSid(HANDLE token, bool logon) {
    DWORD bytes{};
    const auto kind = logon ? TokenLogonSid : TokenUser;
    GetTokenInformation(token, kind, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536) fail(ERROR_INVALID_DATA);
    std::vector<BYTE> data(bytes);
    if (!GetTokenInformation(token, kind, data.data(), bytes, &bytes)) fail(GetLastError());
    if (!logon) return sidText(reinterpret_cast<const TOKEN_USER*>(data.data())->User.Sid);
    const auto groups = reinterpret_cast<const TOKEN_GROUPS*>(data.data());
    if (groups->GroupCount != 1) fail(ERROR_INVALID_SID);
    return sidText(groups->Groups[0].Sid);
}
uint64_t processCreation(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) fail(GetLastError());
    return uint64_t(created.dwLowDateTime) | (uint64_t(created.dwHighDateTime) << 32);
}
std::wstring selfPath() {
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) fail(ERROR_INSUFFICIENT_BUFFER);
    path.resize(count); return path;
}
std::wstring packagePath() { return (std::filesystem::path(selfPath()).parent_path() / L"driver").wstring(); }

struct Peer {
    SessionIdentity identity;
    std::shared_ptr<Handle> process;
};
class Security {
public:
    std::wstring logon = ipc::CurrentLogonSid();
    std::wstring user;
    DWORD session{};
    Security() {
        HANDLE tokenValue{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenValue)) fail(GetLastError());
        Handle token(tokenValue); user = tokenSid(token.get(), false);
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) fail(GetLastError());
        const auto logonDacl = L"O:" + user + L"D:P(A;;GA;;;SY)(A;;GA;;;" + logon + L")";
        const auto userDacl = L"O:" + user + L"D:P(A;;GA;;;SY)(A;;GA;;;" + user + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(logonDacl.c_str(), SDDL_REVISION_1, &logonDescriptor_, nullptr) ||
            !ConvertStringSecurityDescriptorToSecurityDescriptorW(userDacl.c_str(), SDDL_REVISION_1, &userDescriptor_, nullptr)) fail(GetLastError());
    }
    ~Security() { if (logonDescriptor_) LocalFree(logonDescriptor_); if (userDescriptor_) LocalFree(userDescriptor_); }
    SECURITY_ATTRIBUTES attributes(bool currentUser = false) const {
        return {sizeof(SECURITY_ATTRIBUTES), currentUser ? userDescriptor_ : logonDescriptor_, FALSE};
    }
    Peer peer(DWORD pid) const {
        DWORD clientSession{};
        if (!pid || !ProcessIdToSessionId(pid, &clientSession) || clientSession != session) fail(ERROR_ACCESS_DENIED);
        auto process = std::make_shared<Handle>(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!*process || WaitForSingleObject(process->get(), 0) != WAIT_TIMEOUT) fail(ERROR_ACCESS_DENIED);
        HANDLE tokenValue{};
        if (!OpenProcessToken(process->get(), TOKEN_QUERY, &tokenValue)) fail(ERROR_ACCESS_DENIED);
        Handle token(tokenValue);
        if (tokenSid(token.get(), true) != logon || tokenSid(token.get(), false) != user) fail(ERROR_ACCESS_DENIED);
        return {{pid, processCreation(process->get())}, std::move(process)};
    }
    void validateFile(HANDLE handle, bool directory = false) const {
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle, &information)) fail(GetLastError());
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory ||
            (!directory && information.nNumberOfLinks != 1)) fail(ERROR_ACCESS_DENIED);
        PSID owner{}; PACL dacl{}; PSECURITY_DESCRIPTOR descriptor{};
        const DWORD error = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &dacl, nullptr, &descriptor);
        LocalMemory release{descriptor};
        if (error || !owner || !dacl || sidText(owner) != user) fail(ERROR_ACCESS_DENIED);
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
        if (!GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED)) fail(ERROR_ACCESS_DENIED);
        bool permitted{};
        for (DWORD index = 0; index < dacl->AceCount; ++index) {
            void* raw{}; if (!GetAce(dacl, index, &raw)) fail(ERROR_ACCESS_DENIED);
            const auto header = static_cast<const ACE_HEADER*>(raw);
            if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) fail(ERROR_ACCESS_DENIED);
            const auto ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            const auto identity = sidText(const_cast<DWORD*>(&ace->SidStart));
            if (identity != logon && identity != L"S-1-5-18") fail(ERROR_ACCESS_DENIED);
            if (identity == logon) permitted = true;
        }
        if (!permitted) fail(ERROR_ACCESS_DENIED);
    }
private:
    PSECURITY_DESCRIPTOR logonDescriptor_{};
    PSECURITY_DESCRIPTOR userDescriptor_{};
};

void verifyNoReparse(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) fail(GetLastError());
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) fail(ERROR_ACCESS_DENIED);
}
std::filesystem::path secureDirectory(const Security& security, const std::wstring& suffix) {
    const auto runtime = std::filesystem::path(ipc::RuntimeDirectory());
    auto current = runtime.root_path();
    auto attributes = security.attributes(true);
    for (const auto& element : runtime.relative_path()) {
        current /= element;
        if (!CreateDirectoryW(current.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) fail(GetLastError());
        verifyNoReparse(current);
    }
    current /= L"Recovery-" + security.logon + L"." + std::to_wstring(security.session) + suffix;
    attributes = security.attributes();
    if (!CreateDirectoryW(current.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) fail(GetLastError());
    Handle handle(CreateFileW(current.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle) fail(GetLastError());
    security.validateFile(handle.get(), true);
    return current;
}

struct LeaseData { SessionIdentity identity; std::vector<uint64_t> allowed; };
struct JournalData { std::vector<LeaseData> leases; std::vector<ParkingRecord> records; };
class Encoder {
public:
    std::vector<BYTE> bytes;
    void u32(uint32_t value) { for (int i = 0; i < 4; ++i) { bytes.push_back(BYTE(value)); value >>= 8; } }
    void u64(uint64_t value) { u32(uint32_t(value)); u32(uint32_t(value >> 32)); }
    void text(std::wstring_view value) { u32(static_cast<uint32_t>(value.size())); for (auto c : value) { bytes.push_back(BYTE(c)); bytes.push_back(BYTE(c >> 8)); } }
    void rect(const RECT& value) { u32(uint32_t(value.left)); u32(uint32_t(value.top)); u32(uint32_t(value.right)); u32(uint32_t(value.bottom)); }
    void point(const POINT& value) { u32(uint32_t(value.x)); u32(uint32_t(value.y)); }
    void session(const SessionIdentity& value) { u32(value.processId); u64(value.creationTime); }
};
class Decoder {
public:
    explicit Decoder(std::span<const BYTE> data) : bytes(data) {}
    uint32_t u32() { need(4); uint32_t value{}; for (int i = 0; i < 4; ++i) value |= uint32_t(bytes[offset++]) << (8 * i); return value; }
    uint64_t u64() { const uint64_t low = u32(); return low | (uint64_t(u32()) << 32); }
    std::wstring text(size_t maximum) {
        const auto length = u32(); if (length > maximum) fail(ERROR_INVALID_DATA); need(size_t(length) * 2);
        std::wstring value; value.reserve(length);
        for (uint32_t index = 0; index < length; ++index) { const auto low = bytes[offset++]; value.push_back(wchar_t(low | (uint16_t(bytes[offset++]) << 8))); }
        if (value.find(L'\0') != std::wstring::npos) fail(ERROR_INVALID_DATA);
        return value;
    }
    RECT rect() { RECT value{}; value.left = LONG(u32()); value.top = LONG(u32()); value.right = LONG(u32()); value.bottom = LONG(u32()); return value; }
    POINT point() { POINT value{}; value.x = LONG(u32()); value.y = LONG(u32()); return value; }
    SessionIdentity session() { SessionIdentity value; value.processId = u32(); value.creationTime = u64(); if (!value.processId || !value.creationTime) fail(ERROR_INVALID_DATA); return value; }
    bool finished() const { return offset == bytes.size(); }
private:
    std::span<const BYTE> bytes; size_t offset{};
    void need(size_t count) const { if (count > bytes.size() - offset) fail(ERROR_INVALID_DATA); }
};
std::vector<BYTE> encode(const Security& security, const JournalData& data) {
    Encoder out; out.u32(kJournalMagic); out.u32(kJournalVersion); out.text(security.logon); out.u32(security.session);
    out.u32(static_cast<uint32_t>(data.leases.size())); out.u32(static_cast<uint32_t>(data.records.size()));
    for (const auto& lease : data.leases) { out.session(lease.identity); out.u32(static_cast<uint32_t>(lease.allowed.size())); for (auto window : lease.allowed) out.u64(window); }
    for (const auto& record : data.records) {
        out.u32(record.version); out.session(record.session); out.u64(record.window);
        out.u32(record.sourceProcessId); out.u32(record.sourceThreadId); out.u64(record.sourceCreationTime);
        out.u32(record.windowClassAtom); out.u32(record.lifecycleCookie);
        out.u32(record.originalPlacement.flags); out.u32(record.originalPlacement.showCmd);
        out.point(record.originalPlacement.ptMinPosition); out.point(record.originalPlacement.ptMaxPosition);
        out.rect(record.originalPlacement.rcNormalPosition); out.rect(record.originalRect);
        out.rect(record.originalMonitorRect); out.rect(record.originalWorkRect); out.rect(record.virtualBounds);
        out.text(record.originalMonitorName); out.text(record.originalMonitorIdentity);
        out.text(record.virtualMonitorName); out.text(record.virtualMonitorIdentity);
        out.u32(record.originalDpi); out.u32(record.toolWindow ? 1u : 0u);
    }
    if (out.bytes.size() > kMaxJournalBytes) fail(ERROR_BUFFER_OVERFLOW);
    return out.bytes;
}
JournalData decode(const Security& security, std::span<const BYTE> bytes) {
    Decoder in(bytes);
    if (in.u32() != kJournalMagic || in.u32() != kJournalVersion || in.text(184) != security.logon || in.u32() != security.session) fail(ERROR_INVALID_DATA);
    const auto leaseCount = in.u32(), recordCount = in.u32();
    if (leaseCount > kMaxHosts || recordCount > kMaxHosts * ipc::MaxWindows) fail(ERROR_INVALID_DATA);
    JournalData data;
    for (uint32_t index = 0; index < leaseCount; ++index) {
        LeaseData lease; lease.identity = in.session();
        if (std::any_of(data.leases.begin(), data.leases.end(), [&](const auto& other) { return other.identity == lease.identity; })) fail(ERROR_INVALID_DATA);
        const auto count = in.u32(); if (count > ipc::MaxWindows) fail(ERROR_INVALID_DATA);
        for (uint32_t slot = 0; slot < count; ++slot) { const auto window = in.u64(); if (!window || window > UINTPTR_MAX) fail(ERROR_INVALID_DATA); lease.allowed.push_back(window); }
        data.leases.push_back(std::move(lease));
    }
    for (uint32_t index = 0; index < recordCount; ++index) {
        ParkingRecord record; record.version = in.u32(); record.session = in.session(); record.window = in.u64();
        record.sourceProcessId = in.u32(); record.sourceThreadId = in.u32(); record.sourceCreationTime = in.u64();
        record.windowClassAtom = in.u32(); record.lifecycleCookie = in.u32();
        record.originalPlacement.length = sizeof(WINDOWPLACEMENT);
        record.originalPlacement.flags = in.u32(); record.originalPlacement.showCmd = in.u32();
        record.originalPlacement.ptMinPosition = in.point(); record.originalPlacement.ptMaxPosition = in.point();
        record.originalPlacement.rcNormalPosition = in.rect(); record.originalRect = in.rect();
        record.originalMonitorRect = in.rect(); record.originalWorkRect = in.rect(); record.virtualBounds = in.rect();
        wcscpy_s(record.originalMonitorName, in.text(CCHDEVICENAME - 1).c_str()); wcscpy_s(record.originalMonitorIdentity, in.text(127).c_str());
        wcscpy_s(record.virtualMonitorName, in.text(CCHDEVICENAME - 1).c_str()); wcscpy_s(record.virtualMonitorIdentity, in.text(127).c_str());
        record.originalDpi = in.u32(); const auto tool = in.u32(); if (tool > 1) fail(ERROR_INVALID_DATA); record.toolWindow = tool != 0;
        const auto owner = std::find_if(data.leases.begin(), data.leases.end(), [&](const auto& lease) { return lease.identity == record.session; });
        if (owner == data.leases.end() || std::find(owner->allowed.begin(), owner->allowed.end(), record.window) == owner->allowed.end()) fail(ERROR_INVALID_DATA);
        data.records.push_back(record);
    }
    if (!in.finished()) fail(ERROR_INVALID_DATA);
    return data;
}

class Journal {
public:
    Journal(const Security& security, const std::wstring& suffix) : security_(security), directory_(secureDirectory(security, suffix)) {}
    JournalData load() const {
        const auto path = directory_ / L"recovery.bin";
        Handle file(CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file) { if (GetLastError() == ERROR_FILE_NOT_FOUND) return {}; fail(GetLastError()); }
        security_.validateFile(file.get());
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > kMaxJournalBytes) fail(ERROR_INVALID_DATA);
        std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart)); DWORD read{};
        if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) fail(ERROR_READ_FAULT);
        return decode(security_, bytes);
    }
    void write(const JournalData& data) {
        const auto bytes = encode(security_, data);
        const auto temp = directory_ / L"recovery.pending", path = directory_ / L"recovery.bin";
        auto attributes = security_.attributes();
        {
            Handle file(CreateFileW(temp.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, &attributes,
                OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
            if (!file) fail(GetLastError());
            security_.validateFile(file.get());
            LARGE_INTEGER zero{}; DWORD written{};
            if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN) || !SetEndOfFile(file.get()) ||
                !WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
                !FlushFileBuffers(file.get())) fail(GetLastError());
        }
        {
            Handle current(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (current) security_.validateFile(current.get());
            else if (GetLastError() != ERROR_FILE_NOT_FOUND) fail(GetLastError());
        }
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) fail(GetLastError());
    }
    void log(std::wstring_view message, DWORD error = 0) noexcept {
        try {
            std::scoped_lock lock(logMutex_);
            auto attributes = security_.attributes();
            const auto path = directory_ / L"recovery.log";
            Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ,
                &attributes, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file) return;
            security_.validateFile(file.get());
            LARGE_INTEGER size{}; if (!GetFileSizeEx(file.get(), &size) || size.QuadPart > 1024 * 1024) return;
            const std::wstring line = std::to_wstring(GetTickCount64()) + L" code=" + std::to_wstring(error) + L" " + std::wstring(message) + L"\r\n";
            const int length = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
            std::string encoded(static_cast<size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), encoded.data(), length, nullptr, nullptr);
            DWORD written{}; WriteFile(file.get(), encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr);
        } catch (...) {}
    }
private:
    const Security& security_;
    std::filesystem::path directory_;
    std::mutex logMutex_;
};

bool completeIo(HANDLE pipe, OVERLAPPED& operation, DWORD& bytes, DWORD timeout = kIoDeadlineMs) {
    const DWORD wait = WaitForSingleObject(operation.hEvent, timeout);
    if (wait == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
    CancelIoEx(pipe, &operation);
    // The operation owns stack buffers: drain cancellation before returning.
    GetOverlappedResult(pipe, &operation, &bytes, TRUE);
    SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED);
    return false;
}
template<class Data> bool transfer(HANDLE pipe, Data& value, bool write) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event) return false;
    OVERLAPPED operation{}; operation.hEvent = event.get(); DWORD transferred{};
    const BOOL started = write ? WriteFile(pipe, &value, sizeof(value), &transferred, &operation) : ReadFile(pipe, &value, sizeof(value), &transferred, &operation);
    if (!started && (GetLastError() != ERROR_IO_PENDING || !completeIo(pipe, operation, transferred))) return false;
    return transferred == sizeof(value);
}
void assignMessage(ipc::Reply& reply, const std::wstring& message) { wcsncpy_s(reply.message, message.c_str(), _TRUNCATE); }
DWORD errorFor(const ParkingResult& value) {
    if (value) return ERROR_SUCCESS;
    if (value.win32Error) return value.win32Error;
    switch (value.code) {
    case ParkingCode::ownedByAnotherSession: return ERROR_BUSY;
    case ParkingCode::targetTooSmall: return ERROR_INSUFFICIENT_BUFFER;
    case ParkingCode::invalidArgument: return ERROR_INVALID_PARAMETER;
    case ParkingCode::invalidSession: return ERROR_INVALID_HANDLE;
    case ParkingCode::accessDenied: return ERROR_ACCESS_DENIED;
    default: return ERROR_GEN_FAILURE;
    }
}

class Broker {
public:
    explicit Broker(BrokerOptions options) : options_(std::move(options)), security_(),
        journal_(security_, suffix()), parking_({1500, [this](std::span<const ParkingRecord> records) { return persist(records); }}) {
        load();
    }
    DWORD run() {
        auto attributes = security_.attributes();
        Handle mutex(CreateMutexW(&attributes, FALSE, (ipc::BrokerMutexName() + suffix()).c_str()));
        if (!mutex) { const DWORD error = GetLastError(); journal_.log(L"broker mutex creation failed", error); return error; }
        if (GetLastError() == ERROR_ALREADY_EXISTS) return ERROR_SUCCESS;
        worker_ = std::thread([this] { work(); });
        DWORD finalError{};
        const auto name = ipc::BrokerPipeName() + suffix();
        while (!exitReady_) {
            Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1, sizeof(ipc::Reply), sizeof(ipc::Request), kIoDeadlineMs, &attributes));
            if (!pipe) { finalError = GetLastError(); stopRequested_ = true; break; }
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event) { finalError = GetLastError(); stopRequested_ = true; break; }
            OVERLAPPED connected{}; connected.hEvent = event.get();
            BOOL immediate = ConnectNamedPipe(pipe.get(), &connected); const DWORD connectError = immediate ? ERROR_SUCCESS : GetLastError();
            if (immediate || connectError == ERROR_PIPE_CONNECTED) SetEvent(event.get());
            else if (connectError != ERROR_IO_PENDING) { finalError = connectError; stopRequested_ = true; break; }
            if (options_.readyEvent) SetEvent(options_.readyEvent);
            while (WaitForSingleObject(event.get(), 100) == WAIT_TIMEOUT && !exitReady_) {
                if (options_.stopEvent && WaitForSingleObject(options_.stopEvent, 0) == WAIT_OBJECT_0) stopRequested_ = true;
            }
            if (exitReady_) { DWORD ignored{}; CancelIoEx(pipe.get(), &connected); GetOverlappedResult(pipe.get(), &connected, &ignored, TRUE); break; }
            ipc::Request request; ipc::Reply reply; std::optional<Peer> peer;
            ULONG clientPid{};
            try {
                if (!GetNamedPipeClientProcessId(pipe.get(), &clientPid) || !transfer(pipe.get(), request, false)) fail(ERROR_INVALID_DATA);
                if (request.magic != ipc::Magic || request.version != ipc::Version || request.processId != clientPid ||
                    request.count > ipc::MaxWindows || uint32_t(request.operation) > uint32_t(ipc::Operation::Release) ||
                    (request.operation!=ipc::Operation::Ping&&request.operation!=ipc::Operation::State&&!request.requestId)) fail(ERROR_INVALID_DATA);
                peer = security_.peer(clientPid);
                if (request.operation == ipc::Operation::Ping || request.operation == ipc::Operation::State) reply = state(peer->identity, request.operation == ipc::Operation::State);
                else reply = submit(*peer, request);
            } catch (const std::system_error& error) { reply.error = DWORD(error.code().value()); reply.state = ipc::State::Unavailable; assignMessage(reply, L"恢复服务拒绝了无效或未授权的请求"); journal_.log(L"request identity or format rejected", reply.error); }
            catch (...) { reply.error = ERROR_UNHANDLED_EXCEPTION; reply.state = ipc::State::Unavailable; }
            // DisconnectNamedPipe discards unread data. Wait for a bounded client
            // acknowledgement rather than using an unbounded FlushFileBuffers.
            uint32_t acknowledged{};
            const bool delivered = transfer(pipe.get(), reply, true) && transfer(pipe.get(), acknowledged, false) &&
                acknowledged == ipc::ReplyAcknowledged;
            if (delivered && peer && request.operation == ipc::Operation::Release && reply.error == 0 && reply.remaining == 0) {
                std::scoped_lock lock(stateMutex_); emptySince_ = GetTickCount64();
            }
            DisconnectNamedPipe(pipe.get());
        }
        if (finalError) {
            journal_.log(L"broker pipe loop failed; restoring sessions before exit", finalError);
            // Keep the controller alive long enough to restore owned windows even
            // if transport creation fails. No source state is discarded here.
            while (!exitReady_) Sleep(50);
        }
        quitWorker_ = true; queueChanged_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (initializer_.joinable()) initializer_.join();
        return finalError;
    }
private:
    struct Lease {
        LeaseData data; std::shared_ptr<Handle> process;
        bool closing{}; DWORD error{}; ParkingCode parkingCode{}; std::wstring message;
    };
    struct Job { Peer peer; ipc::Request request; std::promise<ipc::Reply> promise; std::shared_future<ipc::Reply> completion; };
    BrokerOptions options_;
    Security security_;
    Journal journal_;
    WindowParking parking_;
    std::mutex stateMutex_, queueMutex_;
    std::condition_variable queueChanged_;
    std::vector<Lease> leases_;
    std::vector<ParkingRecord> cachedRecords_;
    std::vector<SessionIdentity> completed_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::vector<std::shared_ptr<Job>> outstanding_;
    std::deque<std::shared_ptr<Job>> completedJobs_;
    std::thread worker_, initializer_;
    std::atomic<bool> initRunning_{}, quitWorker_{}, exitReady_{}, stopRequested_{};
    std::atomic<bool> retryRequested_{};
    RecoveryPrompt recoveryPrompt_{[this]{retryRequested_=true;queueChanged_.notify_one();}};
    ipc::State globalState_{ipc::State::Checking};
    DisplayResult display_{};
    DWORD startupError_{};
    bool everRegistered_{}, displayTouched_{}, detached_{};
    ULONGLONG emptySince_{GetTickCount64()}, initStarted_{}, lastRecovery_{}, lastInspection_{};

    std::wstring suffix() const {
        if (options_.instanceSuffix.empty()) return {};
        if (options_.instanceSuffix.size() > 64 || options_.instanceSuffix.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") != std::wstring::npos)
            fail(ERROR_INVALID_PARAMETER);
        return L"." + options_.instanceSuffix;
    }
    auto findLease(const SessionIdentity& identity) { return std::find_if(leases_.begin(), leases_.end(), [&](const Lease& lease) { return lease.data.identity == identity; }); }
    bool hasLeases() { std::scoped_lock lock(stateMutex_); return !leases_.empty(); }
    bool hasRecords() { std::scoped_lock lock(stateMutex_); return !cachedRecords_.empty(); }
    DisplayResult inspect() { return options_.inspect ? options_.inspect() : InspectOwnedDisplay(); }
    DisplayResult ensure(int width, int height, const PhaseCallback& callback) { return options_.ensure ? options_.ensure(width, height, callback) : EnsureExtendedDisplay(width, height, callback); }
    DisplayResult detach() { return options_.detach ? options_.detach() : DetachOwnedDisplay(); }
    void setDisplay(const DisplayResult& value, std::optional<ipc::State> stateOverride = {}) {
        std::scoped_lock lock(stateMutex_); display_ = value;
        globalState_ = stateOverride.value_or(value.rebootRequired ? ipc::State::RestartRequired :
            value.code == ERROR_ALREADY_EXISTS ? ipc::State::Conflict : value && value.active ? ipc::State::Ready : ipc::State::Unavailable);
    }
    void phase(Phase value, const std::wstring& message) {
        std::scoped_lock lock(stateMutex_); display_.message = message; display_.code = ERROR_SUCCESS;
        globalState_ = value == Phase::Installing ? ipc::State::Installing :
            value == Phase::Initializing ? ipc::State::Initializing : ipc::State::Checking;
    }
    JournalData journalData(std::span<const ParkingRecord> records, const SessionIdentity* omit = nullptr) {
        JournalData data; data.records.assign(records.begin(), records.end());
        std::scoped_lock lock(stateMutex_);
        for (const auto& lease : leases_) if (!omit || !(lease.data.identity == *omit)) data.leases.push_back(lease.data);
        return data;
    }
    bool persist(std::span<const ParkingRecord> records) {
        try { journal_.write(journalData(records)); return true; }
        catch (const std::system_error& error) { journal_.log(L"journal write failed; recovery record retained", DWORD(error.code().value())); return false; }
        catch (...) { journal_.log(L"journal write failed", ERROR_UNHANDLED_EXCEPTION); return false; }
    }
    void refreshRecords() { auto records = parking_.snapshot(); std::scoped_lock lock(stateMutex_); cachedRecords_ = std::move(records); }
    void load() {
        try {
            auto data = journal_.load();
            const auto imported = parking_.importRecords(data.records);
            if (!imported) fail(ERROR_INVALID_DATA);
            for (auto& saved : data.leases) {
                Lease lease; lease.data = std::move(saved);
                try {
                    auto peer = security_.peer(lease.data.identity.processId);
                    if (peer.identity == lease.data.identity) {
                        SessionIdentity registered; const auto result = parking_.registerSession(peer.process->get(), registered);
                        if (result && registered == peer.identity) lease.process = std::move(peer.process);
                    }
                } catch (...) {}
                leases_.push_back(std::move(lease));
            }
            cachedRecords_ = std::move(data.records); everRegistered_ = !leases_.empty();
            if (!cachedRecords_.empty()) { displayTouched_ = true; setDisplay(inspect()); }
            journal_.log(L"recovery broker started");
        } catch (const std::system_error& error) {
            startupError_ = DWORD(error.code().value()); display_.code = startupError_;
            display_.message = L"恢复记录无法通过权限或内容校验，已禁止新的窗口停放"; globalState_ = ipc::State::Unavailable;
        }
    }
    ipc::Reply state(const SessionIdentity& identity, bool requireLease = false) {
        std::scoped_lock lock(stateMutex_); ipc::Reply reply;
        reply.state = globalState_; reply.statusError = display_.code; reply.installed = display_.installed; reply.active = display_.active;
        reply.left = display_.bounds.left; reply.top = display_.bounds.top; reply.right = display_.bounds.right; reply.bottom = display_.bounds.bottom;
        assignMessage(reply, display_.message);
        const auto lease = findLease(identity);
        if (lease != leases_.end()) {
            if (lease->closing) reply.state = lease->error ? ipc::State::RestoreFailed : ipc::State::Restoring;
            if (lease->error) reply.error = lease->error;
            if (!lease->message.empty()) assignMessage(reply, lease->message);
            reply.parkingCode = uint32_t(lease->parkingCode);
            for (const auto& record : cachedRecords_) if (record.session == identity) {
                ++reply.remaining;
                if (reply.hiddenCount < ipc::MaxWindows) reply.hidden[reply.hiddenCount++] = record.window;
            }
        } else if (std::find(completed_.begin(), completed_.end(), identity) != completed_.end()) {
            reply = {}; reply.state = ipc::State::Stopped; assignMessage(reply, L"本次窗口已恢复");
        } else if (requireLease) reply.error = ERROR_NOT_FOUND;
        if (startupError_) reply.error = startupError_;
        return reply;
    }
    ipc::Reply submit(const Peer& peer, const ipc::Request& request) {
        std::shared_ptr<Job> job;
        {
            std::scoped_lock lock(queueMutex_);
            const auto completed=std::find_if(completedJobs_.begin(),completedJobs_.end(),[&](const auto& entry){
                return entry->peer.identity==peer.identity&&entry->request.requestId==request.requestId;
            });
            if(completed!=completedJobs_.end()){
                if(std::memcmp(&(*completed)->request,&request,sizeof(request))!=0)fail(ERROR_INVALID_DATA);
                return (*completed)->completion.get();
            }
            auto existing = std::find_if(outstanding_.begin(), outstanding_.end(), [&](const auto& entry) {
                return entry->peer.identity == peer.identity && std::memcmp(&entry->request, &request, sizeof(request)) == 0;
            });
            if (existing != outstanding_.end()) job = *existing;
            else {
                if (outstanding_.size() >= 128) { auto reply = state(peer.identity); reply.error = ERROR_BUSY; return reply; }
                job = std::make_shared<Job>(); job->peer = peer; job->request = request; job->completion = job->promise.get_future().share();
                queue_.push_back(job); outstanding_.push_back(job); exitReady_ = false;
            }
        }
        queueChanged_.notify_one();
        if (job->completion.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready) return job->completion.get();
        auto reply = state(peer.identity); reply.error = ERROR_IO_PENDING;
        if (request.operation == ipc::Operation::Release) { reply.state = ipc::State::Restoring; assignMessage(reply, L"正在恢复本次隐藏的窗口"); }
        return reply;
    }
    void leaseResult(const SessionIdentity& identity, DWORD error, const std::wstring& message, ParkingCode code = ParkingCode::ok) {
        std::scoped_lock lock(stateMutex_); const auto lease = findLease(identity);
        if (lease != leases_.end()) { lease->error = error; lease->message = message; lease->parkingCode = code; }
    }
    ipc::Reply execute(const Peer& peer, const ipc::Request& request) {
        if (startupError_) return state(peer.identity);
        if (request.operation == ipc::Operation::Register) return registerPeer(peer, request);
        {
            std::scoped_lock lock(stateMutex_);
            if (findLease(peer.identity) == leases_.end()) {
                ipc::Reply reply; reply.state = ipc::State::Stopped;
                reply.error = request.operation == ipc::Operation::Release && std::find(completed_.begin(), completed_.end(), peer.identity) != completed_.end() ? 0 : ERROR_NOT_FOUND;
                return reply;
            }
        }
        if (request.operation == ipc::Operation::Release) release(peer.identity);
        else if (request.operation == ipc::Operation::Initialize) { leaseResult(peer.identity, 0, L""); initialize(); }
        else if (request.operation == ipc::Operation::Hide || request.operation == ipc::Operation::Show) windowOperation(peer.identity, request);
        return state(peer.identity);
    }
    ipc::Reply registerPeer(const Peer& peer, const ipc::Request& request) {
        std::vector<uint64_t> allowed;
        for (uint32_t index = 0; index < request.count; ++index) {
            const auto window = request.windows[index];
            if (window > UINTPTR_MAX) fail(ERROR_INVALID_PARAMETER);
            if (window && std::find(allowed.begin(), allowed.end(), window) == allowed.end()) allowed.push_back(window);
        }
        {
            std::scoped_lock lock(stateMutex_); const auto existing = findLease(peer.identity);
            if (existing != leases_.end()) {
                ipc::Reply reply; reply.error = existing->data.allowed == allowed ? 0 : ERROR_ALREADY_EXISTS;
                reply.state = globalState_; assignMessage(reply, reply.error ? L"宿主已登记不同的启动窗口配置" : L"宿主会话已登记"); return reply;
            }
            if (leases_.size() >= kMaxHosts) fail(ERROR_TOO_MANY_SESS);
        }
        SessionIdentity registered;
        const auto result = parking_.registerSession(peer.process->get(), registered);
        if (!result || !(registered == peer.identity)) fail(ERROR_ACCESS_DENIED);
        {
            std::scoped_lock lock(stateMutex_); leases_.push_back({{peer.identity, std::move(allowed)}, peer.process});
            completed_.erase(std::remove(completed_.begin(), completed_.end(), peer.identity), completed_.end());
            everRegistered_ = true; detached_ = false; emptySince_ = 0;
        }
        if (!persist(parking_.snapshot())) {
            { std::scoped_lock lock(stateMutex_); const auto lease = findLease(peer.identity); leases_.erase(lease); }
            parking_.unregisterSession(peer.identity); fail(ERROR_WRITE_FAULT);
        }
        return state(peer.identity);
    }
    void release(const SessionIdentity& identity) {
        {
            std::scoped_lock lock(stateMutex_); const auto lease = findLease(identity); if (lease == leases_.end()) return;
            lease->closing = true; lease->error = 0; lease->message = L"正在恢复本次隐藏的窗口";
        }
        const auto report = parking_.restoreSession(identity); refreshRecords();
        if (!report.complete()) {
            DWORD error = ERROR_BUSY; ParkingCode code = ParkingCode::restoreFailed;
            for (const auto& result : report.results) if (!result) { error = errorFor(result); code = result.code; break; }
            leaseResult(identity, error, L"窗口恢复尚未完成，恢复记录已保留并会重试", code); return;
        }
        try { journal_.write(journalData(parking_.snapshot(), &identity)); }
        catch (const std::system_error& error) { leaseResult(identity, DWORD(error.code().value()), L"窗口已回原位，但恢复记录清理失败，将继续重试"); return; }
        parking_.unregisterSession(identity);
        {
            std::scoped_lock lock(stateMutex_); const auto lease = findLease(identity); if (lease != leases_.end()) leases_.erase(lease);
            completed_.push_back(identity); if (completed_.size() > 128) completed_.erase(completed_.begin());
            if (leases_.empty()) emptySince_ = GetTickCount64();
        }
    }
    DisplayResult install(const PhaseCallback& callback) {
        if (options_.install) return options_.install(callback);
        const auto verified = VerifyDriverPackage(packagePath()); if (!verified) return verified;
        callback(Phase::Installing, L"等待 Windows 管理员授权并安装虚拟显示驱动");
        const auto executable = selfPath();
        SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas"; info.lpFile = executable.c_str(); info.lpParameters = L"--install"; info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) return {GetLastError(), L"管理员授权取消或无法启动安装程序"};
        Handle process(info.hProcess); if (!process) return {ERROR_GEN_FAILURE, L"未取得安装程序进程句柄"};
        // State polling remains available. The 120-second status deadline is
        // enforced by maintain(); completion is still observed after a timeout
        // so a late installer cannot leave an active display without a lease.
        while (WaitForSingleObject(process.get(), 200) == WAIT_TIMEOUT) {}
        DWORD code{}; if (!GetExitCodeProcess(process.get(), &code)) code = GetLastError();
        DisplayResult result; result.code = code; result.installed = code == 0 || code == ERROR_SUCCESS_REBOOT_REQUIRED;
        result.rebootRequired = code == ERROR_SUCCESS_REBOOT_REQUIRED || code == ERROR_SUCCESS_REBOOT_INITIATED;
        result.message = result.rebootRequired ? L"驱动安装完成，需要重启 Windows 后再重试" :
            code == ERROR_CANCELLED ? L"已取消管理员授权，普通映射继续可用" : code ? L"驱动安装失败，请查看恢复日志" : L"驱动安装完成";
        return result;
    }
    void initialize() {
        if (initRunning_.exchange(true)) return;
        if (initializer_.joinable()) initializer_.join();
        { std::scoped_lock lock(stateMutex_); initStarted_ = GetTickCount64(); globalState_ = ipc::State::Checking; display_.code = 0; display_.message = L"正在检查虚拟显示器"; displayTouched_ = true; detached_ = false; }
        initializer_ = std::thread([this] {
            const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            try {
                const auto callback = [this](Phase value, const std::wstring& message) { phase(value, message); };
                auto value = inspect();
                const bool newInstall = !value.installed && value.code == ERROR_NOT_FOUND;
                if (value.code == ERROR_ALREADY_EXISTS) setDisplay(value);
                else {
                    if (!value || !value.installed) value = install(callback);
                    if (!value || value.rebootRequired) setDisplay(value);
                    else {
                        if (newInstall && !hasRecords()) {
                            value = detach();
                            if (!value && value.code != ERROR_NOT_FOUND) { setDisplay(value); initRunning_ = false; if (previous) SetThreadDpiAwarenessContext(previous); return; }
                        }
                        if (hasLeases()) value = ensure(1920, 1080, callback);
                        else value = inspect();
                        setDisplay(value);
                    }
                }
            } catch (const std::system_error& error) { setDisplay({DWORD(error.code().value()), L"虚拟显示器初始化失败"}); }
            catch (...) { setDisplay({ERROR_UNHANDLED_EXCEPTION, L"虚拟显示器初始化失败"}); }
            if (previous) SetThreadDpiAwarenessContext(previous);
            initRunning_ = false; queueChanged_.notify_all();
        });
    }
    void windowOperation(const SessionIdentity& identity, const ipc::Request& request) {
        {
            std::scoped_lock lock(stateMutex_); const auto lease = findLease(identity);
            if (lease == leases_.end()) fail(ERROR_NOT_FOUND);
            if (!request.window || request.window > UINTPTR_MAX || std::find(lease->data.allowed.begin(), lease->data.allowed.end(), request.window) == lease->data.allowed.end()) fail(ERROR_ACCESS_DENIED);
            if (lease->closing) fail(ERROR_BUSY);
        }
        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.window));
        ParkingResult result;
        if (request.operation == ipc::Operation::Show) result = parking_.show(identity, window);
        else {
            RECT bounds{};
            { std::scoped_lock lock(stateMutex_); if (initRunning_ || globalState_ != ipc::State::Ready || !display_.active) fail(ERROR_NOT_READY); bounds = display_.bounds; }
            RECT source{}; if (!GetWindowRect(window, &source)) fail(ERROR_INVALID_WINDOW_HANDLE);
            if (IsIconic(window) || IsZoomed(window)) { WINDOWPLACEMENT placement{}; placement.length = sizeof(placement); if (!GetWindowPlacement(window, &placement)) fail(GetLastError()); source = placement.rcNormalPosition; }
            const auto width = int64_t(source.right) - source.left, height = int64_t(source.bottom) - source.top;
            if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX) fail(ERROR_INVALID_PARAMETER);
            if (width > int64_t(bounds.right) - bounds.left || height > int64_t(bounds.bottom) - bounds.top) {
                const auto enlarged = ensure(std::max(1920, int(width)), std::max(1080, int(height)), [this](Phase phaseValue, const std::wstring& message) { phase(phaseValue, message); });
                setDisplay(enlarged); if (!enlarged || !enlarged.active) fail(enlarged.code ? enlarged.code : ERROR_NOT_READY); bounds = enlarged.bounds;
            }
            result = parking_.hide(identity, window, bounds);
        }
        refreshRecords(); leaseResult(identity, errorFor(result), result ? L"" : parkingCodeText(result.code), result.code);
    }
    void maintain() {
        const auto now = GetTickCount64();
        if (initRunning_) {
            std::scoped_lock lock(stateMutex_);
            if (now - initStarted_ >= kInstallerDeadlineMs) { globalState_ = ipc::State::Unavailable; display_.code = ERROR_TIMEOUT; display_.message = L"初始化等待超过 120 秒；可关闭画布，后台仍会观察安装结束并安全收尾"; }
        }
        if (retryRequested_.exchange(false) || now - lastRecovery_ >= 2000 || stopRequested_) {
            lastRecovery_ = now; std::vector<SessionIdentity> restore;
            { std::scoped_lock lock(stateMutex_); for (const auto& lease : leases_) if (stopRequested_ || lease.closing || !lease.process || WaitForSingleObject(lease.process->get(), 0) != WAIT_TIMEOUT) restore.push_back(lease.data.identity); }
            for (const auto& identity : restore) release(identity);
        }
        bool inspectNow{};
        { std::scoped_lock lock(stateMutex_); inspectNow = !initRunning_ && globalState_ == ipc::State::Ready && now - lastInspection_ >= 1500; }
        if (inspectNow) {
            lastInspection_ = now; const auto value = inspect();
            if (!value || !value.active) {
                setDisplay(value, ipc::State::Restoring);
                const auto restored = parking_.restoreAll(); refreshRecords();
                auto failure = value; failure.code = value.code ? value.code : ERROR_DEVICE_NOT_AVAILABLE;
                failure.message = restored.complete() ? L"虚拟屏已失效，窗口已恢复；请重试初始化" : L"虚拟屏已失效，部分窗口仍待恢复";
                setDisplay(failure, restored.complete() ? ipc::State::Unavailable : ipc::State::RestoreFailed);
            }
        }
        bool retryLost{};
        { std::scoped_lock lock(stateMutex_); retryLost = !initRunning_ && globalState_ == ipc::State::RestoreFailed && !cachedRecords_.empty() && now - lastInspection_ >= 2000; }
        if (retryLost) { lastInspection_ = now; const auto report = parking_.restoreAll(); refreshRecords(); if (report.complete()) { std::scoped_lock lock(stateMutex_); globalState_ = ipc::State::Unavailable; display_.message = L"窗口已恢复，请重试初始化"; } }
        unsigned orphaned{};DWORD recoveryError{};std::wstring recoveryMessage;
        {
            std::scoped_lock lock(stateMutex_);
            for(const auto& lease:leases_)if(lease.error&&(!lease.process||WaitForSingleObject(lease.process->get(),0)!=WAIT_TIMEOUT)){
                for(const auto& record:cachedRecords_)if(record.session==lease.data.identity)++orphaned;
                recoveryError=lease.error;recoveryMessage=lease.message;
            }
        }
        recoveryPrompt_.update(orphaned,recoveryError,recoveryMessage);
        bool empty{}, touched{}, expired{};
        {
            std::scoped_lock lock(stateMutex_); empty = leases_.empty() && cachedRecords_.empty(); touched = displayTouched_;
            expired = empty && !initRunning_ && (everRegistered_ ? emptySince_ && now - emptySince_ >= options_.finalReplyGraceMs : now - emptySince_ >= options_.startupIdleMs);
        }
        if (empty && !initRunning_ && touched && !detached_ && now - lastInspection_ >= 1000) {
            lastInspection_ = now; const auto value = detach();
            if (value || value.code == ERROR_NOT_FOUND) { std::scoped_lock lock(stateMutex_); detached_ = true; display_ = value; globalState_ = ipc::State::Stopped; }
            else { journal_.log(L"last-session display detach failed; retry retained", value.code); setDisplay(value); }
        }
        if (expired && (!touched || detached_)) {
            std::scoped_lock lock(queueMutex_); if (queue_.empty() && outstanding_.empty()) exitReady_ = true;
        }
    }
    void work() {
        const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        while (!quitWorker_) {
            std::shared_ptr<Job> job;
            { std::unique_lock lock(queueMutex_); queueChanged_.wait_for(lock, std::chrono::milliseconds(100), [&] { return quitWorker_ || !queue_.empty(); }); if (!queue_.empty()) { job = queue_.front(); queue_.pop_front(); } }
            if (job) {
                ipc::Reply reply;
                if(options_.testOperationDelayMs)Sleep(options_.testOperationDelayMs);
                try { reply = execute(job->peer, job->request); }
                catch (const std::system_error& error) { leaseResult(job->peer.identity, DWORD(error.code().value()), L"恢复操作未完成，原记录保持可重试"); reply = state(job->peer.identity); reply.error = DWORD(error.code().value()); journal_.log(L"operation failed", reply.error); }
                catch (...) { reply = state(job->peer.identity); reply.error = ERROR_UNHANDLED_EXCEPTION; }
                job->promise.set_value(reply);
                { std::scoped_lock lock(queueMutex_); completedJobs_.push_back(job);if(completedJobs_.size()>256)completedJobs_.pop_front();outstanding_.erase(std::remove(outstanding_.begin(), outstanding_.end(), job), outstanding_.end()); }
            }
            try { maintain(); } catch (const std::system_error& error) { journal_.log(L"recovery maintenance failed", DWORD(error.code().value())); }
            catch (...) { journal_.log(L"recovery maintenance failed", ERROR_UNHANDLED_EXCEPTION); }
        }
        if (previous) SetThreadDpiAwarenessContext(previous);
    }
};
} // namespace

DWORD RunRecoveryBroker(BrokerOptions options) { Broker broker(std::move(options)); return broker.run(); }
DWORD RunDriverInstaller() {
    const auto result = InstallOwnedDriver(packagePath());
    return result.rebootRequired ? ERROR_SUCCESS_REBOOT_REQUIRED : result.code;
}
} // namespace gwm::vd

#include "protocol.h"
#include <sddl.h>
#include <shlobj.h>
#include <vector>
#include <system_error>

namespace gwm::vd::ipc {
namespace {
[[noreturn]] void fail(DWORD code) { throw std::system_error(int(code), std::system_category()); }
std::wstring identity() {
    DWORD session{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) fail(GetLastError());
    return CurrentLogonSid() + L"." + std::to_wstring(session);
}
}
std::wstring CurrentLogonSid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) fail(GetLastError());
    struct TokenExit { HANDLE h; ~TokenExit() { CloseHandle(h); } } tokenExit{token};
    DWORD bytes{};
    GetTokenInformation(token, TokenLogonSid, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536) fail(GetLastError());
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token, TokenLogonSid, buffer.data(), bytes, &bytes)) fail(GetLastError());
    const auto groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
    if (groups->GroupCount != 1 || !IsValidSid(groups->Groups[0].Sid)) fail(ERROR_INVALID_SID);
    wchar_t* text{};
    if (!ConvertSidToStringSidW(groups->Groups[0].Sid, &text)) fail(GetLastError());
    std::wstring result(text);
    LocalFree(text);
    return result;
}
std::wstring BrokerPipeName() { return L"\\\\.\\pipe\\GameWindowMapper.Recovery.v1." + identity(); }
std::wstring BrokerMutexName() { return L"Local\\GameWindowMapper.Recovery.v1." + identity(); }
std::wstring RuntimeDirectory() {
    wchar_t* path{};
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &path);
    if (FAILED(hr)) fail(DWORD(hr));
    std::wstring result(path);
    CoTaskMemFree(path);
    return result + L"\\GameWindowMapper\\Runtime\\v1";
}
}

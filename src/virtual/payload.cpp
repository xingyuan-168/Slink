#include "payload.h"
#include "payload_hashes.h"

#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace gwm::vd {
namespace {
[[noreturn]] void Fail(DWORD code, const char* operation) {
    throw std::system_error(static_cast<int>(code ? code : ERROR_GEN_FAILURE), std::system_category(), operation);
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    bool valid() const noexcept { return value && value != INVALID_HANDLE_VALUE; }
};
struct LocalMemory {
    void* value{};
    ~LocalMemory() { if (value) LocalFree(value); }
};
struct UserSecurity {
    std::wstring sid;
    LocalMemory descriptor;
    SECURITY_ATTRIBUTES attributes{};
    UserSecurity() {
        Handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) Fail(GetLastError(), "OpenProcessToken");
        DWORD bytes{};
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
        if (!bytes) Fail(GetLastError(), "GetTokenInformation size");
        std::vector<BYTE> buffer(bytes);
        if (!GetTokenInformation(token.value, TokenUser, buffer.data(), bytes, &bytes)) Fail(GetLastError(), "GetTokenInformation");
        wchar_t* text{};
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) Fail(GetLastError(), "ConvertSidToStringSid");
        sid = text;
        LocalFree(text);
        const std::wstring sddl = L"O:" + sid + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor.value, nullptr))
            Fail(GetLastError(), "Build runtime security descriptor");
        attributes = {sizeof(attributes), descriptor.value, FALSE};
    }
};
struct RuntimeLock {
    Handle handle;
    bool acquired{};
    explicit RuntimeLock(UserSecurity& security) {
        const std::wstring name = L"Global\\GameWindowMapper.Payload.v1." + security.sid;
        handle.value = CreateMutexExW(&security.attributes, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
        if (!handle.valid()) Fail(GetLastError(), "Create runtime extraction mutex");
        const DWORD wait = WaitForSingleObject(handle.value, 30000);
        acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!acquired) Fail(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError(), "Wait for runtime extraction");
    }
    ~RuntimeLock() { if (acquired) ReleaseMutex(handle.value); }
};
void CheckObject(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) Fail(GetLastError(), "Read runtime object attributes");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1))
        Fail(ERROR_ACCESS_DENIED, "Runtime path is a reparse point, hard link, or wrong object type");
}
void RestrictDirectory(HANDLE directory, const UserSecurity& security) {
    PSID owner{};
    LocalMemory descriptor;
    const DWORD query = GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor.value);
    if (query) Fail(query, "Read runtime directory owner");
    LocalMemory currentSid;
    if (!ConvertStringSidToSidW(security.sid.c_str(), &currentSid.value)) Fail(GetLastError(), "Parse current user SID");
    if (!EqualSid(owner, currentSid.value) && !IsWellKnownSid(owner, WinBuiltinAdministratorsSid) && !IsWellKnownSid(owner, WinLocalSystemSid))
        Fail(ERROR_ACCESS_DENIED, "Runtime directory has an unexpected owner");
    PACL acl{}; BOOL present{}, defaulted{};
    if (!GetSecurityDescriptorDacl(security.descriptor.value, &present, &acl, &defaulted) || !present)
        Fail(ERROR_INVALID_SECURITY_DESCR, "Read runtime access list");
    const DWORD update = SetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    if (update) Fail(update, "Restrict runtime directory access");
}
void LockExistingAncestors(const std::wstring& path, std::vector<Handle>& locks) {
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') Fail(ERROR_BAD_PATHNAME, "Runtime requires a local absolute path");
    for (size_t end = 3;;) {
        const std::wstring prefix = path.substr(0, end);
        Handle directory(CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!directory.valid()) Fail(GetLastError(), "Open runtime parent directory");
        CheckObject(directory.value, true);
        locks.push_back(std::move(directory));
        if (end == path.size()) break;
        const size_t next = path.find(L'\\', end + 1);
        end = next == std::wstring::npos ? path.size() : next;
    }
}
void MakeDirectory(const std::wstring& path, UserSecurity& security, std::vector<Handle>& locks) {
    if (!CreateDirectoryW(path.c_str(), &security.attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        Fail(GetLastError(), "Create runtime directory");
    Handle directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directory.valid()) Fail(GetLastError(), "Open protected runtime directory");
    CheckObject(directory.value, true);
    RestrictDirectory(directory.value, security);
    locks.push_back(std::move(directory));
}
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) Fail(ERROR_NOT_SUPPORTED, "Open SHA256 provider");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr;
            Fail(ERROR_GEN_FAILURE, "Create SHA256 hash");
        }
    }
    ~Sha256() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
    void Add(const BYTE* data, ULONG bytes) {
        if (BCryptHashData(hash_, const_cast<BYTE*>(data), bytes, 0) < 0) Fail(ERROR_GEN_FAILURE, "Compute SHA256 hash");
    }
    std::string Finish() {
        std::array<BYTE, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) Fail(ERROR_GEN_FAILURE, "Finish SHA256 hash");
        constexpr char hex[] = "0123456789abcdef";
        std::string text;
        for (BYTE byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; }
        return text;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
};
std::string HashFile(HANDLE file) {
    if (!SetFilePointerEx(file, {}, nullptr, FILE_BEGIN)) Fail(GetLastError(), "Rewind runtime file");
    Sha256 hash;
    std::array<BYTE, 32768> buffer{};
    for (;;) {
        DWORD bytes{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) Fail(GetLastError(), "Read runtime file");
        if (!bytes) return hash.Finish();
        hash.Add(buffer.data(), bytes);
    }
}
struct Payload {
    WORD resource;
    const wchar_t* filename;
    const char* hash;
};
void Extract(HINSTANCE module, const Payload& payload, const std::wstring& directory, UserSecurity& security) {
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(payload.resource), RT_RCDATA);
    if (!resource) Fail(GetLastError(), "Find embedded runtime payload");
    const DWORD bytes = SizeofResource(module, resource);
    HGLOBAL loaded = LoadResource(module, resource);
    const auto* data = static_cast<const BYTE*>(LockResource(loaded));
    if (!loaded || !data || !bytes) Fail(ERROR_RESOURCE_DATA_NOT_FOUND, "Load embedded runtime payload");
    Sha256 resourceHash;
    resourceHash.Add(data, bytes);
    if (resourceHash.Finish() != payload.hash) Fail(ERROR_CRC, "Embedded runtime payload checksum mismatch");
    const std::wstring destination = directory + L"\\" + payload.filename;
    {
        Handle existing(CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (existing.valid()) {
            CheckObject(existing.value, false);
            if (HashFile(existing.value) == payload.hash) return;
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) Fail(error, "Open existing runtime payload");
        }
    }
    std::array<BYTE, 16> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        Fail(ERROR_GEN_FAILURE, "Create runtime temporary name");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring suffix;
    for (BYTE byte : nonce) { suffix += hex[byte >> 4]; suffix += hex[byte & 15]; }
    const std::wstring temporary = directory + L"\\.gwm-" + suffix + L".part";
    Handle output(CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, &security.attributes,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!output.valid()) Fail(GetLastError(), "Create runtime payload staging file");
    bool renamed{};
    try {
        DWORD written{};
        if (!WriteFile(output.value, data, bytes, &written, nullptr)) Fail(GetLastError(), "Write runtime payload");
        if (written != bytes) Fail(ERROR_WRITE_FAULT, "Partial runtime payload write");
        if (!FlushFileBuffers(output.value)) Fail(GetLastError(), "Flush runtime payload");
        if (HashFile(output.value) != payload.hash) Fail(ERROR_CRC, "Extracted runtime payload checksum mismatch");
        // Replace by handle, never by deleting an unknown path. A running older
        // helper keeps its executable locked and yields a sharing error here.
        const size_t nameBytes = destination.size() * sizeof(wchar_t);
        std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = TRUE;
        rename->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(rename->FileName, destination.data(), nameBytes);
        if (!SetFileInformationByHandle(output.value, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())))
            Fail(GetLastError(), "Publish runtime payload (close older mapping sessions before upgrading)");
        renamed = true;
    } catch (...) {
        if (!renamed) {
            FILE_DISPOSITION_INFO remove{TRUE};
            SetFileInformationByHandle(output.value, FileDispositionInfo, &remove, sizeof(remove));
        }
        throw;
    }
}
} // namespace

std::wstring PrepareRuntimePayload(HINSTANCE module) {
    if (!module) Fail(ERROR_INVALID_HANDLE, "Missing DLL resource module");
    UserSecurity security;
    RuntimeLock lock(security);
    wchar_t* local{};
    const HRESULT folder = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local);
    if (FAILED(folder)) Fail(HRESULT_CODE(folder), "Find LocalAppData");
    std::wstring directory(local);
    CoTaskMemFree(local);
    std::vector<Handle> directories;
    LockExistingAncestors(directory, directories);
    for (const wchar_t* component : {L"GameWindowMapper", L"Runtime", L"v1"}) {
        directory += L"\\";
        directory += component;
        MakeDirectory(directory, security, directories);
    }
    const std::wstring driver = directory + L"\\driver";
    MakeDirectory(driver, security, directories);
    if (driver.size() + 32 >= MAX_PATH) Fail(ERROR_FILENAME_EXCED_RANGE, "Runtime path exceeds SetupAPI path limit");
    constexpr Payload driverFiles[]{
        {502, L"MttVDD.inf", "550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d"},
        {503, L"MttVDD.dll", "c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05"},
        {504, L"mttvdd.cat", "08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6"},
        {505, L"LICENSE.txt", GWM_PAYLOAD_LICENSE_SHA256}
    };
    for (const auto& file : driverFiles) Extract(module, file, driver, security);
    const Payload helper{501, L"gwm_recovery.exe", GWM_PAYLOAD_HELPER_SHA256};
    Extract(module, helper, directory, security);
    return directory + L"\\gwm_recovery.exe";
}
} // namespace gwm::vd

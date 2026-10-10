#include "display_manager.h"

#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <utility>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "user32.lib")

namespace gwm::vd {
namespace {
constexpr GUID kDisplayClass{0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr wchar_t kHardwareId[] = L"Root\\MttVDD";
constexpr wchar_t kOwnerKey[] = L"SOFTWARE\\GameWindowMapper\\VirtualDisplay";
constexpr wchar_t kVendorKey[] = L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver";
constexpr wchar_t kMutexName[] = L"Global\\GameWindowMapper.VirtualDisplay.Configuration.v1";
constexpr wchar_t kDeviceName[] = L"GameWindowMapperVDD";
constexpr DWORD kSchema = 1;
constexpr std::array<std::pair<int, int>, 5> kModes{{
    {1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 2880}, {7680, 4320}
}};
constexpr char kXml[] =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<vdd_settings><monitors><count>1</count></monitors>"
    "<gpu><friendlyname>default</friendlyname></gpu>"
    "<global><g_refresh_rate>60</g_refresh_rate></global><resolutions>"
    "<resolution><width>1920</width><height>1080</height><refresh_rate>60</refresh_rate></resolution>"
    "<resolution><width>2560</width><height>1440</height><refresh_rate>60</refresh_rate></resolution>"
    "<resolution><width>3840</width><height>2160</height><refresh_rate>60</refresh_rate></resolution>"
    "<resolution><width>5120</width><height>2880</height><refresh_rate>60</refresh_rate></resolution>"
    "<resolution><width>7680</width><height>4320</height><refresh_rate>60</refresh_rate></resolution>"
    "</resolutions><options><CustomEdid>false</CustomEdid><PreventSpoof>false</PreventSpoof>"
    "<EdidCeaOverride>false</EdidCeaOverride><HardwareCursor>true</HardwareCursor>"
    "<SDR10bit>false</SDR10bit><HDRPlus>false</HDRPlus><logging>false</logging>"
    "<debuglogging>false</debuglogging></options></vdd_settings>\n";

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle() = default;
    explicit Handle(HANDLE v) : value(v) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    bool valid() const noexcept { return value && value != INVALID_HANDLE_VALUE; }
};
struct RegKey {
    HKEY value{};
    ~RegKey() { if (value) RegCloseKey(value); }
};
struct DeviceSet {
    HDEVINFO value{INVALID_HANDLE_VALUE};
    explicit DeviceSet(HDEVINFO v) : value(v) {}
    ~DeviceSet() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
    DeviceSet(const DeviceSet&) = delete;
    DeviceSet& operator=(const DeviceSet&) = delete;
};
struct Device {
    std::wstring instance;
    ULONG status{}, problem{};
    bool present{};
};
struct Topology {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

bool Equal(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}
std::wstring ErrorText(DWORD code) {
    wchar_t* raw{};
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring result = count && raw ? std::wstring(raw, count) : L"Windows 返回未描述的错误";
    if (raw) LocalFree(raw);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
DisplayResult Failure(DWORD code, const std::wstring& context) {
    DisplayResult result;
    result.code = code ? code : ERROR_GEN_FAILURE;
    result.message = context + L"（" + std::to_wstring(result.code) + L"）：" + ErrorText(result.code);
    return result;
}
DisplayResult InstallFailure(DWORD code, const std::wstring& context) {
    DisplayResult result = Failure(code, context);
    if (code == ERROR_AUTHENTICODE_TRUST_NOT_ESTABLISHED || code == ERROR_AUTHENTICODE_PUBLISHER_NOT_TRUSTED ||
        code == ERROR_AUTHENTICODE_DISALLOWED ||
        code == static_cast<DWORD>(CERT_E_UNTRUSTEDROOT) || code == ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION) {
        result.message += L"。文件 SHA256/Authenticode 校验通过不代表 Windows 已信任该驱动发布者；"
            L"驱动安装信任或交互授权尚未满足，静默安装已停止。未导入证书或更改安全策略";
    }
    return result;
}
struct NonInteractiveSetup {
    BOOL previous{SetupSetNonInteractiveMode(TRUE)};
    ~NonInteractiveSetup() { SetupSetNonInteractiveMode(previous); }
};
void Report(const PhaseCallback& callback, Phase phase, const wchar_t* text) {
    if (callback) callback(phase, text);
}

class ConfigurationLock {
public:
    DWORD code{};
    ConfigurationLock() {
        PSECURITY_DESCRIPTOR sd{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;AU)", SDDL_REVISION_1, &sd, nullptr)) {
            code = GetLastError(); return;
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), sd, FALSE};
        handle_ = CreateMutexExW(&attributes, kMutexName, 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
        const DWORD createError = GetLastError();
        LocalFree(sd);
        if (!handle_) { code = createError; return; }
        const DWORD wait = WaitForSingleObject(handle_, 30000);
        acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!acquired_) code = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    }
    ~ConfigurationLock() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
private:
    HANDLE handle_{};
    bool acquired_{};
};

DWORD ReadString(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring& out) {
    DWORD bytes{};
    LSTATUS status = RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS) return static_cast<DWORD>(status);
    if (bytes < sizeof(wchar_t) || bytes > 32768 * sizeof(wchar_t)) return ERROR_INVALID_DATA;
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1);
    status = RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value.data(), &bytes);
    if (status == ERROR_SUCCESS) out.assign(value.data());
    return static_cast<DWORD>(status);
}
DWORD ReadOwnership(std::wstring& instance, bool& owned) {
    owned = false;
    DWORD schema{}, bytes = sizeof(schema);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, kOwnerKey, L"Schema",
        RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &schema, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return static_cast<DWORD>(status);
    if (schema != kSchema) return ERROR_REVISION_MISMATCH;
    std::wstring config;
    DWORD error = ReadString(HKEY_LOCAL_MACHINE, kOwnerKey, L"ConfigurationDirectory", config);
    if (error != ERROR_SUCCESS) return error;
    if (!Equal(config, ConfigurationDirectory())) return ERROR_BAD_CONFIGURATION;
    owned = true;
    error = ReadString(HKEY_LOCAL_MACHINE, kOwnerKey, L"InstanceId", instance);
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
}
DWORD WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return static_cast<DWORD>(RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
}
DWORD SaveOwnership(const std::wstring& instance) {
    RegKey key;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kOwnerKey, 0, nullptr, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key.value, nullptr);
    if (status != ERROR_SUCCESS) return static_cast<DWORD>(status);
    DWORD error = WriteString(key.value, L"ConfigurationDirectory", ConfigurationDirectory());
    if (!error) error = WriteString(key.value, L"InstanceId", instance);
    if (!error) error = static_cast<DWORD>(RegSetValueExW(key.value, L"Schema", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&kSchema), sizeof(kSchema)));
    if (!error) error = static_cast<DWORD>(RegFlushKey(key.value));
    return error;
}

bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA& info) {
    DWORD type{}, bytes{};
    SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, &type, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || type != REG_MULTI_SZ || bytes > 65536) return false;
    std::vector<wchar_t> ids(bytes / sizeof(wchar_t) + 2);
    if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, &type,
        reinterpret_cast<BYTE*>(ids.data()), bytes, nullptr)) return false;
    for (const wchar_t* id = ids.data(); *id; id += wcslen(id) + 1) {
        if (_wcsicmp(id, kHardwareId) == 0 || _wcsicmp(id, L"MttVDD") == 0) return true;
    }
    return false;
}
DWORD EnumerateDevices(std::vector<Device>& devices) {
    // Include absent/unconfigured devnodes: installing by hardware ID must not
    // quietly adopt another application's disabled or incomplete installation.
    DeviceSet set(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
    if (set.value == INVALID_HANDLE_VALUE) return GetLastError();
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA info{sizeof(info)};
        if (!SetupDiEnumDeviceInfo(set.value, index, &info)) {
            const DWORD error = GetLastError();
            return error == ERROR_NO_MORE_ITEMS ? ERROR_SUCCESS : error;
        }
        if (!HasHardwareId(set.value, info)) continue;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(set.value, &info, instance, MAX_DEVICE_ID_LEN, nullptr)) return GetLastError();
        Device device;
        device.instance = instance;
        device.present = CM_Get_DevNode_Status(&device.status, &device.problem, info.DevInst, 0) == CR_SUCCESS;
        devices.push_back(std::move(device));
    }
}
DWORD CheckOwnership(std::wstring& instance, bool& marked, std::vector<Device>& devices) {
    DWORD error = ReadOwnership(instance, marked);
    if (error) return error;
    error = EnumerateDevices(devices);
    if (error) return error;
    for (const auto& device : devices) {
        if (!marked || instance.empty() || !Equal(device.instance, instance)) return ERROR_ALREADY_EXISTS;
    }
    RegKey vendor;
    const LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVendorKey, 0, KEY_READ | KEY_WOW64_64KEY, &vendor.value);
    if (status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return static_cast<DWORD>(status);
    DWORD values{};
    error = static_cast<DWORD>(RegQueryInfoKeyW(vendor.value, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, &values, nullptr, nullptr, nullptr, nullptr));
    if (error || values == 0) return error;
    std::wstring vendorPath;
    error = ReadString(HKEY_LOCAL_MACHINE, kVendorKey, L"VDDPATH", vendorPath);
    if (error || !marked || !Equal(vendorPath, ConfigurationDirectory())) return ERROR_ALREADY_EXISTS;
    // Only this integration's single override is permitted. Feature overrides
    // left by another installer take precedence over our SDR XML settings.
    return values == 1 ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
}

DWORD QueryTopology(UINT32 flags, Topology& result) {
    for (int attempt = 0; attempt != 5; ++attempt) {
        UINT32 pathCount{}, modeCount{};
        LONG error = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
        if (error) return static_cast<DWORD>(error);
        result.paths.resize(pathCount);
        result.modes.resize(modeCount);
        error = QueryDisplayConfig(flags, &pathCount, result.paths.data(), &modeCount, result.modes.data(), nullptr);
        if (error == ERROR_INSUFFICIENT_BUFFER) continue;
        if (error) return static_cast<DWORD>(error);
        result.paths.resize(pathCount);
        result.modes.resize(modeCount);
        return ERROR_SUCCESS;
    }
    return ERROR_RETRY;
}
bool IsDeviceOrDescendant(DEVINST node, const std::wstring& instance) {
    for (int depth = 0; depth != 32; ++depth) {
        wchar_t current[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(node, current, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
        if (Equal(current, instance)) return true;
        DEVINST parent{};
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return false;
}
bool InterfaceBelongsTo(const wchar_t* path, const std::wstring& instance) {
    if (!path || !*path) return false;
    DeviceSet set(SetupDiCreateDeviceInfoList(nullptr, nullptr));
    if (set.value == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA interfaceData{sizeof(interfaceData)};
    if (!SetupDiOpenDeviceInterfaceW(set.value, path, 0, &interfaceData)) return false;
    DWORD required{};
    SetupDiGetDeviceInterfaceDetailW(set.value, &interfaceData, nullptr, 0, &required, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<BYTE> buffer(required);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA info{sizeof(info)};
    if (!SetupDiGetDeviceInterfaceDetailW(set.value, &interfaceData, detail, required, nullptr, &info)) return false;
    return IsDeviceOrDescendant(info.DevInst, instance);
}
bool OwnedPath(const DISPLAYCONFIG_PATH_INFO& path, const std::wstring& instance) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.targetInfo.adapterId, path.targetInfo.id};
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && InterfaceBelongsTo(target.monitorDevicePath, instance)) return true;
    DISPLAYCONFIG_ADAPTER_NAME adapter{};
    adapter.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME, sizeof(adapter), path.targetInfo.adapterId, 0};
    return DisplayConfigGetDeviceInfo(&adapter.header) == ERROR_SUCCESS && InterfaceBelongsTo(adapter.adapterDevicePath, instance);
}
const DISPLAYCONFIG_SOURCE_MODE* SourceMode(const Topology& topology, const DISPLAYCONFIG_PATH_INFO& path) {
    const UINT32 index = path.sourceInfo.modeInfoIdx;
    if (index >= topology.modes.size() || topology.modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) return nullptr;
    return &topology.modes[index].sourceMode;
}
RECT SourceRect(const DISPLAYCONFIG_SOURCE_MODE& mode) {
    return {mode.position.x, mode.position.y, mode.position.x + static_cast<LONG>(mode.width),
        mode.position.y + static_cast<LONG>(mode.height)};
}
void FillPath(DisplayResult& result, const Topology& topology, const DISPLAYCONFIG_PATH_INFO& path) {
    result.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
    if (const auto* source = SourceMode(topology, path)) result.bounds = SourceRect(*source);
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), path.sourceInfo.adapterId, path.sourceInfo.id};
    if (!DisplayConfigGetDeviceInfo(&name.header)) result.gdiDeviceName = name.viewGdiDeviceName;
}
DisplayResult InspectImpl() {
    DisplayResult result;
    bool marked{};
    std::vector<Device> devices;
    const DWORD error = CheckOwnership(result.instanceId, marked, devices);
    if (error) return Failure(error, error == ERROR_ALREADY_EXISTS ? L"发现其他软件的 VDD 设备或配置，不能接管" : L"读取虚拟屏所有权失败");
    if (devices.empty()) return Failure(ERROR_NOT_FOUND, L"尚未安装本项目的虚拟显示设备");
    result.installed = true;
    const Device& device = devices.front();
    if (!device.present || !(device.status & DN_STARTED) || (device.status & DN_HAS_PROBLEM)) {
        result.code = ERROR_NOT_READY;
        result.rebootRequired = device.problem == CM_PROB_NEED_RESTART;
        result.message = L"本项目虚拟设备尚未正常启动，设备问题代码 " + std::to_wstring(device.problem);
        return result;
    }
    Topology topology;
    if (const DWORD query = QueryTopology(QDC_ONLY_ACTIVE_PATHS, topology)) {
        result.code = query;
        result.message = L"读取当前显示拓扑失败：" + ErrorText(query);
        return result;
    }
    for (const auto& path : topology.paths) {
        if (!OwnedPath(path, result.instanceId)) continue;
        if (result.active) return Failure(ERROR_BAD_CONFIGURATION, L"本项目设备出现多个活动显示路径，拒绝修改");
        FillPath(result, topology, path);
    }
    result.message = result.active ? L"虚拟扩展屏已就绪" : L"虚拟显示驱动已安装，扩展屏尚未连接";
    return result;
}

DWORD VerifyFileHash(HANDLE file, const wchar_t* expected) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return ERROR_NOT_SUPPORTED;
    BCRYPT_HASH_HANDLE hash{};
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); return ERROR_GEN_FAILURE; }
    std::array<BYTE, 16384> buffer{};
    DWORD error{};
    for (;;) {
        DWORD bytes{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) { error = GetLastError(); break; }
        if (!bytes) break;
        if (BCryptHashData(hash, buffer.data(), bytes, 0) < 0) { error = ERROR_GEN_FAILURE; break; }
    }
    std::array<BYTE, 32> digest{};
    if (!error && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) error = ERROR_GEN_FAILURE;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (error) return error;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring actual;
    for (BYTE byte : digest) { actual.push_back(hex[byte >> 4]); actual.push_back(hex[byte & 15]); }
    return Equal(actual, expected) ? ERROR_SUCCESS : ERROR_CRC;
}
DWORD VerifyTrust(const std::wstring& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);
    return static_cast<DWORD>(result);
}
DWORD ValidatePackage(const std::wstring& directory, std::vector<Handle>& locks) {
    struct File { const wchar_t* name; const wchar_t* sha256; bool signedFile; };
    constexpr File files[]{
        {L"MttVDD.inf", L"550D211FE481E74DFE3F9D724ED78BE48B3A9113405965D683D9373E8D672F5D", false},
        {L"mttvdd.cat", L"08A0093FC9B2E32B287A6F8A77CA4DE0A31830D29FC33D2B13A918DC859468F6", true},
        {L"MttVDD.dll", L"C9CA837F57A98FBD43BC416A7F535A95843626E7759EAF85CF0CD7CE334DBB05", true}
    };
    for (const auto& entry : files) {
        const std::wstring path = directory + L"\\" + entry.name;
        // Deny write/delete sharing until the entire installation has finished.
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file.valid()) return GetLastError();
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info)) return GetLastError();
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
        if (DWORD error = VerifyFileHash(file.value, entry.sha256)) return error;
        if (entry.signedFile) if (DWORD error = VerifyTrust(path)) return error;
        locks.push_back(std::move(file));
    }
    return ERROR_SUCCESS;
}

DWORD SecureDirectory(const std::wstring& path) {
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)",
        SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const DWORD createError = GetLastError();
    if (!created && createError != ERROR_ALREADY_EXISTS) { LocalFree(descriptor); return createError; }
    Handle directory(CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directory.valid()) { LocalFree(descriptor); return GetLastError(); }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(directory.value, &info)) { LocalFree(descriptor); return GetLastError(); }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        LocalFree(descriptor); return ERROR_ACCESS_DENIED;
    }
    PSID owner{};
    PSECURITY_DESCRIPTOR current{};
    DWORD error = GetSecurityInfo(directory.value, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &current);
    if (!error && !IsWellKnownSid(owner, WinBuiltinAdministratorsSid) && !IsWellKnownSid(owner, WinLocalSystemSid)) error = ERROR_ACCESS_DENIED;
    if (current) LocalFree(current);
    PACL acl{}; BOOL present{}, defaulted{};
    if (!error && !GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted)) error = GetLastError();
    if (!error) error = SetSecurityInfo(directory.value, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(descriptor);
    return error;
}
DWORD ConfigureDriver() {
    const std::wstring directory = ConfigurationDirectory();
    if (directory.empty()) return ERROR_PATH_NOT_FOUND;
    const auto slash = directory.find_last_of(L'\\');
    DWORD error = SecureDirectory(directory.substr(0, slash));
    if (!error) error = SecureDirectory(directory);
    if (error) return error;
    const std::wstring path = directory + L"\\vdd_settings.xml";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return ERROR_ACCESS_DENIED;
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid()) return GetLastError();
    BY_HANDLE_FILE_INFORMATION fileInfo{};
    if (!GetFileInformationByHandle(file.value, &fileInfo)) return GetLastError();
    if ((fileInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || fileInfo.nNumberOfLinks != 1)
        return ERROR_ACCESS_DENIED;
    if (!SetEndOfFile(file.value)) return GetLastError();
    DWORD bytes{};
    if (!WriteFile(file.value, kXml, sizeof(kXml) - 1, &bytes, nullptr) || bytes != sizeof(kXml) - 1) return GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    if (!FlushFileBuffers(file.value)) return GetLastError();
    RegKey vendor;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kVendorKey, 0, nullptr, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &vendor.value, nullptr);
    if (status != ERROR_SUCCESS) return static_cast<DWORD>(status);
    return WriteString(vendor.value, L"VDDPATH", directory);
}

DWORD StageVerifiedPackage(std::vector<Handle>& originals, std::wstring& directory,
                          std::vector<Handle>& stagedLocks) {
    // Copy from the already locked/hashed handles into an administrator-owned
    // directory. SetupAPI must never reopen paths controlled by an unelevated
    // caller after validation (e.g. after a parent-directory rename).
    const std::wstring protectedDirectory = ConfigurationDirectory() + L"\\Driver-25.7.23";
    if (const DWORD error = SecureDirectory(protectedDirectory)) return error;
    if (Equal(directory, protectedDirectory)) return ERROR_SUCCESS;
    directory = protectedDirectory;
    constexpr const wchar_t* names[]{L"MttVDD.inf", L"mttvdd.cat", L"MttVDD.dll"};
    if (originals.size() != std::size(names)) return ERROR_INVALID_DATA;
    for (size_t index = 0; index != std::size(names); ++index) {
        const std::wstring destination = directory + L"\\" + names[index];
        const DWORD attributes = GetFileAttributesW(destination.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return ERROR_ACCESS_DENIED;
        Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!output.valid()) return GetLastError();
        BY_HANDLE_FILE_INFORMATION fileInfo{};
        if (!GetFileInformationByHandle(output.value, &fileInfo)) return GetLastError();
        if ((fileInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || fileInfo.nNumberOfLinks != 1)
            return ERROR_ACCESS_DENIED;
        if (!SetEndOfFile(output.value)) return GetLastError();
        if (!SetFilePointerEx(originals[index].value, {}, nullptr, FILE_BEGIN)) return GetLastError();
        std::array<BYTE, 16384> buffer{};
        for (;;) {
            DWORD read{}, written{};
            if (!ReadFile(originals[index].value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return GetLastError();
            if (!read) break;
            if (!WriteFile(output.value, buffer.data(), read, &written, nullptr)) return GetLastError();
            if (written != read) return ERROR_WRITE_FAULT;
        }
        if (!FlushFileBuffers(output.value)) return GetLastError();
    }
    return ValidatePackage(directory, stagedLocks);
}

DWORD StartDevice(HDEVINFO set, SP_DEVINFO_DATA& info, bool& reboot) {
    SP_PROPCHANGE_PARAMS change{};
    change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.StateChange = DICS_ENABLE;
    change.Scope = DICS_FLAG_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader, sizeof(change)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info)) return GetLastError();
    SP_DEVINSTALL_PARAMS_W params{sizeof(params)};
    if (SetupDiGetDeviceInstallParamsW(set, &info, &params)) reboot |= (params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
    return ERROR_SUCCESS;
}

DWORD CompactModes(Topology& topology) {
    const auto oldModes = std::move(topology.modes);
    topology.modes.clear();
    std::vector<UINT32> indices(oldModes.size(), DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
    for (auto& path : topology.paths) {
        for (auto* index : {&path.sourceInfo.modeInfoIdx, &path.targetInfo.modeInfoIdx}) {
            if (*index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) continue;
            if (*index >= oldModes.size()) return ERROR_INVALID_DATA;
            if (indices[*index] == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
                indices[*index] = static_cast<UINT32>(topology.modes.size());
                topology.modes.push_back(oldModes[*index]);
            }
            *index = indices[*index];
        }
    }
    return ERROR_SUCCESS;
}
DWORD ApplyTopology(Topology& topology, bool validateOnly = false) {
    DWORD error = CompactModes(topology);
    if (error) return error;
    const UINT32 flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VALIDATE;
    LONG status = SetDisplayConfig(static_cast<UINT32>(topology.paths.size()), topology.paths.data(),
        static_cast<UINT32>(topology.modes.size()), topology.modes.data(), flags);
    if (!status && !validateOnly) status = SetDisplayConfig(static_cast<UINT32>(topology.paths.size()), topology.paths.data(),
        static_cast<UINT32>(topology.modes.size()), topology.modes.data(), SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY);
    // Deliberately never SDC_ALLOW_CHANGES, SDC_TOPOLOGY_EXTEND or a saved global
    // topology: those may change other screens' modes, position or primary role.
    return static_cast<DWORD>(status);
}
DWORD DisableOwnedHdr(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
    color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(color), path.targetInfo.adapterId, path.targetInfo.id};
    const LONG query = DisplayConfigGetDeviceInfo(&color.header);
    if (query == ERROR_NOT_SUPPORTED || query == ERROR_INVALID_PARAMETER) return ERROR_SUCCESS;
    if (query) return static_cast<DWORD>(query);
    if (!color.advancedColorEnabled) return ERROR_SUCCESS;
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state{};
    state.header = {DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE, sizeof(state), path.targetInfo.adapterId, path.targetInfo.id};
    state.enableAdvancedColor = 0;
    const LONG update = DisplayConfigSetDeviceInfo(&state.header);
    if (update) return static_cast<DWORD>(update);
    if (const LONG verify = DisplayConfigGetDeviceInfo(&color.header)) return static_cast<DWORD>(verify);
    return color.advancedColorEnabled ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
}
} // namespace

std::wstring ConfigurationDirectory() {
    wchar_t* directory{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &directory))) return {};
    std::wstring result(directory);
    CoTaskMemFree(directory);
    return result + L"\\GameWindowMapper\\VirtualDisplay";
}

DisplayResult InspectOwnedDisplay() { return InspectImpl(); }

DisplayResult VerifyDriverPackage(const std::wstring& packageDirectory) {
    if (packageDirectory.empty()) return Failure(ERROR_INVALID_PARAMETER, L"未指定原始签名驱动目录");
    std::vector<Handle> locks;
    const DWORD error = ValidatePackage(packageDirectory, locks);
    if (error) return Failure(error, L"驱动包内容或签名校验失败");
    DisplayResult result;
    result.message = L"VirtualDrivers 25.7.23 原始 AMD64 驱动包 SHA256 与签名验证通过";
    return result;
}

DisplayResult InstallOwnedDriver(const std::wstring& packageDirectory, const PhaseCallback& phase) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    if (sizeof(void*) != 8 || system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
        return Failure(ERROR_NOT_SUPPORTED, L"虚拟显示安装程序需要 Windows x64 和原生 x64 辅助进程");
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return Failure(GetLastError(), L"读取安装进程权限失败");
    TOKEN_ELEVATION elevation{}; DWORD returned{};
    if (!GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &returned)) return Failure(GetLastError(), L"读取安装进程权限失败");
    if (!elevation.TokenIsElevated) return Failure(ERROR_ELEVATION_REQUIRED, L"安装虚拟显示驱动需要 Windows 管理员授权");
    ConfigurationLock lock;
    if (lock.code) return Failure(lock.code, L"另一进程正在配置虚拟显示器");
    Report(phase, Phase::Checking, L"检查驱动所有权");
    std::wstring instance; bool marked{};
    std::vector<Device> devices;
    if (const DWORD error = CheckOwnership(instance, marked, devices)) return Failure(error, L"驱动所有权检查失败，未修改现有设备");
    if (!devices.empty()) {
        DisplayResult current = InspectImpl();
        if (current) return current;
    }
    if (packageDirectory.empty()) return Failure(ERROR_INVALID_PARAMETER, L"未指定原始签名驱动目录");
    wchar_t absolute[32768]{};
    const DWORD length = GetFullPathNameW(packageDirectory.c_str(), static_cast<DWORD>(std::size(absolute)), absolute, nullptr);
    if (!length || length >= std::size(absolute)) return Failure(ERROR_BAD_PATHNAME, L"驱动包目录无效");
    std::wstring directory(absolute);
    while (directory.size() > 3 && (directory.back() == L'\\' || directory.back() == L'/')) directory.pop_back();
    Report(phase, Phase::VerifyingPackage, L"校验原始签名驱动");
    std::vector<Handle> packageLocks;
    if (const DWORD error = ValidatePackage(directory, packageLocks)) return Failure(error, L"驱动包内容或签名校验失败，未安装");
    if (const DWORD error = SaveOwnership(instance)) return Failure(error, L"记录本项目驱动所有权失败");
    if (const DWORD error = ConfigureDriver()) return Failure(error, L"创建受保护的虚拟屏配置失败");
    std::vector<Handle> stagedLocks;
    if (const DWORD error = StageVerifiedPackage(packageLocks, directory, stagedLocks)) return Failure(error, L"缓存原始签名驱动到受保护目录失败");
    const std::wstring inf = directory + L"\\MttVDD.inf";
    if (inf.size() >= MAX_PATH) return Failure(ERROR_FILENAME_EXCED_RANGE, L"SetupAPI 驱动路径超过长度限制");
    Report(phase, Phase::Installing, L"安装本项目虚拟显示设备");
    // A background helper must fail explicitly when Windows requires publisher
    // confirmation, instead of blocking on an invisible Windows Security UI.
    NonInteractiveSetup nonInteractive;
    wchar_t publishedInf[MAX_PATH]{};
    if (!SetupCopyOEMInfW(inf.c_str(), directory.c_str(), SPOST_PATH, 0, publishedInf, MAX_PATH, nullptr, nullptr))
        return InstallFailure(GetLastError(), L"Windows 拒绝将原始驱动加入驱动存储");

    DeviceSet set(SetupDiCreateDeviceInfoList(&kDisplayClass, nullptr));
    if (set.value == INVALID_HANDLE_VALUE) return Failure(GetLastError(), L"创建显示设备信息集失败");
    SP_DEVINFO_DATA info{sizeof(info)};
    if (devices.empty()) {
        if (!SetupDiCreateDeviceInfoW(set.value, kDeviceName, &kDisplayClass, L"GameWindowMapper Virtual Display",
            nullptr, DICD_GENERATE_ID, &info)) return Failure(GetLastError(), L"创建本项目根显示设备失败");
        constexpr wchar_t hardwareIds[] = L"Root\\MttVDD\0";
        if (!SetupDiSetDeviceRegistryPropertyW(set.value, &info, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(hardwareIds), sizeof(hardwareIds))) return Failure(GetLastError(), L"设置根显示设备硬件 ID 失败");
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(set.value, &info, id, MAX_DEVICE_ID_LEN, nullptr)) return Failure(GetLastError(), L"读取新设备身份失败");
        instance = id;
        // Persist exact ownership before registration. A crash afterwards leaves
        // a repairable owned instance, not an indistinguishable foreign device.
        if (const DWORD error = SaveOwnership(instance)) return Failure(error, L"保存新设备身份失败");
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.value, &info)) return Failure(GetLastError(), L"注册本项目根显示设备失败");
    } else if (!SetupDiOpenDeviceInfoW(set.value, instance.c_str(), nullptr, 0, &info)) {
        return Failure(GetLastError(), L"打开已登记的虚拟显示设备失败");
    }
    SP_DEVINSTALL_PARAMS_W params{sizeof(params)};
    if (!SetupDiGetDeviceInstallParamsW(set.value, &info, &params)) return Failure(GetLastError(), L"读取设备安装参数失败");
    params.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
    wcscpy_s(params.DriverPath, publishedInf);
    if (!SetupDiSetDeviceInstallParamsW(set.value, &info, &params) ||
        !SetupDiBuildDriverInfoList(set.value, &info, SPDIT_COMPATDRIVER)) return Failure(GetLastError(), L"选择已校验驱动失败");
    SP_DRVINFO_DATA_W driver{sizeof(driver)};
    if (!SetupDiEnumDriverInfoW(set.value, &info, SPDIT_COMPATDRIVER, 0, &driver)) return Failure(GetLastError(), L"未找到匹配的 AMD64 驱动");
    BOOL needReboot{};
    // This targets one exact devnode, unlike UpdateDriverForPlugAndPlayDevices.
    const BOOL installed = DiInstallDevice(nullptr, set.value, &info, &driver, 0, &needReboot);
    const DWORD installError = GetLastError();
    SetupDiDestroyDriverInfoList(set.value, &info, SPDIT_COMPATDRIVER);
    if (!installed) return InstallFailure(installError, L"Windows 拒绝安装虚拟显示驱动");
    bool reboot = needReboot != FALSE;
    if (!reboot) {
        const DWORD startError = StartDevice(set.value, info, reboot);
        if (startError) return Failure(startError, L"启动本项目虚拟显示设备失败");
    }
    if (reboot) {
        DisplayResult result;
        result.code = ERROR_SUCCESS_REBOOT_REQUIRED;
        result.installed = true;
        result.rebootRequired = true;
        result.instanceId = instance;
        result.message = L"驱动已安装，Windows 要求重启后才能使用虚拟屏；程序不会自动重启";
        return result;
    }
    Report(phase, Phase::Initializing, L"等待虚拟显示设备启动");
    DisplayResult result;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    for (;;) {
        result = InspectImpl();
        if (result || result.rebootRequired || (result.code != ERROR_NOT_READY && result.code != ERROR_NOT_FOUND)) break;
        if (GetTickCount64() >= deadline) break;
        Sleep(200);
    }
    return result;
}

DisplayResult EnsureExtendedDisplay(int minWidth, int minHeight, const PhaseCallback& phase) {
    if (minWidth <= 0 || minHeight <= 0 || minWidth > 7680 || minHeight > 4320)
        return Failure(ERROR_NOT_SUPPORTED, L"源窗口尺寸不能完整放入本版本虚拟屏支持的显示模式");
    ConfigurationLock lock;
    if (lock.code) return Failure(lock.code, L"另一进程正在配置虚拟显示器");
    Report(phase, Phase::Checking, L"检查虚拟显示设备");
    DisplayResult result = InspectImpl();
    if (!result) return result;
    Report(phase, Phase::Initializing, L"初始化虚拟扩展屏");
    Topology all;
    std::vector<DISPLAYCONFIG_PATH_INFO> owned;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    for (;;) {
        if (const DWORD error = QueryTopology(QDC_ALL_PATHS, all)) return Failure(error, L"读取可用显示路径失败");
        owned.clear();
        for (const auto& path : all.paths) {
            if (path.targetInfo.targetAvailable && OwnedPath(path, result.instanceId)) owned.push_back(path);
        }
        if (!owned.empty()) break;
        if (GetTickCount64() >= deadline) break;
        Sleep(200);
    }
    if (owned.empty()) return Failure(ERROR_NOT_READY, L"驱动已安装，但没有可用的本项目虚拟显示路径");
    Topology base;
    base.modes = all.modes;
    POINT position{};
    bool hasOther{};
    LONG rightmost = std::numeric_limits<LONG>::min();
    DISPLAYCONFIG_PATH_INFO* activeOwned{};
    for (auto& path : owned) if (path.flags & DISPLAYCONFIG_PATH_ACTIVE) {
        if (activeOwned) return Failure(ERROR_BAD_CONFIGURATION, L"本项目虚拟屏出现多个活动路径");
        activeOwned = &path;
    }
    for (const auto& path : all.paths) {
        if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE) || OwnedPath(path, result.instanceId)) continue;
        const auto* source = SourceMode(all, path);
        if (!source) return Failure(ERROR_INVALID_DATA, L"现有显示器没有完整模式信息，拒绝更改拓扑");
        const RECT bounds = SourceRect(*source);
        if (bounds.right > rightmost) { rightmost = bounds.right; position = {bounds.right, bounds.top}; }
        hasOther = true;
        base.paths.push_back(path);
    }
    if (!hasOther) return Failure(ERROR_NOT_SUPPORTED, L"需要至少一个现有显示器，不能将虚拟屏设为主屏");
    minWidth = (std::max)(1920, minWidth);
    minHeight = (std::max)(1080, minHeight);
    if (activeOwned) {
        const auto* current = SourceMode(all, *activeOwned);
        if (!current || (current->position.x == 0 && current->position.y == 0))
            return Failure(ERROR_BAD_CONFIGURATION, L"虚拟屏模式异常或被设为主屏，拒绝自动修改");
        minWidth = (std::max)(minWidth, static_cast<int>(current->width));
        minHeight = (std::max)(minHeight, static_cast<int>(current->height));
        position = {current->position.x, current->position.y};
        const RECT currentRect = SourceRect(*current);
        for (const auto& other : base.paths) {
            const RECT otherRect = SourceRect(*SourceMode(base, other)); RECT intersection{};
            if ((SameLuid(other.sourceInfo.adapterId, activeOwned->sourceInfo.adapterId) && other.sourceInfo.id == activeOwned->sourceInfo.id) ||
                IntersectRect(&intersection, &currentRect, &otherRect))
                return Failure(ERROR_BAD_CONFIGURATION, L"虚拟屏被设为镜像或与现有屏幕重叠，请先恢复窗口再重试");
        }
        // Repeated hide requests must not reset every display mode. Existing
        // parked windows retain their coordinates, including during growth.
        const auto refresh = activeOwned->targetInfo.refreshRate;
        if (current->width >= static_cast<UINT32>(minWidth) && current->height >= static_cast<UINT32>(minHeight) &&
            refresh.Denominator && refresh.Numerator == 60 * refresh.Denominator) {
            if (const DWORD error = DisableOwnedHdr(*activeOwned)) return Failure(error, L"无法将本项目虚拟屏设为 SDR");
            FillPath(result, all, *activeOwned);
            Report(phase, Phase::Ready, L"虚拟扩展屏已就绪");
            return result;
        }
    }
    DWORD lastError = ERROR_NOT_SUPPORTED;
    bool applied{};
    for (const auto& candidate : owned) {
        if (activeOwned && !(candidate.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
        const bool sourceUsed = std::any_of(base.paths.begin(), base.paths.end(), [&](const auto& path) {
            return SameLuid(path.sourceInfo.adapterId, candidate.sourceInfo.adapterId) && path.sourceInfo.id == candidate.sourceInfo.id;
        });
        if (sourceUsed) continue; // Never convert an existing clone source in place.
        for (const auto [width, height] : kModes) {
            if (width < minWidth || height < minHeight) continue;
            if (static_cast<int64_t>(position.x) + width > std::numeric_limits<LONG>::max() ||
                static_cast<int64_t>(position.y) + height > std::numeric_limits<LONG>::max()) continue;
            RECT proposed{position.x, position.y, position.x + width, position.y + height};
            bool overlaps{};
            for (const auto& other : base.paths) {
                const RECT otherRect = SourceRect(*SourceMode(base, other)); RECT intersection{};
                if (IntersectRect(&intersection, &proposed, &otherRect)) { overlaps = true; break; }
            }
            if (overlaps) { lastError = ERROR_BAD_CONFIGURATION; continue; }
            Topology desired = base;
            DISPLAYCONFIG_PATH_INFO path = candidate;
            DISPLAYCONFIG_MODE_INFO source{};
            source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
            source.id = path.sourceInfo.id;
            source.adapterId = path.sourceInfo.adapterId;
            source.sourceMode.width = static_cast<UINT32>(width);
            source.sourceMode.height = static_cast<UINT32>(height);
            source.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
            source.sourceMode.position = {position.x, position.y};
            path.sourceInfo.modeInfoIdx = static_cast<UINT32>(desired.modes.size());
            desired.modes.push_back(source);
            path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            path.targetInfo.refreshRate = {60, 1};
            path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
            path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
            path.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
            path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
            desired.paths.push_back(path);
            lastError = ApplyTopology(desired);
            if (!lastError) { applied = true; break; }
        }
        if (applied) break;
    }
    if (!applied) return Failure(lastError, L"无法在保留现有显示器设置的同时连接所需虚拟屏模式");
    Topology current;
    if (const DWORD error = QueryTopology(QDC_ONLY_ACTIVE_PATHS, current)) return Failure(error, L"虚拟屏连接后读取显示配置失败");
    for (const auto& path : current.paths) {
        if (!OwnedPath(path, result.instanceId)) continue;
        FillPath(result, current, path);
        if (const DWORD error = DisableOwnedHdr(path)) return Failure(error, L"无法将本项目虚拟屏设为 SDR");
        if (result.bounds.right - result.bounds.left < minWidth || result.bounds.bottom - result.bounds.top < minHeight)
            return Failure(ERROR_BAD_CONFIGURATION, L"虚拟屏实际尺寸小于源窗口，不能隐藏");
        result.message = L"虚拟扩展屏已就绪";
        Report(phase, Phase::Ready, L"虚拟扩展屏已就绪");
        return result;
    }
    return Failure(ERROR_NOT_READY, L"Windows 未保持虚拟显示路径");
}

DisplayResult DetachOwnedDisplay() {
    ConfigurationLock lock;
    if (lock.code) return Failure(lock.code, L"另一进程正在配置虚拟显示器");
    DisplayResult result = InspectImpl();
    if (result.code == ERROR_NOT_FOUND) { result.code = ERROR_SUCCESS; result.message = L"没有本项目虚拟显示路径需要撤销"; return result; }
    if (!result) return result;
    Topology current;
    if (const DWORD error = QueryTopology(QDC_ONLY_ACTIVE_PATHS, current)) return Failure(error, L"读取当前显示拓扑失败");
    const size_t before = current.paths.size();
    std::erase_if(current.paths, [&](const auto& path) { return OwnedPath(path, result.instanceId); });
    if (before == current.paths.size()) { result.active = false; result.message = L"虚拟屏已断开"; return result; }
    if (current.paths.empty()) return Failure(ERROR_NOT_SUPPORTED, L"当前只剩本项目虚拟屏，不能撤销最后一个显示器");
    if (const DWORD error = ApplyTopology(current)) return Failure(error, L"撤销本项目虚拟显示路径失败");
    result = InspectImpl();
    if (result && result.active) return Failure(ERROR_BUSY, L"虚拟显示路径仍处于活动状态");
    if (result) { result.bounds = {}; result.gdiDeviceName.clear(); result.message = L"虚拟屏已断开，驱动保留供下次使用"; }
    return result;
}
} // namespace gwm::vd

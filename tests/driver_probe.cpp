#include "virtual/display_manager.h"
#include <iostream>
#include <string>

namespace {
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string Json(const std::wstring& value) {
    std::string out = "\"";
    for (const unsigned char ch : Utf8(value)) {
        if (ch == '\\' || ch == '"') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else if (ch >= 0x20) out += static_cast<char>(ch);
    }
    return out + '"';
}
void Print(const gwm::vd::DisplayResult& result) {
    std::cout << "{\"code\":" << result.code << ",\"message\":" << Json(result.message)
        << ",\"rebootRequired\":" << (result.rebootRequired ? "true" : "false")
        << ",\"installed\":" << (result.installed ? "true" : "false")
        << ",\"active\":" << (result.active ? "true" : "false")
        << ",\"instanceId\":" << Json(result.instanceId)
        << ",\"gdiDeviceName\":" << Json(result.gdiDeviceName)
        << ",\"bounds\":[" << result.bounds.left << ',' << result.bounds.top << ','
        << result.bounds.right << ',' << result.bounds.bottom << "]}\n";
}
bool Number(const wchar_t* text, int& value) {
    wchar_t* end{};
    const long parsed = wcstol(text, &end, 10);
    if (!*text || *end || parsed <= 0 || parsed > 8192) return false;
    value = static_cast<int>(parsed);
    return true;
}
}

int wmain(int argc, wchar_t** argv) {
    const auto phase = [](gwm::vd::Phase current, const std::wstring& text) {
        std::cerr << "phase=" << static_cast<int>(current) << ' ' << Utf8(text) << '\n';
    };
    gwm::vd::DisplayResult result;
    if (argc == 1 || (argc == 2 && wcscmp(argv[1], L"--inspect") == 0)) {
        result = gwm::vd::InspectOwnedDisplay();
    } else if (argc == 3 && wcscmp(argv[1], L"--verify-package") == 0) {
        result = gwm::vd::VerifyDriverPackage(argv[2]);
    } else if (argc == 3 && wcscmp(argv[1], L"--install") == 0) {
        result = gwm::vd::InstallOwnedDriver(argv[2], phase);
    } else if (argc == 2 && wcscmp(argv[1], L"--detach") == 0) {
        result = gwm::vd::DetachOwnedDisplay();
    } else if ((argc == 2 || argc == 4) && wcscmp(argv[1], L"--extend") == 0) {
        int width = 1920, height = 1080;
        if (argc == 4 && (!Number(argv[2], width) || !Number(argv[3], height))) {
            std::cerr << "invalid physical pixel dimensions\n"; return ERROR_INVALID_PARAMETER;
        }
        result = gwm::vd::EnsureExtendedDisplay(width, height, phase);
    } else {
        std::cerr << "driver_probe [--inspect | --verify-package <driver-directory> | --install <driver-directory> | --extend [width height] | --detach]\n"
            "--inspect and --verify-package are read-only. Install requires elevation. Before --detach restore all parked windows.\n";
        return ERROR_INVALID_PARAMETER;
    }
    Print(result);
    return static_cast<int>(result.code);
}

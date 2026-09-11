// Real WGC client-coordinate verification against the owned dynamic source.
// Only child processes launched here are moved/resized/maximized/closed.
#include "capture.h"
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("{\"test\":\"%s\",\"pass\":%s}\n", name, ok ? "true" : "false");
    if (!ok) ++failures;
    std::fflush(stdout);
}
struct Source {
    PROCESS_INFORMATION process{};
    HWND hwnd = nullptr;
    Source(const std::filesystem::path& executable, bool d3d) {
        const auto title = L"GWM_CAPTURE_GEOMETRY_" + std::to_wstring(GetCurrentProcessId()) + (d3d ? L"_D3D11" : L"_GDI");
        std::wstring command = L"\"" + executable.wstring() + L"\" " + (d3d ? L"--d3d11" : L"--gdi") +
            L" --title \"" + title + L"\" --x 30 --y 30 --width 640 --height 480";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, executable.parent_path().c_str(), &startup, &process))
            throw std::runtime_error("Cannot launch owned dynamic_source.exe");
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            hwnd = FindWindowW(L"GWM.DynamicSource", title.c_str());
            if (hwnd || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(30);
        }
        if (!hwnd) { close(); throw std::runtime_error("Owned dynamic source did not create a HWND"); }
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        if (pid != process.dwProcessId) { hwnd = nullptr; close(); throw std::runtime_error("Source HWND ownership mismatch"); }
    }
    void close() noexcept {
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
                // Fallback is restricted to the exact process handle created above.
                TerminateProcess(process.hProcess, 125); WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
        }
        hwnd = nullptr;
    }
    ~Source() { close(); }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
};
struct HiddenCanvas {
    HWND hwnd = nullptr;
    HiddenCanvas() {
        hwnd = CreateWindowExW(0, L"STATIC", L"GWM capture geometry hidden target", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) throw std::runtime_error("Cannot create hidden graphics target");
    }
    ~HiddenCanvas() { if (hwnd) DestroyWindow(hwnd); }
};
struct Pixels { UINT width = 0, height = 0; std::vector<uint32_t> values; };
Pixels readTexture(gwm::Capture& capture, gwm::Graphics& graphics) {
    if (!capture.texture) throw std::runtime_error("No capture texture");
    D3D11_TEXTURE2D_DESC desc{}; capture.texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) throw std::runtime_error("Unexpected capture pixel format");
    Pixels pixels; pixels.width = desc.Width; pixels.height = desc.Height;
    pixels.values.resize(static_cast<size_t>(desc.Width) * desc.Height);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(graphics.device->CreateTexture2D(&desc, nullptr, staging.put()));
    graphics.context->CopyResource(staging.get(), capture.texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(graphics.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) pixels.values[static_cast<size_t>(y) * desc.Width + x] = row[x];
    }
    graphics.context->Unmap(staging.get(), 0);
    return pixels;
}
bool stabilize(gwm::Capture& capture, gwm::Graphics& graphics, HWND source) {
    const uint64_t initialFrames = capture.frames;
    const uint64_t started = GetTickCount64(), deadline = started + 12000;
    RECT previous{}; unsigned equalGeometry = 0;
    do {
        capture.update(graphics);
        RECT actual{}; GetClientRect(source, &actual);
        if (EqualRect(&actual, &previous)) ++equalGeometry; else equalGeometry = 0;
        previous = actual;
        if (capture.state == GWM_LIVE && capture.frames >= initialFrames + 6 && equalGeometry >= 8 &&
            GetTickCount64() >= started + 500 && capture.clientSize.cx == actual.right &&
            capture.clientSize.cy == actual.bottom && GetTickCount64() - capture.frameAt <= 250)
            return true;
        Sleep(30);
    } while (GetTickCount64() < deadline);
    return false;
}
void sample(const Pixels& pixels, const gwm::Capture& capture, int x, int y, uint32_t expected, const char* name) {
    const int sx = capture.clientOffset.x + x, sy = capture.clientOffset.y + y;
    const bool inside = sx >= 0 && sy >= 0 && static_cast<UINT>(sx) < pixels.width && static_cast<UINT>(sy) < pixels.height;
    const uint32_t actual = inside ? pixels.values[static_cast<size_t>(sy) * pixels.width + sx] : 0;
    bool same = inside;
    for (unsigned shift : {0u, 8u, 16u}) {
        const int got = static_cast<int>((actual >> shift) & 255), wanted = static_cast<int>((expected >> shift) & 255);
        if (got < wanted - 3 || got > wanted + 3) same = false;
    }
    std::printf("{\"sample\":\"%s\",\"client_x\":%d,\"client_y\":%d,\"texture_x\":%d,\"texture_y\":%d,\"expected_rgb\":\"%06x\",\"actual_rgb\":\"%06x\",\"pass\":%s}\n",
        name, x, y, sx, sy, expected & 0xffffffu, actual & 0xffffffu, same ? "true" : "false");
    if (!same) ++failures;
}
void verify(gwm::Capture& capture, gwm::Graphics& graphics, HWND source, const char* backend, const char* phase) {
    const bool stable = stabilize(capture, graphics, source);
    RECT client{}; GetClientRect(source, &client);
    std::printf("{\"backend\":\"%s\",\"phase\":\"%s\",\"dpi\":%u,\"client_width\":%ld,\"client_height\":%ld,\"capture_offset_x\":%ld,\"capture_offset_y\":%ld,\"captured_frames\":%llu,\"stable\":%s}\n",
        backend, phase, GetDpiForWindow(source), client.right, client.bottom, capture.clientOffset.x, capture.clientOffset.y,
        static_cast<unsigned long long>(capture.frames), stable ? "true" : "false");
    check(stable, "capture_geometry_stabilizes");
    if (!stable) return;
    const auto pixels = readTexture(capture, graphics);
    // These points avoid all text (y <= 81), the moving marker (height/2),
    // and bottom-right coordinates. Paired axis samples detect x and y offsets.
    sample(pixels, capture, 100, 120, 0x435a72, "vertical_grid_100_120");
    sample(pixels, capture, 110, 120, 0x131e2e, "background_110_120");
    sample(pixels, capture, 120, 100, 0x435a72, "horizontal_grid_120_100");
    sample(pixels, capture, 120, 110, 0x131e2e, "background_120_110");
    sample(pixels, capture, 150, 120, 0x435a72, "second_vertical_grid_150_120");
    sample(pixels, capture, 160, 120, 0x131e2e, "second_background_160_120");
    std::fflush(stdout);
}
void run(const std::filesystem::path& executable, bool d3d) {
    Source source(executable, d3d);
    HiddenCanvas target;
    gwm::Graphics graphics(target.hwnd);
    gwm::Capture capture(source.hwnd, graphics);
    const char* backend = d3d ? "D3D11" : "GDI";
    BOOL wow64 = FALSE; IsWow64Process(source.process.hProcess, &wow64);
    std::printf("{\"source_backend\":\"%s\",\"source_pid\":%lu,\"source_is_wow64\":%s}\n",
        backend, static_cast<unsigned long>(source.process.dwProcessId), wow64 ? "true" : "false");
    verify(capture, graphics, source.hwnd, backend, "normal");
    check(SetWindowPos(source.hwnd, nullptr, 90, 70, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "move_owned_source");
    verify(capture, graphics, source.hwnd, backend, "moved");
    check(SetWindowPos(source.hwnd, nullptr, 0, 0, 820, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "resize_owned_source");
    verify(capture, graphics, source.hwnd, backend, "resized");
    ShowWindowAsync(source.hwnd, SW_MAXIMIZE);
    verify(capture, graphics, source.hwnd, backend, "maximized");
    check(IsZoomed(source.hwnd) != FALSE, "owned_source_is_maximized");
    ShowWindowAsync(source.hwnd, SW_RESTORE);
    verify(capture, graphics, source.hwnd, backend, "restored");
    check(IsZoomed(source.hwnd) == FALSE && IsIconic(source.hwnd) == FALSE, "owned_source_is_restored");
}
}

int wmain(int argc, wchar_t** argv) {
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    auto source = std::filesystem::path(self).parent_path() / L"dynamic_source.exe";
    if (argc == 3 && std::wstring(argv[1]) == L"--source") source = std::filesystem::absolute(argv[2]);
    else if (argc != 1) { std::fprintf(stderr, "Usage: capture_geometry_tests [--source path-to-dynamic_source.exe]\n"); return 2; }
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        winrt::check_hresult(initialized);
        run(source, false);
        run(source, true);
    } catch (const winrt::hresult_error& error) {
        std::fprintf(stderr, "Capture geometry HRESULT: 0x%08lx\n", static_cast<unsigned long>(error.code().value)); ++failures;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Capture geometry error: %s\n", error.what()); ++failures;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    std::printf("{\"summary\":true,\"scope\":\"actual_WGC_owned_GDI_and_D3D11_client_grid\",\"monitor_count\":%d,\"multi_dpi_cross_monitor_exercised\":false,\"failures\":%d,\"pass\":%s}\n",
        GetSystemMetrics(SM_CMONITORS), failures, failures ? "false" : "true");
    return failures ? 1 : 0;
}

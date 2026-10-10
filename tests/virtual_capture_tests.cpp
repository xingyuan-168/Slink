// Short integration test: only its own animated HWND is moved. Requires the
// project-owned driver to be installed. Never installs a driver or touches games.
#include "capture.h"
#include "virtual/display_manager.h"
#include "virtual/window_parking.h"
#include "virtual_test_source.h"
#include <cstdio>
#include <set>

namespace {
int failures{};
void check(bool pass, const char* name) {
    std::printf("{\"test\":\"%s\",\"pass\":%s}\n", name, pass ? "true" : "false");
    if (!pass) ++failures;
}
uint32_t sample(gwm::Graphics& graphics, const gwm::Capture& capture) {
    if (!capture.texture || !capture.clientSize.cx || !capture.clientSize.cy) return 0;
    const UINT x = UINT(capture.clientOffset.x + capture.clientSize.cx / 2);
    const UINT y = UINT(capture.clientOffset.y + capture.clientSize.cy / 2);
    D3D11_TEXTURE2D_DESC original{}; capture.texture->GetDesc(&original);
    if (x >= original.Width || y >= original.Height) return 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(graphics.device->CreateTexture2D(&desc, nullptr, staging.put()));
    const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
    graphics.context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, capture.texture.get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(graphics.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    const uint32_t pixel = *static_cast<const uint32_t*>(mapped.pData) & 0xffffffu;
    graphics.context->Unmap(staging.get(), 0);
    return pixel;
}
void updates(gwm::Graphics& graphics, gwm::Capture& capture, const char* stage) {
    const auto initialFrames = capture.frames;
    std::set<uint32_t> colors;
    const auto deadline = GetTickCount64() + 2500;
    while (GetTickCount64() < deadline) {
        const auto before = capture.frames;
        capture.update(graphics);
        if (capture.frames > before && capture.state == GWM_LIVE) colors.insert(sample(graphics, capture));
        if (capture.frames >= initialFrames + 6 && colors.contains(0xd21e28) && colors.contains(0x1e32d2)) break;
        Sleep(20);
    }
    const bool pass = capture.frames >= initialFrames + 6 && colors.contains(0xd21e28) && colors.contains(0x1e32d2);
    std::printf("{\"stage\":\"%s\",\"new_frames\":%llu,\"color_count\":%zu,\"pass\":%s}\n",
        stage, static_cast<unsigned long long>(capture.frames - initialFrames), colors.size(), pass ? "true" : "false");
    if (!pass) ++failures;
}
}
int wmain() {
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HWND surface{};
    gwm::vd::WindowParking parking; // Own source dies with this test; no durable host recovery is claimed.
    gwm::vd::SessionIdentity session;
    bool displayActive{};
    try {
        winrt::check_hresult(co);
        auto display = gwm::vd::EnsureExtendedDisplay();
        check(bool(display) && display.active && (display.bounds.left != 0 || display.bounds.top != 0), "owned_nonprimary_extended_display");
        if (!display || !display.active) throw winrt::hresult_error(HRESULT_FROM_WIN32(display.code));
        displayActive = true;
        gwm::test::VirtualSource source;
        HWND h = source.window.load();
        if (!h) throw winrt::hresult_error(E_FAIL);
        RECT original{}; GetWindowRect(h, &original);
        surface = CreateWindowExW(0, L"STATIC", L"GWM virtual capture GPU target", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        gwm::Graphics graphics(surface);
        gwm::Capture capture(h, graphics);
        updates(graphics, capture, "physical_screen_before_hide");
        check(bool(parking.registerSession(GetCurrentProcess(), session)), "register_owned_test_session");
        auto hidden = parking.hide(session, h, display.bounds);
        check(bool(hidden) && parking.isHidden(h), "move_to_virtual_screen");
        RECT moved{}, intersection{}; GetWindowRect(h, &moved);
        check(IntersectRect(&intersection, &moved, &display.bounds) && EqualRect(&intersection, &moved), "whole_window_inside_virtual_screen");
        check(moved.right - moved.left == original.right - original.left && moved.bottom - moved.top == original.bottom - original.top,
            "source_window_dimensions_unchanged");
        updates(graphics, capture, "virtual_screen_while_hidden");
        check(bool(parking.show(session, h)) && !parking.isHidden(h), "restore_to_physical_screen");
        RECT restored{}; GetWindowRect(h, &restored);
        check(EqualRect(&original, &restored) != FALSE, "original_physical_rectangle_restored");
        updates(graphics, capture, "physical_screen_after_restore");
    } catch (const winrt::hresult_error& error) {
        std::printf("{\"hresult\":\"0x%08lx\",\"pass\":false}\n", static_cast<unsigned long>(error.code().value));
        ++failures;
    } catch (...) { ++failures; }
    check(parking.restoreAll().complete(), "test_records_cleared");
    if (surface) DestroyWindow(surface);
    if (displayActive) check(bool(gwm::vd::DetachOwnedDisplay()), "detach_after_own_test_session");
    winrt::clear_factory_cache();
    if (SUCCEEDED(co)) CoUninitialize();
    if (previous) SetThreadDpiAwarenessContext(previous);
    std::printf("{\"summary\":true,\"failures\":%d,\"pass\":%s}\n", failures, failures ? "false" : "true");
    return failures ? 1 : 0;
}

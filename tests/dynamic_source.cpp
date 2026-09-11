// Disposable, independently selectable GDI / D3D11 test window.
// The client surface has a 50-pixel grid, corner coordinates and a frame counter.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
struct Source {
    bool d3d = false;
    HWND window = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ original = nullptr;
    void* pixels = nullptr;
    int width = 0, height = 0;
    uint64_t frame = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Texture2D> backbuffer;

    ~Source() { releaseBitmap(); }
    void releaseBitmap() {
        if (memory && original) SelectObject(memory, original);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        memory = nullptr; bitmap = nullptr; original = nullptr; pixels = nullptr;
    }
    bool resize(int w, int h) {
        if (w <= 0 || h <= 0) return true;
        if (w == width && h == height && memory) return true;
        releaseBitmap(); width = w; height = h;
        memory = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(memory, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!memory || !bitmap) return false;
        original = SelectObject(memory, bitmap);
        if (!d3d) return true;
        HRESULT hr;
        if (!swap) {
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferDesc.Width = static_cast<UINT>(w);
            desc.BufferDesc.Height = static_cast<UINT>(h);
            desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2; desc.OutputWindow = window; desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &context);
        } else {
            backbuffer.Reset();
            hr = swap->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h),
                DXGI_FORMAT_UNKNOWN, 0);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "D3D11 initialization/resize failed: 0x%08lx\n", static_cast<unsigned long>(hr));
            return false;
        }
        return SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer)));
    }
    void draw() {
        if (!memory || IsIconic(window)) return;
        ++frame;
        RECT all{0, 0, width, height};
        HBRUSH bg = CreateSolidBrush(RGB(19, 30, 46));
        FillRect(memory, &all, bg); DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(67, 90, 114));
        auto oldPen = SelectObject(memory, pen);
        for (int x = 0; x < width; x += 50) { MoveToEx(memory, x, 0, nullptr); LineTo(memory, x, height); }
        for (int y = 0; y < height; y += 50) { MoveToEx(memory, 0, y, nullptr); LineTo(memory, width, y); }
        SelectObject(memory, oldPen); DeleteObject(pen);
        SetBkMode(memory, TRANSPARENT); SetTextColor(memory, RGB(255, 255, 255));
        wchar_t label[256];
        swprintf_s(label, L"%s  FRAME %llu  CLIENT %dx%d  网格 50px",
            d3d ? L"D3D11" : L"GDI", static_cast<unsigned long long>(frame), width, height);
        TextOutW(memory, 8, 8, label, static_cast<int>(wcslen(label)));
        TextOutW(memory, 8, 36, L"(0,0) 客户区原点", 11);
        swprintf_s(label, L"(%d,%d)", width - 1, height - 1);
        TextOutW(memory, std::max(0, width - 115), std::max(0, height - 25), label, static_cast<int>(wcslen(label)));
        // A moving marker and binary frame signature make repeated frames visible.
        RECT marker{static_cast<LONG>((frame * 5) % static_cast<uint64_t>(std::max(1, width - 32))), height / 2, 0, height / 2 + 32};
        marker.right = marker.left + 32;
        HBRUSH accent = CreateSolidBrush(RGB(40, 220, 145));
        FillRect(memory, &marker, accent); DeleteObject(accent);
        for (int bit = 0; bit < 24; ++bit) {
            RECT cell{8 + bit * 12, 65, 18 + bit * 12, 81};
            FillRect(memory, &cell, reinterpret_cast<HBRUSH>(GetStockObject((frame & (uint64_t{1} << bit)) ? WHITE_BRUSH : BLACK_BRUSH)));
        }
        GdiFlush();
        if (d3d && backbuffer) {
            // Opaque BGRA; GDI does not initialize the alpha channel.
            auto data = static_cast<uint32_t*>(pixels);
            for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) data[i] |= 0xff000000u;
            context->UpdateSubresource(backbuffer.Get(), 0, nullptr, pixels, static_cast<UINT>(width * 4), 0);
            const auto hr = swap->Present(0, 0);
            if (FAILED(hr)) { std::fprintf(stderr, "Present failed: 0x%08lx\n", static_cast<unsigned long>(hr)); DestroyWindow(window); }
        } else InvalidateRect(window, nullptr, FALSE);
    }
};
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* source = reinterpret_cast<Source*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        source = static_cast<Source*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        source->window = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(source));
    }
    if (!source) return DefWindowProcW(hwnd, message, wp, lp);
    switch (message) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && !source->resize(LOWORD(lp), HIWORD(lp))) DestroyWindow(hwnd);
        return 0;
    case WM_TIMER: source->draw(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        if (!source->d3d && source->memory) BitBlt(dc, 0, 0, source->width, source->height, source->memory, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, message, wp, lp);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    Source source;
    std::wstring title = L"GWM dynamic test source";
    int x = 30, y = 30, width = 640, height = 480;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--d3d11") source.d3d = true;
        else if (arg == L"--gdi") source.d3d = false;
        else if (arg == L"--title" && i + 1 < argc) title = argv[++i];
        else if (arg == L"--x" && i + 1 < argc) x = _wtoi(argv[++i]);
        else if (arg == L"--y" && i + 1 < argc) y = _wtoi(argv[++i]);
        else if (arg == L"--width" && i + 1 < argc) width = _wtoi(argv[++i]);
        else if (arg == L"--height" && i + 1 < argc) height = _wtoi(argv[++i]);
        else { std::fprintf(stderr, "Usage: dynamic_source [--gdi|--d3d11] [--title text] [--x N --y N --width N --height N]\n"); return 2; }
    }
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return 2;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{}; wc.lpfnWndProc = windowProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GWM.DynamicSource"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));
    if (!RegisterClassW(&wc)) return 3;
    RECT bounds{0, 0, width, height}; AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND window = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
        x, y, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, wc.hInstance, &source);
    if (!window) return 4;
    ShowWindow(window, SW_SHOW); UpdateWindow(window);
    if (!SetTimer(window, 1, 16, nullptr)) { DestroyWindow(window); return 5; }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}

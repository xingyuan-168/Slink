#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

namespace gwm::test {
// Disposable HWND on its own responsive UI thread. No existing user window is
// enumerated or moved by the virtual display integration tests.
class VirtualSource {
public:
    std::atomic<HWND> window{};
    VirtualSource(int x = 80, int y = 80) : x_(x), y_(y),
        thread_([this] { run(); }) { WaitForSingleObject(ready_, 5000); }
    ~VirtualSource() {
        if (const auto h = window.load()) PostMessageW(h, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
        CloseHandle(ready_);
    }
    VirtualSource(const VirtualSource&) = delete;
    VirtualSource& operator=(const VirtualSource&) = delete;
private:
    int x_, y_;
    HANDLE ready_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    unsigned frame_{};
    std::thread thread_;
    static LRESULT CALLBACK proc(HWND h, UINT message, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<VirtualSource*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<VirtualSource*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) switch (message) {
        case WM_TIMER: ++self->frame_; InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; const auto dc = BeginPaint(h, &paint);
            RECT area{}; GetClientRect(h, &area);
            const auto brush = CreateSolidBrush((self->frame_ & 1) ? RGB(210, 30, 40) : RGB(30, 50, 210));
            FillRect(dc, &area, brush); DeleteObject(brush);
            EndPaint(h, &paint); return 0;
        }
        case WM_DESTROY: self->window = nullptr; PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, message, w, l);
    }
    void run() {
        const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GWM.VirtualDisplay.TestSource";
        RegisterClassW(&wc);
        const HWND h = CreateWindowExW(0, wc.lpszClassName, L"GWM virtual display test (disposable)",
            WS_OVERLAPPEDWINDOW, x_, y_, 400, 300, nullptr, nullptr, wc.hInstance, this);
        window = h;
        if (h) { ShowWindow(h, SW_SHOWNOACTIVATE); SetTimer(h, 1, 100, nullptr); }
        SetEvent(ready_);
        if (h) {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (previous) SetThreadDpiAwarenessContext(previous);
    }
};
}

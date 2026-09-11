#pragma once
#include "graphics.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Foundation.h>

namespace gwm {
struct Capture {
    HWND hwnd=nullptr;DWORD process=0;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 size{};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    uint64_t frameAt=0,frames=0;
    int state=GWM_WAITING,error=GWM_OK;
    POINT clientOffset{};SIZE clientSize{};
    Capture(HWND,Graphics&);
    ~Capture();
    Capture(const Capture&)=delete;
    Capture& operator=(const Capture&)=delete;
    void update(Graphics&);
    void close() noexcept;
};
}

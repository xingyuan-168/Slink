#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
HWND window{};
bool useD3d{};
uint32_t frame{};
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain> swapchain;
ComPtr<ID2D1Factory> factory;
ComPtr<ID2D1RenderTarget> target;
ComPtr<IDWriteFactory> textFactory;
ComPtr<IDWriteTextFormat> format;
ComPtr<ID2D1SolidColorBrush> brush;
constexpr COLORREF blue=RGB(24,64,224), orange=RGB(240,160,24);

void RebuildTarget() {
    target.Reset(); brush.Reset();
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;
    auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
    if (FAILED(factory->CreateDxgiSurfaceRenderTarget(surface.Get(),&properties,&target))) return;
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),&brush);
}
bool InitD3d() {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width=1920; desc.BufferDesc.Height=1080;
    desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.OutputWindow=window; desc.Windowed=TRUE;
    desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&desc,&swapchain,&device,nullptr,&context);
    if(FAILED(hr)) hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&desc,&swapchain,&device,nullptr,&context);
    if(FAILED(hr)||FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf())))return false;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(textFactory.GetAddressOf()))))return false;
    textFactory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,42,L"en-US",&format);
    RebuildTarget(); return target && brush && format;
}
void PaintGdi(HDC dc) {
    SetPropW(window,L"SLinkFixturePaint",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(frame)));
    RECT client{};GetClientRect(window,&client);
    HBRUSH background=CreateSolidBrush(RGB(16,34,46));FillRect(dc,&client,background);DeleteObject(background);
    // A signature and a binary frame counter are sampled by the PoC from the visible canvas.
    for(int bit=0;bit<20;++bit){
        const bool on=bit<4?((0xAu>>bit)&1u)!=0:((frame>>(bit-4))&1u)!=0;
        RECT r{bit*24,0,(bit+1)*24,48};HBRUSH b=CreateSolidBrush(on?orange:blue);FillRect(dc,&r,b);DeleteObject(b);
    }
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(220,235,245));
    std::wstring label=L"CQ_SLink GDI fixture / frame "+std::to_wstring(frame);
    TextOutW(dc,20,80,label.c_str(),static_cast<int>(label.size()));
    const int x=static_cast<int>(frame%600);RECT moving{x,140,x+80,220};HBRUSH b=CreateSolidBrush(RGB(70,180,140));FillRect(dc,&moving,b);DeleteObject(b);
    HPEN pen=CreatePen(PS_SOLID,2,RGB(255,255,255));HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    Rectangle(dc,0,0,800,600);SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
}
void PaintD3d() {
    if(!target||!brush)return;
    target->BeginDraw(); target->Clear(D2D1::ColorF(16.f/255,34.f/255,46.f/255));
    for(int bit=0;bit<20;++bit){
        bool on=bit<4?((0xAu>>bit)&1u)!=0:((frame>>(bit-4))&1u)!=0;
        brush->SetColor(on?D2D1::ColorF(240.f/255,160.f/255,24.f/255):D2D1::ColorF(24.f/255,64.f/255,224.f/255));
        target->FillRectangle(D2D1::RectF(static_cast<float>(bit*24),0,static_cast<float>((bit+1)*24),48),brush.Get());
    }
    brush->SetColor(D2D1::ColorF(0.86f,0.92f,0.96f));
    auto label=L"CQ_SLink D3D11 fixture / frame "+std::to_wstring(frame);
    target->DrawText(label.c_str(),static_cast<UINT32>(label.size()),format.Get(),D2D1::RectF(20,80,1200,140),brush.Get());
    float x=static_cast<float>(frame%600);target->FillRectangle(D2D1::RectF(x,170,x+80,250),brush.Get());
    target->DrawRectangle(D2D1::RectF(1,1,799,599),brush.Get(),2);
    HRESULT hr=target->EndDraw();if(hr==D2DERR_RECREATE_TARGET)RebuildTarget();
    // Keep presenting while occluded: the fixture must not deliberately pause when parked.
    HRESULT present=swapchain->Present(0,0);
    SetPropW(window,L"SLinkFixturePaint",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(frame)));
    SetPropW(window,L"SLinkFixturePresent",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(present)));
}
LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg){
    case WM_TIMER: ++frame;SetPropW(hwnd,L"SLinkFixtureFrame",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(frame)));if(useD3d)PaintD3d();else {InvalidateRect(hwnd,nullptr,FALSE);UpdateWindow(hwnd);}return 0;
    case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);if(!useD3d)PaintGdi(dc);EndPaint(hwnd,&ps);return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_DESTROY:KillTimer(hwnd,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    useD3d=std::wstring(command).find(L"d3d")!=std::wstring::npos;
    WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=instance;wc.lpszClassName=L"SLinkTestSource";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc))return 2;
    RECT rect{0,0,1920,1080};AdjustWindowRectEx(&rect,WS_OVERLAPPEDWINDOW,FALSE,0);
    window=CreateWindowExW(0,wc.lpszClassName,useD3d?L"CQ_SLink D3D11 test source":L"CQ_SLink GDI test source",
        WS_OVERLAPPEDWINDOW,30,30,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,nullptr);
    if(!window)return 3;
    if(useD3d&&!InitD3d()){DestroyWindow(window);return 4;}
    ShowWindow(window,SW_SHOWNOACTIVATE);SetTimer(window,1,33,nullptr);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    brush.Reset();target.Reset();format.Reset();textFactory.Reset();factory.Reset();swapchain.Reset();context.Reset();device.Reset();
    return 0;
}

#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// A one-to-one 800x600 GPU copy for backend qualification, not the final grid renderer.
class WgcProbe {
    using Item=winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
    using Pool=winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
    using Session=winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGISwapChain> swap;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice interop{nullptr};
    Item item{nullptr};Pool pool{nullptr};Session session{nullptr};
    winrt::event_token token{};bool subscribed{};LONG cropX{},cropY{};
public:
    uint64_t frames{};HRESULT error{S_OK};
    ~WgcProbe(){Close();}
    void Close() noexcept {try{if(pool&&subscribed){pool.FrameArrived(token);subscribed=false;}if(session)session.Close();if(pool)pool.Close();session=nullptr;pool=nullptr;item=nullptr;}catch(...){}}
    void Open(HWND destination,HWND source){
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=800;desc.BufferDesc.Height=600;
        desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount=2;desc.OutputWindow=destination;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        winrt::check_hresult(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&desc,swap.put(),device.put(),nullptr,context.put()));
        auto dxgi=device.as<IDXGIDevice>();winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        interop=inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        auto factory=winrt::get_activation_factory<Item,IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(source,winrt::guid_of<Item>(),winrt::put_abi(item)));
        RECT bounds{};winrt::check_hresult(DwmGetWindowAttribute(source,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)));
        POINT client{};ClientToScreen(source,&client);cropX=client.x-bounds.left;cropY=client.y-bounds.top;
        pool=Pool::CreateFreeThreaded(interop,winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,item.Size());
        token=pool.FrameArrived([destination](auto const&,auto const&){PostMessageW(destination,WM_APP+1,0,0);});subscribed=true;
        session=pool.CreateCaptureSession(item);session.StartCapture();
    }
    void Update() noexcept {try{if(!pool)return;while(auto frame=pool.TryGetNextFrame()){
        auto access=frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture,back;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
        winrt::check_hresult(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),back.put_void()));
        const auto size=frame.ContentSize();
        if(cropX<0||cropY<0||cropX+800>size.Width||cropY+600>size.Height){frame.Close();error=E_BOUNDS;return;}
        D3D11_BOX region{static_cast<UINT>(cropX),static_cast<UINT>(cropY),0,static_cast<UINT>(cropX+800),static_cast<UINT>(cropY+600),1};
        context->CopySubresourceRegion(back.get(),0,0,0,0,texture.get(),0,&region);
        winrt::check_hresult(swap->Present(0,0));++frames;frame.Close();
    }}catch(winrt::hresult_error const&e){error=e.code();}catch(...){error=E_FAIL;}}
};


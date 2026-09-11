#include "capture.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.Metadata.h>

namespace gwm {
using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
Capture::Capture(HWND h,Graphics& g):hwnd(h) {
    if(!IsWindow(h)||GetAncestor(h,GA_ROOT)!=h){state=GWM_INVALID;return;}
    GetWindowThreadProcessId(h,&process);
    auto interop=winrt::get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(h,winrt::guid_of<GraphicsCaptureItem>(),winrt::put_abi(item)));
    size=item.Size();if(size.Width<=0||size.Height<=0)throw winrt::hresult_invalid_argument();
    pool=Direct3D11CaptureFramePool::CreateFreeThreaded(g.runtimeDevice,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
    session=pool.CreateCaptureSession(item);
    if(winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",L"IsCursorCaptureEnabled"))session.IsCursorCaptureEnabled(false);
    session.StartCapture();
}
Capture::~Capture(){close();}
void Capture::close() noexcept {
    try{if(session)session.Close();}catch(...){}
    try{if(pool)pool.Close();}catch(...){}
    session=nullptr;pool=nullptr;item=nullptr;bitmap=nullptr;texture=nullptr;
}
void Capture::update(Graphics& g) {
    DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);
    if(state==GWM_INVALID||!IsWindow(hwnd)||pid!=process){state=GWM_INVALID;close();return;}
    if(IsIconic(hwnd)){state=GWM_MINIMIZED;return;}
    if(!pool){state=GWM_CAPTURE_ERROR;error=GWM_GRAPHICS_FAILURE;return;}
    // Poll on the render thread; free-threaded WGC queues at most two frames.
    // No DLL callback executes on a capture thread during unload.
    winrt::Windows::Graphics::SizeInt32 changed=size;
    for(int i=0;i<2;++i){
        auto frame=pool.TryGetNextFrame();if(!frame)break;
        auto content=frame.ContentSize();if(content.Width<=0||content.Height<=0){frame.Close();continue;}
        changed=content;
        auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),source.put_void()));
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        // A resized item may deliver one frame backed by the old pool size.
        // Keep the preceding texture and geometry together until the new pool
        // supplies a complete frame; never display an undersized partial copy.
        if(desc.Width!=static_cast<UINT>(content.Width)||desc.Height!=static_cast<UINT>(content.Height)){frame.Close();continue;}
        D3D11_TEXTURE2D_DESC existing{};if(texture)texture->GetDesc(&existing);
        if(!texture||existing.Width!=desc.Width||existing.Height!=desc.Height){
            bitmap=nullptr;texture=nullptr;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
            desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.MiscFlags=0;
            winrt::check_hresult(g.device->CreateTexture2D(&desc,nullptr,texture.put()));bitmap=g.bitmap(texture.get());
        }
        g.context->CopyResource(texture.get(),source.get());frameAt=nowMs();++frames;frame.Close();
    }
    if(changed.Width!=size.Width||changed.Height!=size.Height){size=changed;pool.Recreate(g.runtimeDevice,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);}
    RECT client{},bounds{},outer{};POINT origin{};
    if(GetClientRect(hwnd,&client)&&ClientToScreen(hwnd,&origin)){
        GetWindowRect(hwnd,&outer);
        if(FAILED(DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds))))bounds=outer;
        // WGC normally includes the visible frame, not invisible resize borders.
        // Match exact delivered dimensions before selecting the origin.
        D3D11_TEXTURE2D_DESC delivered{};if(texture)texture->GetDesc(&delivered);
        bool matched=true;
        if(delivered.Width==static_cast<UINT>(client.right)&&delivered.Height==static_cast<UINT>(client.bottom)){bounds.left=origin.x;bounds.top=origin.y;}
        else if(delivered.Width==static_cast<UINT>(outer.right-outer.left)&&delivered.Height==static_cast<UINT>(outer.bottom-outer.top))bounds=outer;
        else matched=delivered.Width==static_cast<UINT>(bounds.right-bounds.left)&&delivered.Height==static_cast<UINT>(bounds.bottom-bounds.top);
        if(matched){clientOffset={origin.x-bounds.left,origin.y-bounds.top};clientSize={client.right,client.bottom};}
    }
    state=bitmap?GWM_LIVE:GWM_WAITING;error=GWM_OK;
}
}

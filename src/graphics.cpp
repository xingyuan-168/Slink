#include "graphics.h"
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wincodec.h>
#include <cmath>

namespace gwm {
using winrt::check_hresult;
static D2D1_COLOR_F color(uint32_t c) {return D2D1::ColorF(c);}
std::shared_ptr<Image> decodeImage(const uint8_t* data,int size) {
    // Decode only explicitly supported containers, on the caller's existing COM
    // apartment or a temporary MTA. Never keep caller memory after return.
    HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct ComExit {HRESULT hr;~ComExit(){if(SUCCEEDED(hr))CoUninitialize();}} exit{co};
    if(FAILED(co)&&co!=RPC_E_CHANGED_MODE)check_hresult(co);
    winrt::com_ptr<IWICImagingFactory> f;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(f.put())));
    winrt::com_ptr<IWICStream> stream;check_hresult(f->CreateStream(stream.put()));
    check_hresult(stream->InitializeFromMemory(const_cast<BYTE*>(data),static_cast<DWORD>(size)));
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    check_hresult(f->CreateDecoderFromStream(stream.get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.put()));
    GUID container{};check_hresult(decoder->GetContainerFormat(&container));
    if(container!=GUID_ContainerFormatPng&&container!=GUID_ContainerFormatBmp&&container!=GUID_ContainerFormatJpeg)throw winrt::hresult_invalid_argument();
    winrt::com_ptr<IWICBitmapFrameDecode> frame;check_hresult(decoder->GetFrame(0,frame.put()));
    auto image=std::make_shared<Image>();check_hresult(frame->GetSize(&image->width,&image->height));
    if(!image->width||!image->height||image->width>4096||image->height>4096)throw winrt::hresult_invalid_argument();
    winrt::com_ptr<IWICFormatConverter> converter;check_hresult(f->CreateFormatConverter(converter.put()));
    check_hresult(converter->Initialize(frame.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    image->pixels.resize(size_t(image->width)*image->height*4);
    check_hresult(converter->CopyPixels(nullptr,image->width*4,static_cast<UINT>(image->pixels.size()),image->pixels.data()));
    return image;
}
Graphics::Graphics(HWND h):window(h) {
    D3D_FEATURE_LEVEL level{};
    check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,device.put(),&level,context.put()));
    auto dxgi=device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspect;check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspect.put()));
    runtimeDevice=inspect.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    D2D1_FACTORY_OPTIONS options{};check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,factory.put_void()));
    check_hresult(factory->CreateDevice(dxgi.get(),d2device.put()));
    check_hresult(d2device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,dc.put()));
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.put())));
    setTextDpi(GetDpiForWindow(h));
    check_hresult(dc->CreateSolidColorBrush(color(0xffffff),brush.put()));
    winrt::com_ptr<IDXGIAdapter> adapter;check_hresult(dxgi->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> dxFactory;check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),dxFactory.put_void()));
    RECT r{};GetClientRect(h,&r);width=std::max(1L,r.right);height=std::max(1L,r.bottom);
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    check_hresult(dxFactory->CreateSwapChainForHwnd(device.get(),h,&desc,nullptr,nullptr,swap.put()));
    dxFactory->MakeWindowAssociation(h,DXGI_MWA_NO_ALT_ENTER);
    resize(width,height);
}
void Graphics::setTextDpi(UINT dpi) {
    if(!dpi)dpi=96;if(dpi==textDpi)return;
    font=nullptr;centerFont=nullptr;
    const float pixels=12.0f*dpi/96.0f;
    check_hresult(write->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,pixels,L"zh-CN",font.put()));
    font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};winrt::com_ptr<IDWriteInlineObject> ellipsis;
    check_hresult(write->CreateEllipsisTrimmingSign(font.get(),ellipsis.put()));font->SetTrimming(&trim,ellipsis.get());
    check_hresult(write->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,pixels,L"zh-CN",centerFont.put()));
    centerFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);centerFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    textDpi=dpi;
}
void Graphics::resize(int w,int h) {
    if(w<=0||h<=0)return;
    dc->SetTarget(nullptr);target=nullptr;
    if(w!=width||h!=height){check_hresult(swap->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0));width=w;height=h;}
    winrt::com_ptr<IDXGISurface> surface;check_hresult(swap->GetBuffer(0,__uuidof(IDXGISurface),surface.put_void()));
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
    check_hresult(dc->CreateBitmapFromDxgiSurface(surface.get(),&props,target.put()));dc->SetTarget(target.get());
}
winrt::com_ptr<ID2D1Bitmap1> Graphics::bitmap(ID3D11Texture2D* tex) {
    winrt::com_ptr<IDXGISurface> surface;check_hresult(tex->QueryInterface(__uuidof(IDXGISurface),surface.put_void()));
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    winrt::com_ptr<ID2D1Bitmap1> result;check_hresult(dc->CreateBitmapFromDxgiSurface(surface.get(),&props,result.put()));return result;
}
ID2D1Bitmap1* Graphics::image(const std::shared_ptr<Image>& img) {
    if(!img)return nullptr;
    auto found=images.find(img.get());if(found!=images.end())return found->second.bitmap.get();
    CachedImage entry;entry.image=img;
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    check_hresult(dc->CreateBitmap(D2D1::SizeU(img->width,img->height),img->pixels.data(),img->width*4,&props,entry.bitmap.put()));
    auto result=entry.bitmap.get();images.emplace(img.get(),std::move(entry));return result;
}
void Graphics::begin(){dc->BeginDraw();dc->SetTransform(D2D1::Matrix3x2F::Identity());dc->Clear(color(0x111318));}
bool Graphics::end(){check_hresult(dc->EndDraw());HRESULT result=swap->Present(1,0);check_hresult(result);return result==S_OK;}
void Graphics::fill(D2D1_RECT_F r,uint32_t rgb){brush->SetColor(color(rgb));dc->FillRectangle(r,brush.get());}
void Graphics::text(const std::wstring& s,D2D1_RECT_F r,uint32_t rgb,bool centered) {
    brush->SetColor(color(rgb));dc->DrawTextW(s.c_str(),static_cast<UINT32>(s.size()),centered?centerFont.get():font.get(),r,brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static D2D1_RECT_F fit(float w,float h,D2D1_RECT_F r) {
    float scale=std::min((r.right-r.left)/w,(r.bottom-r.top)/h);
    float x=r.left+((r.right-r.left)-w*scale)*.5f,y=r.top+((r.bottom-r.top)-h*scale)*.5f;
    return D2D1::RectF(x,y,x+w*scale,y+h*scale);
}
void Graphics::drawImage(ID2D1Bitmap1* b,D2D1_RECT_F dest) {
    if(!b)return;auto s=b->GetPixelSize();auto r=fit(float(s.width),float(s.height),dest);
    dc->DrawBitmap(b,r,1,D2D1_INTERPOLATION_MODE_LINEAR);
}
void Graphics::drawRegion(ID2D1Bitmap1* b,Region roi,POINT offset,SIZE client,D2D1_RECT_F area) {
    if(!b)return;auto sz=b->GetPixelSize();
    double sx=double(roi.x)+offset.x,sy=double(roi.y)+offset.y;
    double l=std::max({sx,double(offset.x),0.0}),t=std::max({sy,double(offset.y),0.0});
    double r=std::min({sx+roi.w,double(offset.x)+client.cx,double(sz.width)});
    double bottom=std::min({sy+roi.h,double(offset.y)+client.cy,double(sz.height)});
    if(r<=l||bottom<=t)return;
    auto full=fit(float(roi.w),float(roi.h),area);double scale=(full.right-full.left)/roi.w;
    auto dst=D2D1::RectF(float(full.left+(l-sx)*scale),float(full.top+(t-sy)*scale),float(full.left+(r-sx)*scale),float(full.top+(bottom-sy)*scale));
    auto src=D2D1::RectF(float(l),float(t),float(r),float(bottom));
    dc->DrawBitmap(b,dst,1,D2D1_INTERPOLATION_MODE_LINEAR,src);
}
const wchar_t* stateText(int state) {
    switch(state){case GWM_EMPTY:return L"空槽位";case GWM_WAITING:return L"等待首帧";
    case GWM_MINIMIZED:return L"已最小化";case GWM_INVALID:return L"窗口无效或已关闭";
    case GWM_CAPTURE_ERROR:return L"捕获失败";case GWM_FORCED_BACKGROUND:return L"已启用底图";
    case GWM_RECOVERING:return L"正在恢复图形设备";default:return L"";}
}
}

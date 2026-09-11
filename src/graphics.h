#pragma once
#include "model.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <unordered_map>

namespace gwm {
struct Graphics {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice runtimeDevice{nullptr};
    winrt::com_ptr<IDXGISwapChain1> swap;
    winrt::com_ptr<ID2D1Factory1> factory;
    winrt::com_ptr<ID2D1Device> d2device;
    winrt::com_ptr<ID2D1DeviceContext> dc;
    winrt::com_ptr<ID2D1Bitmap1> target;
    winrt::com_ptr<IDWriteFactory> write;
    winrt::com_ptr<IDWriteTextFormat> font,centerFont;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    struct CachedImage {std::shared_ptr<Image> image;winrt::com_ptr<ID2D1Bitmap1> bitmap;};
    std::unordered_map<Image*,CachedImage> images;
    int width=0,height=0;HWND window=nullptr;
    UINT textDpi=0;
    explicit Graphics(HWND);
    void resize(int,int);
    void setTextDpi(UINT);
    winrt::com_ptr<ID2D1Bitmap1> bitmap(ID3D11Texture2D*);
    ID2D1Bitmap1* image(const std::shared_ptr<Image>&);
    void begin();
    bool end();
    void fill(D2D1_RECT_F,uint32_t rgb);
    void text(const std::wstring&,D2D1_RECT_F,uint32_t,bool centered=false);
    void drawImage(ID2D1Bitmap1*,D2D1_RECT_F);
    void drawRegion(ID2D1Bitmap1*,Region,POINT offset,SIZE client,D2D1_RECT_F);
};
}

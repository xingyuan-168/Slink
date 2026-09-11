// GPU geometry checks against the production Graphics::drawRegion implementation.
// This deliberately tests known texture/client geometry, not WGC's inference of
// a real window's client offset. No user window or display setting is touched.
#include "graphics.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t Red = 0xffe02020u;
constexpr uint32_t Green = 0xff20d040u;
constexpr uint32_t Blue = 0xff2040e0u;
constexpr uint32_t Yellow = 0xffe0d020u;
constexpr uint32_t Frame = 0xffd020d0u;
constexpr uint32_t Background = 0xff111318u;
int failures = 0;

class TestWindow {
public:
    HWND handle = nullptr;
    TestWindow() {
        // The stock STATIC class avoids adding a class/window procedure lifetime.
        // WS_POPUP gives the hidden window an exact 64 x 64 client surface.
        handle = CreateWindowExW(0, L"STATIC", L"GWM GPU geometry test",
            WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!handle) throw std::runtime_error("CreateWindowExW failed");
    }
    ~TestWindow() { if (handle) DestroyWindow(handle); }
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
};

std::vector<uint32_t> readPixels(gwm::Graphics& graphics) {
    winrt::com_ptr<ID3D11Texture2D> rendered;
    winrt::check_hresult(graphics.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), rendered.put_void()));
    D3D11_TEXTURE2D_DESC desc{};
    rendered->GetDesc(&desc);
    if (desc.Width != 64 || desc.Height != 64 || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        throw std::runtime_error("Unexpected render target shape/format");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(graphics.device->CreateTexture2D(&desc, nullptr, staging.put()));
    graphics.context->CopyResource(staging.get(), rendered.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(graphics.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    struct Unmap {
        ID3D11DeviceContext* context; ID3D11Resource* resource;
        ~Unmap() { context->Unmap(resource, 0); }
    } unmap{graphics.context.get(), staging.get()};
    std::vector<uint32_t> result(64 * 64);
    for (size_t y = 0; y < 64; ++y) {
        auto row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        for (size_t x = 0; x < 64; ++x) result[y * 64 + x] = row[x];
    }
    return result;
}

void pixel(const std::vector<uint32_t>& pixels, int x, int y, uint32_t expected, const char* name) {
    const uint32_t actual = pixels[static_cast<size_t>(y) * 64 + x];
    bool equal = true;
    // Ignore target alpha; allow at most two RGB units for implementation rounding.
    // Every checked pixel lies inside a flat color, away from filtering boundaries.
    for (unsigned shift : {0u, 8u, 16u}) {
        const int a = static_cast<int>((actual >> shift) & 255u);
        const int e = static_cast<int>((expected >> shift) & 255u);
        if (a < e - 2 || a > e + 2) equal = false;
    }
    std::printf("{\"test\":\"%s\",\"x\":%d,\"y\":%d,\"expected_rgb\":\"%06x\",\"actual_rgb\":\"%06x\",\"pass\":%s}\n",
        name, x, y, expected & 0xffffffu, actual & 0xffffffu, equal ? "true" : "false");
    if (!equal) ++failures;
}

std::vector<uint32_t> draw(gwm::Graphics& graphics, ID2D1Bitmap1* bitmap,
    gwm::Region roi, POINT offset, SIZE client, D2D1_RECT_F area) {
    graphics.begin();
    graphics.drawRegion(bitmap, roi, offset, client, area);
    // Do not Present: the purpose is deterministic GPU readback from a hidden
    // window, not display refresh/occlusion behavior. EndDraw submits D2D work.
    winrt::check_hresult(graphics.dc->EndDraw());
    return readPixels(graphics);
}

void run() {
    TestWindow window;
    gwm::Graphics graphics(window.handle);
    // Texture 16 x 16. The physical client begins at (4,4), size 8 x 8.
    // The four 4 x 4 client quadrants are red/green/blue/yellow; everything
    // outside the client is a magenta sentinel representing window chrome.
    std::vector<uint32_t> colors(16 * 16, Frame);
    for (int y = 4; y < 8; ++y) for (int x = 4; x < 8; ++x) colors[y * 16 + x] = Red;
    for (int y = 4; y < 8; ++y) for (int x = 8; x < 12; ++x) colors[y * 16 + x] = Green;
    for (int y = 8; y < 12; ++y) for (int x = 4; x < 8; ++x) colors[y * 16 + x] = Blue;
    for (int y = 8; y < 12; ++y) for (int x = 8; x < 12; ++x) colors[y * 16 + x] = Yellow;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 16; desc.Height = 16; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data{colors.data(), 16 * sizeof(uint32_t), 0};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(graphics.device->CreateTexture2D(&desc, &data, texture.put()));
    auto bitmap = graphics.bitmap(texture.get());

    // All expectations below are deliberately hardcoded for integer scaling.
    // They do not recalculate fit/intersection using the production formula.
    auto result = draw(graphics, bitmap.get(), {0, 0, 8, 8}, {4, 4}, {8, 8}, D2D1::RectF(8, 8, 40, 40));
    pixel(result, 16, 16, Red, "client_offset_top_left");
    pixel(result, 32, 16, Green, "client_offset_top_right");
    pixel(result, 16, 32, Blue, "client_offset_bottom_left");
    pixel(result, 32, 32, Yellow, "client_offset_bottom_right");
    pixel(result, 4, 16, Background, "destination_left_untouched");
    pixel(result, 44, 16, Background, "destination_right_untouched");

    result = draw(graphics, bitmap.get(), {2, 2, 4, 4}, {4, 4}, {8, 8}, D2D1::RectF(0, 0, 32, 32));
    pixel(result, 8, 8, Red, "positive_roi_top_left");
    pixel(result, 24, 8, Green, "positive_roi_top_right");
    pixel(result, 8, 24, Blue, "positive_roi_bottom_left");
    pixel(result, 24, 24, Yellow, "positive_roi_bottom_right");

    result = draw(graphics, bitmap.get(), {4, 4, 8, 8}, {4, 4}, {8, 8}, D2D1::RectF(0, 0, 32, 32));
    pixel(result, 8, 8, Yellow, "outside_roi_valid_quarter");
    pixel(result, 24, 8, Background, "outside_roi_right_padding");
    pixel(result, 8, 24, Background, "outside_roi_bottom_padding");
    pixel(result, 24, 24, Background, "outside_roi_no_stretch");

    result = draw(graphics, bitmap.get(), {0, 0, 12, 8}, {4, 4}, {8, 8}, D2D1::RectF(0, 0, 48, 32));
    pixel(result, 8, 8, Red, "wide_roi_valid_left");
    pixel(result, 24, 8, Green, "wide_roi_valid_right");
    pixel(result, 40, 8, Background, "client_clip_excludes_chrome_top");
    pixel(result, 40, 24, Background, "client_clip_excludes_chrome_bottom");

    result = draw(graphics, bitmap.get(), {0, 0, 8, 8}, {4, 4}, {8, 8}, D2D1::RectF(0, 0, 64, 32));
    pixel(result, 8, 16, Background, "horizontal_center_left_padding");
    pixel(result, 56, 16, Background, "horizontal_center_right_padding");
    pixel(result, 24, 8, Red, "horizontal_center_left_quadrant");
    pixel(result, 40, 24, Yellow, "horizontal_center_right_quadrant");

    result = draw(graphics, bitmap.get(), {0, 0, 8, 8}, {4, 4}, {8, 8}, D2D1::RectF(0, 0, 32, 64));
    pixel(result, 16, 8, Background, "vertical_center_top_padding");
    pixel(result, 16, 56, Background, "vertical_center_bottom_padding");
    pixel(result, 8, 24, Red, "vertical_center_top_quadrant");
    pixel(result, 24, 40, Yellow, "vertical_center_bottom_quadrant");

    result = draw(graphics, bitmap.get(), {20, 20, 8, 8}, {4, 4}, {8, 8}, D2D1::RectF(0, 0, 32, 32));
    pixel(result, 8, 8, Background, "fully_outside_roi_is_background");
    pixel(result, 24, 24, Background, "fully_outside_roi_no_chrome");

    result = draw(graphics, bitmap.get(), {0, 0, 8, 8}, {14, 14}, {8, 8}, D2D1::RectF(0, 0, 32, 32));
    pixel(result, 3, 3, Frame, "texture_clip_valid_intersection");
    pixel(result, 16, 3, Background, "texture_clip_right_padding");
    pixel(result, 3, 16, Background, "texture_clip_bottom_padding");
}
}

int wmain() {
    const auto dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        winrt::check_hresult(initialized);
        run();
    } catch (const winrt::hresult_error& error) {
        std::fprintf(stderr, "GPU geometry HRESULT: 0x%08lx\n", static_cast<unsigned long>(error.code().value));
        ++failures;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU geometry error: %s\n", error.what());
        ++failures;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    std::printf("{\"summary\":true,\"scope\":\"production_GPU_geometry_only_not_WGC_client_offset_inference\",\"failures\":%d,\"pass\":%s}\n",
        failures, failures ? "false" : "true");
    return failures ? 1 : 0;
}

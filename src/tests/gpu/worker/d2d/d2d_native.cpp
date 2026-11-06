// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/d2d/d2d_native.h"

#include "tests/gpu/worker/scene/overlay.h"

#include <d2d1_1helper.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace sb::gpu::d2d {

namespace {

constexpr D2D1_COLOR_F kBackground = {18.0f / 255.0f, 20.0f / 255.0f, 26.0f / 255.0f, 1.0f};  // Qt raster's (18, 20, 26)
constexpr wchar_t kFallbackFamily[] = L"Segoe UI";
// The text workload's font: 14 logical px (the canvas transform makes it physical). A uniform line
// with the baseline 14 px below its top, so a string's baseline lands exactly on its op's y (Qt's drawText(point)).
constexpr float kFontPx = 14.0f, kLineBaseline = 14.0f, kLineHeight = 20.0f;

D2D1_COLOR_F colourOf(std::uint32_t rgba8)
{
    // Straight alpha: Direct2D premultiplies brush colours itself.
    return {float(rgba8 & 0xff) / 255.0f, float((rgba8 >> 8) & 0xff) / 255.0f, float((rgba8 >> 16) & 0xff) / 255.0f, float(rgba8 >> 24) / 255.0f};
}

D2D1_BITMAP_PROPERTIES1 bitmapProps(D2D1_BITMAP_OPTIONS options, D2D1_ALPHA_MODE alpha = D2D1_ALPHA_MODE_PREMULTIPLIED)
{
    return D2D1::BitmapProperties1(options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alpha), 96.0f, 96.0f);
}

// The UMD version DXGI reports (CheckInterfaceSupport), as "31.0.15.5222" (the D3D11 backend's).
std::string driverVersion(IDXGIAdapter1 *adapter)
{
    LARGE_INTEGER umd{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        return {};
    char text[64];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                  unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
    return text;
}

} // namespace

Direct2DNative::Direct2DNative(std::vector<std::uint8_t> fontBytes) : m_fontBytes(std::move(fontBytes)) {}

Direct2DNative::~Direct2DNative()
{
    shutdown();
}

void Direct2DNative::shutdown()
{
    if (m_device) {
        waitIdle();
        unload();
        releaseWindow();
    }
    // D2D-1: every Direct2D resource belongs to the device context; all of them go before it.
    m_star.Reset();
    m_textFormat.Reset();
    m_fonts.Reset();
    if (m_dwrite && m_fontLoader)
        m_dwrite->UnregisterFontFileLoader(m_fontLoader.Get());
    m_fontLoader.Reset();
    m_dwrite.Reset();
    m_dc.Reset();
    m_d2dDevice.Reset();
    m_factory.Reset();
    for (ComPtr<ID3D11Query> &q : m_frameQueries)
        q.Reset();
    m_queryPending = {};
    if (m_cx) {
        m_cx->ClearState();
        m_cx->Flush();
    }
    m_cx.Reset();
    m_info.Reset();
    m_device.Reset();
    m_adapter.Reset();
}

std::string Direct2DNative::createDevice(const BackendInit &init, BackendInfo *info)
{
    if (std::string e = dxgi::findAdapter(init, &m_adapter, init.allowSoftware); !e.empty())
        return e;
    DXGI_ADAPTER_DESC1 desc{};
    m_adapter->GetDesc1(&desc);
    // The probe's device (M5): feature level 11_1 or 11_0 with BGRA support answers Direct2D.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (init.validation ? D3D11_CREATE_DEVICE_DEBUG : 0);
    HRESULT hr = D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &m_device, &level, &m_cx);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        std::fprintf(stderr, "syntheon-gpu: d2d: the debug layer isn't installed; running without validation\n");
        flags &= ~UINT(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &m_device, &level, &m_cx);
    }
    if (FAILED(hr))
        return "N/A: Needs a Direct3D 11 device with BGRA support";
    if ((flags & D3D11_CREATE_DEVICE_DEBUG) && SUCCEEDED(m_device.As(&m_info))) {
        m_info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        m_info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
    }
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (FAILED(hr = m_device.As(&dxgiDevice)))
        return dxgi::failure("QueryInterface (IDXGIDevice1)", hr);
    dxgiDevice->SetMaximumFrameLatency(2);  // §5.4 (the swapchain's waitable object does the same for a window)

    D2D1_FACTORY_OPTIONS options{};
    options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
    if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                      reinterpret_cast<void **>(m_factory.ReleaseAndGetAddressOf()))))
        return "N/A: Needs Direct2D 1.1 (" + dxgi::failure("D2D1CreateFactory", hr) + ")";
    if (FAILED(hr = m_factory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice)))
        return dxgi::failure("ID2D1Factory1::CreateDevice", hr);
    if (FAILED(hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dc)))
        return dxgi::failure("ID2D1Device::CreateDeviceContext", hr);
    // Physical pixels throughout: the canvas transform does the logical-to-physical scaling.
    m_dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    m_dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // The offscreen content is premultiplied (not opaque to Direct2D), where ClearType isn't allowed.
    m_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    for (ComPtr<ID3D11Query> &q : m_frameQueries) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (FAILED(hr = m_device->CreateQuery(&qd, &q)))
            return dxgi::failure("CreateQuery (event)", hr);
    }
    if (info) {
        info->deviceName = dxgi::utf8(desc.Description);
        info->apiVersion = std::string("Direct2D 1.1 (feature level ") + (level == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0") + ")";
        info->driver = driverVersion(m_adapter.Get());
        info->vendorId = desc.VendorId;
        info->deviceId = desc.DeviceId;
        info->gpuTimestamps = false;  // the native paths are CPU-timed (Runner::runNative)
        info->layers.clear();
        info->presentMode.clear();
    }
    return {};
}

std::string Direct2DNative::createTextFormat()
{
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(m_dwrite.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
        return dxgi::failure("DWriteCreateFactory", hr);
    // §7.1: IBM Plex Sans from the worker's resource through an in-memory font collection
    // (IDWriteFactory5, Windows 10 1703+). Anything missing: the closest system font, and a warning.
    const auto bundled = [this]() -> std::string {
        if (m_fontBytes.empty())
            return "the bundled font IBM Plex Sans isn't in the worker";
        ComPtr<IDWriteFactory5> f5;
        if (FAILED(m_dwrite.As(&f5)))
            return "DirectWrite can't load in-memory fonts before Windows 10 1703";
        ComPtr<IDWriteInMemoryFontFileLoader> memory;
        HRESULT hr = f5->CreateInMemoryFontFileLoader(&memory);
        if (FAILED(hr))
            return dxgi::failure("CreateInMemoryFontFileLoader", hr);
        if (FAILED(hr = f5->RegisterFontFileLoader(memory.Get())))
            return dxgi::failure("RegisterFontFileLoader", hr);
        m_fontLoader = memory;  // unregistered at shutdown
        ComPtr<IDWriteFontFile> file;  // owner null: DirectWrite keeps its own copy of the bytes
        if (FAILED(hr = memory->CreateInMemoryFontFileReference(f5.Get(), m_fontBytes.data(), UINT32(m_fontBytes.size()), nullptr, &file)))
            return dxgi::failure("CreateInMemoryFontFileReference", hr);
        ComPtr<IDWriteFontSetBuilder1> builder;
        if (FAILED(hr = f5->CreateFontSetBuilder(&builder)))
            return dxgi::failure("CreateFontSetBuilder", hr);
        if (FAILED(hr = builder->AddFontFile(file.Get())))
            return dxgi::failure("IDWriteFontSetBuilder1::AddFontFile", hr);
        ComPtr<IDWriteFontSet> set;
        if (FAILED(hr = builder->CreateFontSet(&set)))
            return dxgi::failure("CreateFontSet", hr);
        ComPtr<IDWriteFontCollection1> collection;
        if (FAILED(hr = f5->CreateFontCollectionFromFontSet(set.Get(), &collection)))
            return dxgi::failure("CreateFontCollectionFromFontSet", hr);
        if (collection->GetFontFamilyCount() == 0)
            return "DirectWrite found no font family in the bundled IBM Plex Sans";
        // The family name as the font file states it (the variable font's named instances share it).
        ComPtr<IDWriteFontFamily1> family;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(hr = collection->GetFontFamily(0, &family)) || FAILED(hr = family->GetFamilyNames(&names)))
            return dxgi::failure("the bundled font's family name", hr);
        UINT32 index = 0, length = 0;
        BOOL exists = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists)
            index = 0;
        if (FAILED(hr = names->GetStringLength(index, &length)))
            return dxgi::failure("the bundled font's family name", hr);
        std::wstring name(std::size_t(length) + 1, L'\0');
        if (FAILED(hr = names->GetString(index, name.data(), length + 1)))
            return dxgi::failure("the bundled font's family name", hr);
        name.resize(length);
        m_family = std::move(name);
        m_fonts = collection;
        return {};
    };
    const std::string why = bundled();
    if (!why.empty()) {
        std::fprintf(stderr, "syntheon-gpu: d2d: warning: %s; the text workload uses Segoe UI\n", why.c_str());
        m_warning = why + "; the text workload uses Segoe UI instead of IBM Plex Sans";  // reaches the result (a `warning` event)
        m_fonts.Reset();  // the system collection
        m_family = kFallbackFamily;
    }
    if (FAILED(hr = m_dwrite->CreateTextFormat(m_family.c_str(), m_fonts.Get(), DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, kFontPx, L"en-us", &m_textFormat)))
        return dxgi::failure("CreateTextFormat", hr);
    m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_textFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kLineHeight, kLineBaseline);
    return {};
}

std::string Direct2DNative::init(const BackendInit &init, const PresentTarget &target, BackendInfo *info)
{
    m_init = init;
    m_headless = target.headless;
    if (std::string e = createDevice(init, info); !e.empty())
        return e;
    if (std::string e = createTextFormat(); !e.empty())
        return e;
    // The unit star (Qt raster's): 10 vertices, outer radius 1, inner 0.45, the first one straight up.
    HRESULT hr = m_factory->CreatePathGeometry(&m_star);
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(hr))
        hr = m_star->Open(&sink);
    if (FAILED(hr))
        return dxgi::failure("CreatePathGeometry (star)", hr);
    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i < 10; ++i) {
        const double r = (i % 2 == 0) ? 1.0 : 0.45;
        const double a = -kPi / 2 + double(i) * kPi / 5;
        const D2D1_POINT_2F p = D2D1::Point2F(float(r * std::cos(a)), float(r * std::sin(a)));
        if (i == 0)
            sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
        else
            sink->AddLine(p);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(hr = sink->Close()))
        return dxgi::failure("ID2D1GeometrySink::Close (star)", hr);
    return validationProblem();
}

std::string Direct2DNative::load(const NativeOps &ops)
{
    if (!m_dc)
        return "load() before init()";
    if (m_init.renderWidth == 0 || m_init.renderHeight == 0)
        return "Direct2D has no render size";
    m_ops = ops;
    const D2D1_SIZE_U size = D2D1::SizeU(m_init.renderWidth, m_init.renderHeight);
    HRESULT hr = m_dc->CreateBitmap(size, nullptr, 0, bitmapProps(D2D1_BITMAP_OPTIONS_TARGET), &m_content);
    if (FAILED(hr))
        return dxgi::failure("CreateBitmap (the content)", hr);
    if (FAILED(hr = m_dc->CreateBitmap(size, nullptr, 0, bitmapProps(D2D1_BITMAP_OPTIONS(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW)), &m_readTarget)))
        return dxgi::failure("CreateBitmap (readback)", hr);
    if (FAILED(hr = m_dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_brush)))
        return dxgi::failure("CreateSolidColorBrush", hr);
    // 16 gradient definitions, created once (§7.1): a 4-stop linear brush (0, 1/3, 2/3, 1) and a 3-stop
    // radial one (0, 1/2, 1) each, so 32 stop collections; frame() only moves their end points.
    if (ops.kind == NativeKind::Gradients) {
        ID2D1RenderTarget *rt = m_dc.Get();  // the ID2D1RenderTarget overloads (ID2D1DeviceContext declares others)
        for (std::size_t g = 0; g < m_linear.size(); ++g) {
            const auto &c = ops.gradientStops[g];
            const D2D1_GRADIENT_STOP linear[4] = {{0.0f, colourOf(c[0])}, {1.0f / 3.0f, colourOf(c[1])}, {2.0f / 3.0f, colourOf(c[2])}, {1.0f, colourOf(c[3])}};
            const D2D1_GRADIENT_STOP radial[3] = {{0.0f, colourOf(c[0])}, {0.5f, colourOf(c[1])}, {1.0f, colourOf(c[2])}};
            ComPtr<ID2D1GradientStopCollection> linearStops, radialStops;
            if (FAILED(hr = rt->CreateGradientStopCollection(linear, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &linearStops))
                || FAILED(hr = rt->CreateGradientStopCollection(radial, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &radialStops)))
                return dxgi::failure("CreateGradientStopCollection", hr);
            if (FAILED(hr = rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 1)),
                                                          linearStops.Get(), &m_linear[g])))
                return dxgi::failure("CreateLinearGradientBrush", hr);
            if (FAILED(hr = rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), 1, 1),
                                                          radialStops.Get(), &m_radial[g])))
                return dxgi::failure("CreateRadialGradientBrush", hr);
        }
    }
    if (ops.kind == NativeKind::Blits) {
        // The 256 x 256 image, straight RGBA -> premultiplied BGRA (Direct2D's bitmap format).
        const Texture &img = ops.image;
        std::vector<std::uint8_t> bgra(img.rgba8.size());
        for (std::size_t i = 0; i + 3 < img.rgba8.size(); i += 4) {
            const unsigned a = img.rgba8[i + 3];
            bgra[i + 0] = std::uint8_t((img.rgba8[i + 2] * a + 127) / 255);
            bgra[i + 1] = std::uint8_t((img.rgba8[i + 1] * a + 127) / 255);
            bgra[i + 2] = std::uint8_t((img.rgba8[i + 0] * a + 127) / 255);
            bgra[i + 3] = std::uint8_t(a);
        }
        if (FAILED(hr = m_dc->CreateBitmap(D2D1::SizeU(img.width, img.height), bgra.data(), img.width * 4, bitmapProps(D2D1_BITMAP_OPTIONS_NONE), &m_image)))
            return dxgi::failure("CreateBitmap (the blit image)", hr);
    }
    if (ops.kind == NativeKind::Text) {
        // The ASCII strings widened once (content, not a cached layout: every frame still calls DrawText).
        m_texts.clear();
        for (const TextOp &op : ops.texts)
            m_texts.emplace_back(op.text.begin(), op.text.end());
    }
    m_loaded = true;
    return validationProblem();
}

void Direct2DNative::unload()
{
    if (!m_dc)
        return;
    waitIdle();
    m_dc->SetTarget(nullptr);  // the content may be the current target
    m_loaded = false;
    m_content.Reset();
    m_readTarget.Reset();
    m_brush.Reset();
    m_image.Reset();
    for (auto &b : m_linear)
        b.Reset();
    for (auto &b : m_radial)
        b.Reset();
    m_texts.clear();
    m_ops = NativeOps{};
}

void Direct2DNative::releaseWindow()
{
    // H1's order for a flip-model swapchain: everything that references the back buffer goes first (the
    // context's target, the bitmap on its surface), then the swapchain, then ClearState and Flush, so the
    // next CreateSwapChainForHwnd on the same HWND doesn't fail with E_ACCESSDENIED.
    if (m_dc)
        m_dc->SetTarget(nullptr);
    m_windowTarget.Reset();
    m_swap.destroy();
    if (m_cx) {
        m_cx->ClearState();
        m_cx->Flush();
    }
    m_presentModeName.clear();
    m_resumed = false;
}

std::string Direct2DNative::suspend()
{
    if (!m_device)
        return {};
    std::string e = waitIdle();
    releaseWindow();
    if (!e.empty())
        return e;
    return validationProblem();
}

std::string Direct2DNative::resume(const PresentTarget &target)
{
    if (!m_dc)
        return "resume() before init()";
    if (m_resumed)
        releaseWindow();
    const auto fail = [this](std::string e) {
        releaseWindow();
        return e;
    };
    HRESULT hr = S_OK;
    if (m_headless) {
        // The back buffer's stand-in: render size, scale 1 (the 3D backends' headless target).
        m_scale = 1.0;
        m_windowW = m_init.renderWidth;
        m_windowH = m_init.renderHeight;
        if (FAILED(hr = m_dc->CreateBitmap(D2D1::SizeU(m_windowW, m_windowH), nullptr, 0, bitmapProps(D2D1_BITMAP_OPTIONS_TARGET), &m_windowTarget)))
            return fail(dxgi::failure("CreateBitmap (the headless target)", hr));
    } else {
        if (!target.hwnd)
            return fail("the test window has no native window handle");
        m_scale = target.scale > 0 ? target.scale : 1.0;
        if (std::string e = m_swap.create(m_device.Get(), static_cast<HWND>(target.hwnd), target.windowWidth, target.windowHeight); !e.empty())
            return fail(e);
        m_windowW = m_swap.width;
        m_windowH = m_swap.height;
        // Buffer 0 is always the one to draw into (the D3D11 runtime rotates a flip-model swapchain's buffers).
        ComPtr<IDXGISurface> surface;
        if (FAILED(hr = m_swap.swapchain->GetBuffer(0, IID_PPV_ARGS(surface.ReleaseAndGetAddressOf()))))
            return fail(dxgi::failure("IDXGISwapChain::GetBuffer", hr));
        if (FAILED(hr = m_dc->CreateBitmapFromDxgiSurface(surface.Get(), bitmapProps(D2D1_BITMAP_OPTIONS(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW), D2D1_ALPHA_MODE_IGNORE),
                                                          &m_windowTarget)))
            return fail(dxgi::failure("CreateBitmapFromDxgiSurface (the back buffer)", hr));
        m_presentModeName = m_swap.modeName();
    }
    m_resumed = true;
    return validationProblem();
}

std::string Direct2DNative::endDraw(const char *what)
{
    const HRESULT hr = m_dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        return "LOST: the Direct2D device was lost";  // D2D-1: every resource would need recreating; report and stop
    if (FAILED(hr))
        return dxgi::failure(what, hr);
    return {};
}

std::string Direct2DNative::drawContent(double t)
{
    m_dc->SetTarget(m_content.Get());
    m_dc->BeginDraw();
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dc->Clear(kBackground);
    // The canvas is 1920 x 1080 logical px, scaled to the render size, under the global translation (§7.1):
    // a canvas point p lands at s (p + shift), as Qt raster's scale(s) then translate(shift).
    const float s = float(m_init.renderWidth) / kCanvasWidth;
    const NativeShift shift = nativeTranslation(t);
    const D2D1::Matrix3x2F canvas = D2D1::Matrix3x2F::Translation(shift.x, shift.y) * D2D1::Matrix3x2F::Scale(s, s);

    switch (m_ops.kind) {
    case NativeKind::Shapes:
        for (const ShapeOp &op : m_ops.shapes) {
            // translate(c), rotate(r) (degrees, clockwise on the y-down canvas, as Qt's).
            const D2D1::Matrix3x2F place = D2D1::Matrix3x2F::Rotation(op.rotation) * D2D1::Matrix3x2F::Translation(op.cx, op.cy) * canvas;
            m_brush->SetColor(colourOf(op.rgba8));
            const D2D1_RECT_F box = D2D1::RectF(-op.w / 2, -op.h / 2, op.w / 2, op.h / 2);
            switch (op.type) {
            case 0:
                m_dc->SetTransform(place);
                m_dc->FillRectangle(box, m_brush.Get());
                break;
            case 1:
                m_dc->SetTransform(place);
                m_dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), op.w / 2, op.h / 2), m_brush.Get());
                break;
            case 2:
                m_dc->SetTransform(place);
                m_dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 8.0f, 8.0f), m_brush.Get(), 2.0f);
                break;
            default:
                m_dc->SetTransform(D2D1::Matrix3x2F::Scale(op.w / 2, op.w / 2) * place);  // a star is square: R = w / 2
                m_dc->FillGeometry(m_star.Get(), m_brush.Get());
                break;
            }
        }
        break;
    case NativeKind::Text:
        m_dc->SetTransform(canvas);
        for (std::size_t i = 0; i < m_ops.texts.size(); ++i) {
            const TextOp &op = m_ops.texts[i];
            m_brush->SetColor(colourOf(op.rgba8));
            const std::wstring &text = m_texts[i];
            const D2D1_RECT_F line = D2D1::RectF(op.x, op.y - kLineBaseline, op.x + kCanvasWidth, op.y - kLineBaseline + kLineHeight);
            m_dc->DrawText(text.c_str(), UINT32(text.size()), m_textFormat.Get(), line, m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                           DWRITE_MEASURING_MODE_NATURAL);
        }
        break;
    case NativeKind::Blits: {
        const float hw = float(m_ops.image.width) / 2, hh = float(m_ops.image.height) / 2;
        const D2D1_RECT_F dest = D2D1::RectF(-hw, -hh, hw, hh);
        for (const BlitOp &op : m_ops.blits) {
            // translate(c), rotate(r), scale(k), as Qt raster; bilinear (§7.1).
            m_dc->SetTransform(D2D1::Matrix3x2F::Scale(op.scale, op.scale) * D2D1::Matrix3x2F::Rotation(op.rotation)
                               * D2D1::Matrix3x2F::Translation(op.cx, op.cy) * canvas);
            m_dc->DrawBitmap(m_image.Get(), &dest, op.opacity, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
        }
        break;
    }
    case NativeKind::Gradients:
        m_dc->SetTransform(canvas);
        for (const GradientOp &op : m_ops.gradients) {
            const float l = op.cx - op.w / 2, tp = op.cy - op.h / 2;
            if (op.radial) {
                // Qt's object-bounding radial (centre 0.5, radius 0.5): the ellipse's own centre and radii.
                ID2D1RadialGradientBrush *b = m_radial[op.gradient].Get();
                b->SetCenter(D2D1::Point2F(op.cx, op.cy));
                b->SetRadiusX(op.w / 2);
                b->SetRadiusY(op.h / 2);
                m_dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(op.cx, op.cy), op.w / 2, op.h / 2), b);
            } else {
                // Qt's object-bounding linear (0, 0) -> (1, 1) in the unit box, which Qt then scales by (w, h): the
                // bands lie along x/w + y/h = c, also for a non-square box. The brush stays in unit space and its
                // transform (scale(w, h), then translate to the box) puts it there.
                ID2D1LinearGradientBrush *b = m_linear[op.gradient].Get();
                b->SetTransform(D2D1::Matrix3x2F::Scale(op.w, op.h) * D2D1::Matrix3x2F::Translation(l, tp));
                m_dc->FillRectangle(D2D1::RectF(l, tp, l + op.w, tp + op.h), b);
            }
        }
        break;
    }
    return endDraw("EndDraw (the ops)");
}

std::string Direct2DNative::compose(ID2D1Bitmap1 *target, std::uint32_t w, std::uint32_t h)
{
    // The overlay's pixels (rasterOverlay, premultiplied RGBA -> BGRA), remade when its text or scale changes.
    if (m_overlay != m_overlayDrawn || m_overlayScale != m_scale) {
        m_overlayBitmap.Reset();
        m_overlayDrawn = m_overlay;
        m_overlayScale = m_scale;
        if (!m_overlay.empty()) {
            OverlayLayout layout;
            std::vector<std::uint8_t> px = rasterOverlay(m_overlay, m_scale, &layout);
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
                std::swap(px[i], px[i + 2]);
            if (const HRESULT hr = m_dc->CreateBitmap(D2D1::SizeU(UINT32(layout.w), UINT32(layout.h)), px.data(), UINT32(layout.w) * 4,
                                                      bitmapProps(D2D1_BITMAP_OPTIONS_NONE), &m_overlayBitmap);
                FAILED(hr))
                return dxgi::failure("CreateBitmap (the overlay)", hr);
            m_overlayX = layout.x, m_overlayY = layout.y, m_overlayW = layout.w, m_overlayH = layout.h;
        }
    }
    m_dc->SetTarget(target);
    m_dc->BeginDraw();
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dc->Clear(kBackground);
    const D2D1_RECT_F window = D2D1::RectF(0, 0, float(w), float(h));
    m_dc->DrawBitmap(m_content.Get(), &window, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);  // the content scaled to the window
    // The overlay in physical px, unscaled, nearest, over the content (§5.9; Revised: at (layout.x, layout.y)).
    // One that doesn't fit the window is left out entirely (never a partial box), as the 3D present pass does.
    if (m_overlayBitmap && std::uint32_t(m_overlayX + m_overlayW) <= w && std::uint32_t(m_overlayY + m_overlayH) <= h) {
        const D2D1_RECT_F box = D2D1::RectF(float(m_overlayX), float(m_overlayY), float(m_overlayX + m_overlayW), float(m_overlayY + m_overlayH));
        m_dc->DrawBitmap(m_overlayBitmap.Get(), &box, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
    }
    return endDraw("EndDraw (the present)");
}

std::string Direct2DNative::waitSlot(std::size_t slot)
{
    if (!m_queryPending[slot])
        return {};
    m_queryPending[slot] = false;
    m_cx->Flush();
    const auto start = std::chrono::steady_clock::now();
    unsigned spins = 0;
    for (;;) {
        const HRESULT hr = m_cx->GetData(m_frameQueries[slot].Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK)
            return {};
        if (FAILED(hr))
            return dxgi::failure("GetData", hr);
        if (FAILED(m_device->GetDeviceRemovedReason()))
            return "LOST: the GPU driver reset (DXGI_ERROR_DEVICE_REMOVED)";
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10))  // P-STOP
            return "LOST: the GPU stopped responding";
        // L1: a short back-off, as the D3D11 backend's.
        if (++spins < 32)
            YieldProcessor();
        else if (spins < 4096) {
            if (!SwitchToThread())
                Sleep(0);
        } else {
            Sleep(1);
        }
    }
}

std::string Direct2DNative::waitIdle()
{
    for (std::size_t k = 0; k < m_frameQueries.size(); ++k) {
        if (std::string e = waitSlot(std::size_t((m_frame + k) % m_frameQueries.size())); !e.empty())
            return e;
    }
    return {};
}

std::string Direct2DNative::frame(double t)
{
    if (!m_loaded || !m_resumed)
        return "frame() before load() and resume()";
    // §5.4: frame n reuses frame n-2's event query only once the GPU has finished it (at most 2 frames queued).
    const std::size_t slot = std::size_t(m_frame % m_frameQueries.size());
    if (std::string e = waitSlot(slot); !e.empty())
        return e;
    if (!m_headless) {
        if (std::string e = m_swap.waitForFrame(); !e.empty())
            return e;
    }
    if (std::string e = drawContent(t); !e.empty())
        return e;
    if (std::string e = compose(m_windowTarget.Get(), m_windowW, m_windowH); !e.empty())
        return e;
    m_cx->End(m_frameQueries[slot].Get());
    m_queryPending[slot] = true;
    ++m_frame;
    if (!m_headless) {
        if (std::string e = m_swap.present(); !e.empty())
            return e;
    }
    return validationProblem();
}

std::string Direct2DNative::readBitmap(ID2D1Bitmap1 *source, std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t> *rgba8)
{
    // A CPU-readable copy (CPU_READ | CANNOT_DRAW) of a render-size target, mapped; BGRA -> RGBA. The
    // content is opaque (cleared to the background, everything drawn over it), so premultiplied = straight.
    m_dc->SetTarget(nullptr);  // the source isn't the target while it is copied
    D2D1_POINT_2U origin = D2D1::Point2U(0, 0);
    const D2D1_RECT_U all = D2D1::RectU(0, 0, w, h);
    HRESULT hr = m_readTarget->CopyFromBitmap(&origin, source, &all);
    if (FAILED(hr))
        return dxgi::failure("ID2D1Bitmap1::CopyFromBitmap (readback)", hr);
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(hr = m_readTarget->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
        return dxgi::failure("ID2D1Bitmap1::Map (readback)", hr);
    rgba8->resize(std::size_t(w) * h * 4);  // row 0 = the top
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t *src = mapped.bits + std::size_t(y) * mapped.pitch;
        std::uint8_t *dst = rgba8->data() + std::size_t(y) * w * 4;
        for (std::uint32_t x = 0; x < w; ++x) {
            dst[4 * x + 0] = src[4 * x + 2];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x + 0];
            dst[4 * x + 3] = src[4 * x + 3];
        }
    }
    m_readTarget->Unmap();
    return validationProblem();
}

std::string Direct2DNative::readback(double t, std::vector<std::uint8_t> *rgba8)
{
    if (!m_loaded)
        return "readback() before load()";
    // The verification frame: the content at t, render size, before the overlay (§5.7).
    if (std::string e = drawContent(t); !e.empty())
        return e;
    return readBitmap(m_content.Get(), m_init.renderWidth, m_init.renderHeight, rgba8);
}

#if defined(SYNTHEON_DEV_HOOKS)
std::string Direct2DNative::readbackPresented(std::vector<std::uint8_t> *rgba8)
{
    if (!m_headless || !m_resumed || !m_loaded || m_frame == 0)
        return "readbackPresented() needs a headless frame";
    return readBitmap(m_windowTarget.Get(), m_windowW, m_windowH, rgba8);
}
#endif

std::string Direct2DNative::validationProblem()
{
    if (m_info) {
        const UINT64 n = m_info->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(m_info->GetMessage(i, nullptr, &bytes)) || bytes == 0)
                continue;
            std::vector<unsigned char> buffer(bytes);
            auto *m = reinterpret_cast<D3D11_MESSAGE *>(buffer.data());
            if (FAILED(m_info->GetMessage(i, m, &bytes)))
                continue;
            if (m->Severity != D3D11_MESSAGE_SEVERITY_ERROR && m->Severity != D3D11_MESSAGE_SEVERITY_CORRUPTION)
                continue;
            const std::string text(m->pDescription ? m->pDescription : "(no message)");
            std::fprintf(stderr, "syntheon-gpu: d2d: %s\n", text.c_str());
            if (m_validationErrors++ == 0)
                m_firstValidationError = text;
        }
        m_info->ClearStoredMessages();
    }
    if (m_validationErrors == 0)
        return {};
    return "validation: " + m_firstValidationError;
}

} // namespace sb::gpu::d2d

namespace sb::gpu {
std::unique_ptr<IBackend> makeDirect2DBackend(std::vector<std::uint8_t> fontBytes)
{
    return std::make_unique<d2d::Direct2DNative>(std::move(fontBytes));
}
} // namespace sb::gpu

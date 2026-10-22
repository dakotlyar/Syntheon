// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Direct2D native 2D path (Phase 4a design §7.1, task 27), Windows only (P-WIN): an
// ID2D1DeviceContext on a D3D11 device with BGRA support on the selected adapter draws the shared op
// lists (native_ops.h) into an offscreen ID2D1Bitmap1 at render size; each frame that bitmap is drawn
// (linear) into the shared DXGI swapchain's back buffer, the overlay (rasterOverlay's pixels) on top,
// unscaled and nearest, then Present. Headless, a render-size target bitmap stands in for the back
// buffer (no present). Text is DirectWrite with the bundled IBM Plex Sans (an in-memory font
// collection). Shown, not scored (D8); a failure is only a warning in the GPU test (D23 a).
// It is both an INative2D (the workload) and an IBackend (the window flow the worker drives).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "tests/gpu/worker/backend.h"
#include "tests/gpu/worker/dxgi/dxgi_swapchain.h"
#include "tests/gpu/worker/native2d.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sb::gpu::d2d {

using Microsoft::WRL::ComPtr;

class Direct2DNative final : public IBackend, public INative2D
{
public:
    // fontBytes: IBM Plex Sans (the worker's resource); empty or unusable -> a system font and a warning.
    explicit Direct2DNative(std::vector<std::uint8_t> fontBytes);
    ~Direct2DNative() override;

    // IBackend: the window flow; the 3D entry points are refused (a native path draws NativeOps).
    Api api() const override { return Api::Direct2D; }
    std::string init(const BackendInit &init, const PresentTarget &target, BackendInfo *info) override;
    std::string load(const SceneData &) override { return "Direct2D draws the native 2D workloads only"; }
    std::string frame(double t, std::vector<double> *) override { return frame(t); }
    std::string drain(std::vector<double> *) override { return waitIdle(); }
    std::string readback(double t, std::vector<std::uint8_t> *rgba8) override;
    void unload() override;
    std::string suspend() override;
    std::string resume(const PresentTarget &target) override;
    void setOverlay(const std::string &text) override { m_overlay = text; }
    std::uint32_t lastFrameDraws() const override { return m_ops.opsPerFrame; }  // the ops of the last frame
    std::string presentMode() const override { return m_presentModeName; }
    void setRenderSize(std::uint32_t w, std::uint32_t h) override { m_init.renderWidth = w, m_init.renderHeight = h; }

    // INative2D
    std::string load(const NativeOps &ops) override;
    std::string frame(double t) override;
    const char *overlayName() const override { return "Direct2D"; }
    std::string warning() const override { return m_warning; }

    void shutdown();
    // D3D11 debug-layer errors seen so far (--validation).
    int validationErrors() const { return m_validationErrors; }
    // The family DirectWrite draws the text workload with ("IBM Plex Sans", or the fallback's).
    const std::wstring &fontFamily() const { return m_family; }

#if defined(SYNBENCH_DEV_HOOKS)
    // Dev hook (the overlay test): headless only, the last frame as the window would get it (the
    // render-size stand-in for the back buffer: the content, then the overlay), RGBA8 top row first.
    std::string readbackPresented(std::vector<std::uint8_t> *rgba8);
#endif

private:
    std::string createDevice(const BackendInit &init, BackendInfo *info);
    std::string createTextFormat();
    std::string drawContent(double t);              // the ops at t into m_content (render size)
    std::string compose(ID2D1Bitmap1 *target, std::uint32_t w, std::uint32_t h);  // content + overlay into the window target
    std::string readBitmap(ID2D1Bitmap1 *source, std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t> *rgba8);
    std::string endDraw(const char *what);          // D2D-2: EndDraw reports the errors; RECREATE_TARGET -> "LOST: ..."
    std::string waitSlot(std::size_t slot);         // §5.4: at most 2 frames queued
    std::string waitIdle() override;
    void releaseWindow();
    std::string validationProblem();

    BackendInit m_init;
    std::vector<std::uint8_t> m_fontBytes;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_cx;
    ComPtr<ID3D11InfoQueue> m_info;
    ComPtr<ID2D1Factory1> m_factory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_dc;
    ComPtr<IDWriteFactory> m_dwrite;
    ComPtr<IDWriteFontFileLoader> m_fontLoader;     // the registered in-memory loader (unregistered at shutdown)
    ComPtr<IDWriteFontCollection> m_fonts;          // null: the system collection
    ComPtr<IDWriteTextFormat> m_textFormat;
    std::wstring m_family;
    std::string m_warning;  // why the bundled font wasn't used ("" when it was): the `warning` event's text
    ComPtr<ID2D1PathGeometry> m_star;               // the unit 10-vertex star (outer radius 1, inner 0.45)
    std::array<ComPtr<ID3D11Query>, 2> m_frameQueries;
    std::array<bool, 2> m_queryPending{};
    std::uint64_t m_frame = 0;

    // The window (or the headless stand-in) and the workload.
    bool m_headless = false, m_resumed = false;
    dxgi::Swapchain m_swap;
    ComPtr<ID2D1Bitmap1> m_windowTarget;            // the back buffer (IDXGISurface) or the headless target
    std::uint32_t m_windowW = 0, m_windowH = 0;     // physical px
    double m_scale = 1.0;                           // the window's devicePixelRatio (the overlay's size)
    std::string m_presentModeName;
    NativeOps m_ops;
    bool m_loaded = false;
    ComPtr<ID2D1Bitmap1> m_content;                 // render size, TARGET
    ComPtr<ID2D1Bitmap1> m_readTarget;              // render size, CPU_READ (readback)
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<ID2D1Bitmap1> m_image;                   // the blit image, premultiplied BGRA
    std::array<ComPtr<ID2D1LinearGradientBrush>, 16> m_linear;
    std::array<ComPtr<ID2D1RadialGradientBrush>, 16> m_radial;
    std::vector<std::wstring> m_texts;              // the strings, widened once at load()
    std::string m_overlay, m_overlayDrawn;          // the text, and the text m_overlayBitmap holds
    double m_overlayScale = 0;
    ComPtr<ID2D1Bitmap1> m_overlayBitmap;
    int m_overlayX = 0, m_overlayY = 0, m_overlayW = 0, m_overlayH = 0;
    int m_validationErrors = 0;
    std::string m_firstValidationError;
};

} // namespace sb::gpu::d2d

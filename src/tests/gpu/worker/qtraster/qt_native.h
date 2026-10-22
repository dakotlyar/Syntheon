// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Qt raster native 2D path (Phase 4a design §7.1, task 26): QPainter on a QImage
// (ARGB32_Premultiplied) at render size, with Antialiasing, SmoothPixmapTransform and
// TextAntialiasing, drawing the shared op lists (native_ops.h). Every frame is presented through
// the window's QBackingStore (the content scaled to the window, then the overlay unscaled on top);
// headless there is no present. CPU only, every OS; this is the scored native 2D path (D8).
// It is both an INative2D (the workload) and an IBackend (the window flow the worker drives).

#include "tests/gpu/worker/backend.h"
#include "tests/gpu/worker/native2d.h"

#include <QBackingStore>
#include <QBrush>
#include <QFont>
#include <QImage>
#include <QPolygonF>
#include <QWindow>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace sb::gpu {

class QtNative final : public IBackend, public INative2D
{
public:
    // window: the shown RasterSurface window (null: headless, no present).
    explicit QtNative(QWindow *window);
    ~QtNative() override;

    // IBackend: the window flow; the 3D entry points are refused (a native path draws NativeOps).
    Api api() const override { return Api::QtRaster; }
    std::string init(const BackendInit &init, const PresentTarget &target, BackendInfo *info) override;
    std::string load(const SceneData &) override { return "Qt raster draws the native 2D workloads only"; }
    std::string frame(double t, std::vector<double> *) override { return frame(t); }
    std::string drain(std::vector<double> *) override { return {}; }
    std::string readback(double t, std::vector<std::uint8_t> *rgba8) override;
    void unload() override;
    std::string suspend() override;
    std::string resume(const PresentTarget &target) override;
    void setOverlay(const std::string &text) override { m_overlay = text; }
    std::uint32_t lastFrameDraws() const override { return m_ops.opsPerFrame; }  // the ops of the last frame
    std::string presentMode() const override { return m_window ? "backing store" : std::string(); }
    void setRenderSize(std::uint32_t w, std::uint32_t h) override { m_init.renderWidth = w, m_init.renderHeight = h; }

    // INative2D
    std::string load(const NativeOps &ops) override;
    std::string frame(double t) override;
    const char *overlayName() const override { return "Qt raster"; }

#if defined(SYNBENCH_DEV_HOOKS)
    // Dev hook (task 26's overlay test): the image the last frame() presented (the one the backing store flushes), at
    // the window's physical size: the content scaled to the window with the overlay on top. Headless dev builds compose
    // it too, at the render size.
    QImage presentedImage() const { return m_presented; }
#endif

private:
    void drawOps(double t);
    // The frame as presented: the content scaled to the window (smooth), then the overlay unscaled at (layout.x,
    // layout.y), SourceOver, in physical px, into m_presented. The backing store flushes this very image.
    void compose(std::uint32_t w, std::uint32_t h);
    // The overlay box for the text and scale, rebuilt only when either changes (L1: not per frame).
    void refreshOverlay();

    BackendInit m_init;
    QWindow *m_window = nullptr;
    std::unique_ptr<QBackingStore> m_store;
    double m_scale = 1.0;  // the window's devicePixelRatio (the overlay's size)
    std::uint32_t m_windowW = 0, m_windowH = 0;  // physical px
    NativeOps m_ops;
    bool m_loaded = false;
    QImage m_content;  // render size, ARGB32_Premultiplied
    QImage m_image;    // the blit image, premultiplied
    QFont m_font;
    std::array<QBrush, 16> m_linear, m_radial;
    QPolygonF m_star;  // the unit 10-vertex star (outer radius 1, inner 0.45)
    std::string m_overlay;
    QImage m_presented;                // the composed frame (physical px, device pixel ratio 1 until flushed)
    QImage m_overlayBox;               // rasterOverlay(m_overlayDrawn, m_overlayScale), premultiplied; null = none
    std::string m_overlayDrawn;
    double m_overlayScale = 0;
    int m_overlayX = 0, m_overlayY = 0;
    std::vector<QString> m_texts;      // the text ops' strings, converted once at load()
    int m_fontId = -1;
};

} // namespace sb::gpu

namespace sb::gpu {
// For the worker: the Qt raster path as a backend for a window (null: headless).
std::unique_ptr<IBackend> makeQtRasterBackend(QWindow *window);
} // namespace sb::gpu

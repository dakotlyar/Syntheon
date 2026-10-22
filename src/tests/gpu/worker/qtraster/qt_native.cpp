// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/qtraster/qt_native.h"

#include "tests/gpu/worker/scene/overlay.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QRegion>

#include <cmath>
#include <cstring>

namespace sb::gpu {

namespace {

constexpr QRgb kBackground = qRgb(18, 20, 26);
constexpr char kFontResource[] = ":/synbench-gpu/fonts/IBMPlexSans-Variable.ttf";

QColor colourOf(std::uint32_t rgba8)
{
    return QColor(int(rgba8 & 0xff), int((rgba8 >> 8) & 0xff), int((rgba8 >> 16) & 0xff), int(rgba8 >> 24));
}

QPolygonF unitStar()
{
    constexpr double kPi = 3.14159265358979323846;
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const double r = (i % 2 == 0) ? 1.0 : 0.45;
        const double a = -kPi / 2 + double(i) * kPi / 5;
        star << QPointF(r * std::cos(a), r * std::sin(a));
    }
    return star;
}

} // namespace

QtNative::QtNative(QWindow *window) : m_window(window) {}

QtNative::~QtNative() = default;

std::string QtNative::init(const BackendInit &init, const PresentTarget &, BackendInfo *info)
{
    m_init = init;
    // IBM Plex Sans from the worker's resources (the src/app/fonts.cpp pattern): the text workload's font.
    m_fontId = QFontDatabase::addApplicationFont(QLatin1StringView(kFontResource));
    const QStringList families = m_fontId >= 0 ? QFontDatabase::applicationFontFamilies(m_fontId) : QStringList();
    if (families.isEmpty())
        return "the bundled font IBM Plex Sans couldn't be loaded";
    m_font = QFont(families.first());
    m_font.setPixelSize(14);  // 14 logical px; the painter's scale(s) makes it physical
    m_font.setStyleStrategy(QFont::PreferAntialias);
    m_star = unitStar();
    if (info) {
        info->deviceName = "CPU";
        info->apiVersion = std::string("Qt ") + qVersion() + " raster";
        info->driver.clear();
        info->vendorId = init.vendorId;
        info->deviceId = init.deviceId;
        info->gpuTimestamps = false;
        info->presentMode.clear();
    }
    return {};
}

std::string QtNative::load(const NativeOps &ops)
{
    if (m_init.renderWidth == 0 || m_init.renderHeight == 0)
        return "Qt raster has no render size";
    m_ops = ops;
    m_content = QImage(int(m_init.renderWidth), int(m_init.renderHeight), QImage::Format_ARGB32_Premultiplied);
    if (m_content.isNull())
        return "Qt raster couldn't allocate the " + std::to_string(m_init.renderWidth) + "x" + std::to_string(m_init.renderHeight) + " image";
    m_content.fill(kBackground);
    if (ops.kind == NativeKind::Blits) {
        const QImage straight(ops.image.rgba8.data(), int(ops.image.width), int(ops.image.height), int(ops.image.width) * 4, QImage::Format_RGBA8888);
        m_image = straight.convertToFormat(QImage::Format_ARGB32_Premultiplied);  // owns its pixels
    }
    if (ops.kind == NativeKind::Gradients) {
        // The 16 definitions, prebuilt once: object-bounding coordinates, so one brush serves every size.
        for (std::size_t g = 0; g < m_linear.size(); ++g) {
            const auto &stops = ops.gradientStops[g];
            QLinearGradient linear(0.0, 0.0, 1.0, 1.0);
            linear.setCoordinateMode(QGradient::ObjectBoundingMode);
            for (int s = 0; s < 4; ++s)
                linear.setColorAt(double(s) / 3.0, colourOf(stops[std::size_t(s)]));
            m_linear[g] = QBrush(linear);
            QRadialGradient radial(0.5, 0.5, 0.5);
            radial.setCoordinateMode(QGradient::ObjectBoundingMode);
            for (int s = 0; s < 3; ++s)
                radial.setColorAt(double(s) / 2.0, colourOf(stops[std::size_t(s)]));
            m_radial[g] = QBrush(radial);
        }
    }
    m_loaded = true;
    return {};
}

void QtNative::unload()
{
    m_loaded = false;
    m_content = QImage();
    m_image = QImage();
    m_ops = NativeOps{};
}

std::string QtNative::suspend()
{
    m_store.reset();  // the backing store belongs to the shown window
    return {};
}

std::string QtNative::resume(const PresentTarget &target)
{
    m_store.reset();
    m_scale = target.scale > 0 ? target.scale : 1.0;
    m_windowW = target.windowWidth;
    m_windowH = target.windowHeight;
    if (m_window) {
        if (m_windowW == 0 || m_windowH == 0)
            return "the test window has no size";
        m_store = std::make_unique<QBackingStore>(m_window);
    }
    return {};
}

void QtNative::drawOps(double t)
{
    QPainter p(&m_content);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(m_content.rect(), QColor(kBackground));
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    // The canvas is 1920 x 1080 logical px, scaled to the render size, under the global translation (§7.1).
    const double s = double(m_content.width()) / double(kCanvasWidth);
    p.scale(s, s);
    const NativeShift shift = nativeTranslation(t);
    p.translate(double(shift.x), double(shift.y));

    switch (m_ops.kind) {
    case NativeKind::Shapes:
        for (const ShapeOp &op : m_ops.shapes) {
            p.save();
            p.translate(double(op.cx), double(op.cy));
            p.rotate(double(op.rotation));
            const QColor colour = colourOf(op.rgba8);
            const QRectF box(-double(op.w) / 2, -double(op.h) / 2, double(op.w), double(op.h));
            switch (op.type) {
            case 0:
                p.fillRect(box, colour);
                break;
            case 1:
                p.setPen(Qt::NoPen);
                p.setBrush(colour);
                p.drawEllipse(box);
                break;
            case 2:
                p.setPen(QPen(colour, 2.0));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(box, 8.0, 8.0);
                break;
            default:
                p.setPen(Qt::NoPen);
                p.setBrush(colour);
                p.scale(double(op.w) / 2, double(op.w) / 2);
                p.drawPolygon(m_star);
                break;
            }
            p.restore();
        }
        break;
    case NativeKind::Text:
        p.setFont(m_font);
        for (const TextOp &op : m_ops.texts) {
            p.setPen(colourOf(op.rgba8));
            p.drawText(QPointF(double(op.x), double(op.y)), QString::fromLatin1(op.text.data(), qsizetype(op.text.size())));
        }
        break;
    case NativeKind::Blits:
        for (const BlitOp &op : m_ops.blits) {
            p.save();
            p.translate(double(op.cx), double(op.cy));
            p.rotate(double(op.rotation));
            p.scale(double(op.scale), double(op.scale));
            p.setOpacity(double(op.opacity));
            p.drawImage(QPointF(-double(m_image.width()) / 2, -double(m_image.height()) / 2), m_image);
            p.restore();
        }
        break;
    case NativeKind::Gradients:
        p.setPen(Qt::NoPen);
        for (const GradientOp &op : m_ops.gradients) {
            const QRectF box(double(op.cx) - double(op.w) / 2, double(op.cy) - double(op.h) / 2, double(op.w), double(op.h));
            if (op.radial) {
                p.setBrush(m_radial[op.gradient]);
                p.drawEllipse(box);
            } else {
                p.fillRect(box, m_linear[op.gradient]);
            }
        }
        break;
    }
}

QImage QtNative::overlayImage(std::uint32_t w, std::uint32_t h, int *x, int *y) const
{
    if (m_overlay.empty())
        return {};
    OverlayLayout layout;
    const std::vector<std::uint8_t> pixels = rasterOverlay(m_overlay, m_scale, &layout);
    // An overlay that doesn't fit the window is left out entirely (never a partial box), as the 3D present pass does.
    if (std::uint32_t(layout.x + layout.w) > w || std::uint32_t(layout.y + layout.h) > h)
        return {};
    *x = layout.x;
    *y = layout.y;
    const QImage box(pixels.data(), layout.w, layout.h, layout.w * 4, QImage::Format_RGBA8888_Premultiplied);
    return box.copy();  // owns its pixels
}

std::string QtNative::frame(double t)
{
    if (!m_loaded)
        return "frame() before load()";
    drawOps(t);
    if (!m_store || !m_window)
        return {};  // headless: nothing is presented
    const QSize logical = m_window->size();
    if (logical.isEmpty())
        return "the test window has no size";
    m_store->resize(logical);
    const QRegion region(QRect(QPoint(0, 0), logical));
    m_store->beginPaint(region);
    {
        QPainter p(m_store->paintDevice());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(QRectF(QPointF(0, 0), QSizeF(logical)), m_content);  // the content scaled to the window
        int ox = 0, oy = 0;
        const QImage overlay = overlayImage(m_windowW, m_windowH, &ox, &oy);
        if (!overlay.isNull()) {
            // The overlay in physical px, unscaled, nearest, over the content (§5.9; Revised: SourceOver).
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.scale(1.0 / m_scale, 1.0 / m_scale);
            p.drawImage(QPoint(ox, oy), overlay);
        }
    }
    m_store->endPaint();
    m_store->flush(region);
    return {};
}

std::string QtNative::readback(double t, std::vector<std::uint8_t> *rgba8)
{
    if (!m_loaded)
        return "readback() before load()";
    drawOps(t);  // the verification frame: the content at render size, before the overlay
    const QImage out = m_content.convertToFormat(QImage::Format_RGBA8888);
    rgba8->resize(std::size_t(out.width()) * std::size_t(out.height()) * 4);
    for (int y = 0; y < out.height(); ++y)
        std::memcpy(rgba8->data() + std::size_t(y) * std::size_t(out.width()) * 4, out.constScanLine(y), std::size_t(out.width()) * 4);
    return {};
}

#if defined(SYNBENCH_DEV_HOOKS)
QImage QtNative::presentedImage()
{
    // Composed as frame() does, into an image of the window's physical size (headless: the render size).
    const std::uint32_t w = m_window ? m_windowW : std::uint32_t(m_content.width());
    const std::uint32_t h = m_window ? m_windowH : std::uint32_t(m_content.height());
    QImage out(int(w), int(h), QImage::Format_ARGB32_Premultiplied);
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(QRectF(0, 0, double(w), double(h)), m_content);
    int ox = 0, oy = 0;
    const QImage overlay = overlayImage(w, h, &ox, &oy);
    if (!overlay.isNull()) {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(QPoint(ox, oy), overlay);
    }
    return out;
}
#endif

std::unique_ptr<IBackend> makeQtRasterBackend(QWindow *window)
{
    return std::make_unique<QtNative>(window);
}

} // namespace sb::gpu

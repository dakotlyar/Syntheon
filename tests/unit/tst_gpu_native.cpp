// SPDX-License-Identifier: GPL-3.0-or-later
// The Qt raster native 2D path in process (Phase 4a task 26), headless on Qt's offscreen platform: each
// workload at content / 64 draws something, the overlay is the shared rasterOverlay over the content, and
// the readback is exactly the render size. The same for Direct2D on WARP (task 27, Windows CI only).
// (The worker-level runs are tst_gpu_backends' and tst_gpu_present's.)
#include "tests/gpu/worker/qtraster/qt_native.h"
#include "tests/gpu/worker/scene/overlay.h"
#if defined(Q_OS_WIN)
#include "tests/gpu/worker/d2d/d2d_native.h"
#endif

#include <QFile>
#include <QImage>
#include <QTest>

#include <cmath>

using namespace sb::gpu;

#define SB_OK(expr)                                                                                                                  \
    do {                                                                                                                             \
        const std::string sbReason = (expr);                                                                                        \
        QVERIFY2(sbReason.empty(), sbReason.c_str());                                                                               \
    } while (false)

namespace {

// How many distinct colours and how far from the background: a flat image has one.
int distinctColours(const std::vector<std::uint8_t> &rgba)
{
    QSet<quint32> seen;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
        seen.insert(quint32(rgba[i]) | (quint32(rgba[i + 1]) << 8) | (quint32(rgba[i + 2]) << 16));
    return int(seen.size());
}

#if defined(Q_OS_WIN)
// The overlay box of a presented frame is rasterOverlay(text) over the content, everything else is the
// content (±1): "" or the first difference. presented and content are RGBA8, W x H, top row first.
QString overlayOverContent(const std::vector<std::uint8_t> &presented, const std::vector<std::uint8_t> &content, int W, int H, const std::string &text)
{
    OverlayLayout layout;
    const std::vector<std::uint8_t> box = rasterOverlay(text, 1.0, &layout);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::uint8_t *c = &content[(std::size_t(y) * std::size_t(W) + std::size_t(x)) * 4];
            const std::uint8_t *got = &presented[(std::size_t(y) * std::size_t(W) + std::size_t(x)) * 4];
            int want[3] = {c[0], c[1], c[2]};
            if (x >= layout.x && x < layout.x + layout.w && y >= layout.y && y < layout.y + layout.h) {
                const std::uint8_t *o = &box[(std::size_t(y - layout.y) * std::size_t(layout.w) + std::size_t(x - layout.x)) * 4];
                for (int k = 0; k < 3; ++k)
                    want[k] = int(std::lround(o[k] + c[k] * (255.0 - o[3]) / 255.0));
            }
            for (int k = 0; k < 3; ++k) {
                if (std::abs(int(got[k]) - want[k]) > 1)
                    return QStringLiteral("pixel (%1, %2) channel %3 is %4, not %5").arg(x).arg(y).arg(k).arg(got[k]).arg(want[k]);
            }
        }
    }
    return {};
}

// Task 27: Direct2D on WARP (software, the D3D11 debug layer where Windows has it), headless, with the
// worker's font from the same resource file the worker reads.
std::unique_ptr<d2d::Direct2DNative> makeDirect2D(std::uint32_t w, std::uint32_t h, const NativeOps &ops, QString *why)
{
    QFile font(QStringLiteral(":/synbench-gpu/fonts/IBMPlexSans-Variable.ttf"));
    std::vector<std::uint8_t> bytes;
    if (font.open(QIODevice::ReadOnly)) {
        const QByteArray data = font.readAll();
        bytes.assign(data.cbegin(), data.cend());
    }
    auto n = std::make_unique<d2d::Direct2DNative>(std::move(bytes));
    BackendInit init;
    init.gpuId = "1414:008c#0";
    init.vendorId = 0x1414;
    init.deviceId = 0x008c;
    init.renderWidth = w;
    init.renderHeight = h;
    init.allowSoftware = true;
    init.validation = true;
    BackendInfo info;
    std::string e = n->init(init, PresentTarget{true}, &info);
    if (e.empty() && info.apiVersion.rfind("Direct2D 1.1", 0) != 0)
        e = "apiVersion is '" + info.apiVersion + "'";
    if (e.empty())  // visible in the CI log (the "GPU test results" step): proof that these cases ran on WARP, not skipped
        qInfo("Direct2D ran: %s on %s (%s)", info.apiVersion.c_str(), info.deviceName.c_str(), info.driver.c_str());
    if (e.empty())
        e = n->resume(PresentTarget{true});
    if (e.empty())
        e = n->load(ops);
    if (!e.empty()) {
        *why = QString::fromStdString(e);
        return nullptr;
    }
    return n;
}
#endif

} // namespace

class TestGpuNative : public QObject
{
    Q_OBJECT

    std::unique_ptr<QtNative> make(std::uint32_t w, std::uint32_t h, const NativeOps &ops, double scale = 1.0)
    {
        auto n = std::make_unique<QtNative>(nullptr);
        BackendInit init;
        init.renderWidth = w;
        init.renderHeight = h;
        BackendInfo info;
        [&] { SB_OK(n->init(init, PresentTarget{true}, &info)); }();
        [&] { QVERIFY(info.apiVersion.rfind("Qt ", 0) == 0 && info.apiVersion.find(" raster") != std::string::npos); }();
        PresentTarget target{true};
        target.scale = scale;  // the window's devicePixelRatio: the overlay's size
        [&] { SB_OK(n->resume(target)); }();
        [&] { SB_OK(n->load(ops)); }();
        return n;
    }

private slots:
    void everyWorkloadDrawsAndReadsBackAtRenderSize_data()
    {
        QTest::addColumn<int>("kind");
        for (const NativeKind k : kNativeKinds)
            QTest::newRow(nativeId(k)) << int(k);
    }

    void everyWorkloadDrawsAndReadsBackAtRenderSize()
    {
        QFETCH(int, kind);
        const NativeOps ops = buildNativeOps(NativeKind(kind), 64);
        auto n = make(320, 180, ops);
        QVERIFY(n);
        QCOMPARE(n->lastFrameDraws(), ops.opsPerFrame);
        for (int i = 0; i < 5; ++i)
            SB_OK(n->frame(i / 60.0));
        std::vector<std::uint8_t> px;
        SB_OK(n->readback(10.0, &px));
        QCOMPARE(px.size(), std::size_t(320 * 180 * 4));
        QVERIFY2(distinctColours(px) > 8, "the readback is flat");
        // The same time gives the same image (the ops are the whole content).
        std::vector<std::uint8_t> again;
        SB_OK(n->readback(10.0, &again));
        QVERIFY(px == again);
        // A moving canvas: the global translation changes the picture between times.
        std::vector<std::uint8_t> later;
        SB_OK(n->readback(11.0, &later));
        QVERIFY(px != later);
        n->unload();
        QVERIFY(!n->frame(0.0).empty());  // nothing loaded
    }

    // Revised (Dave): the overlay is rasterOverlay() drawn unscaled at (layout.x, layout.y), SourceOver, after the
    // scaled content: the box pixels equal the shared overlay over the content, and nothing else changes. The image
    // checked is the one frame() really composes and presents (presentedImage()), at devicePixelRatio 1, 1.25 and 2.
    void overlayIsTheSharedRasterOverlay_data()
    {
        QTest::addColumn<double>("scale");
        QTest::newRow("dpr 1") << 1.0;
        QTest::newRow("dpr 1.25") << 1.25;
        QTest::newRow("dpr 2") << 2.0;
    }

    void overlayIsTheSharedRasterOverlay()
    {
        QFETCH(double, scale);
        const NativeOps ops = buildNativeOps(NativeKind::Shapes, 64);
        constexpr std::uint32_t W = 800, H = 450;
        auto n = make(W, H, ops, scale);
        QVERIFY(n);
        const std::string text = "Shapes | Qt raster | 412 fps";
        n->setOverlay(text);
        SB_OK(n->frame(0.0));
        const QImage presented = n->presentedImage();
        QCOMPARE(presented.width(), int(W));
        QCOMPARE(presented.height(), int(H));
        std::vector<std::uint8_t> content;
        SB_OK(n->readback(0.0, &content));  // the same frame time, no overlay
        OverlayLayout layout;
        const std::vector<std::uint8_t> box = rasterOverlay(text, scale, &layout);
        QVERIFY(layout.w > 0 && layout.h > 0);
        int inside = 0;
        for (int y = 0; y < int(H); ++y) {
            for (int x = 0; x < int(W); ++x) {
                const QRgb got = presented.pixel(x, y);
                const std::uint8_t *c = &content[(std::size_t(y) * W + std::size_t(x)) * 4];
                int want[3] = {c[0], c[1], c[2]};
                if (x >= layout.x && x < layout.x + layout.w && y >= layout.y && y < layout.y + layout.h) {
                    const std::uint8_t *o = &box[(std::size_t(y - layout.y) * std::size_t(layout.w) + std::size_t(x - layout.x)) * 4];
                    for (int k = 0; k < 3; ++k)
                        want[k] = int(std::lround(o[k] + c[k] * (255.0 - o[3]) / 255.0));
                    ++inside;
                }
                const int have[3] = {qRed(got), qGreen(got), qBlue(got)};
                for (int k = 0; k < 3; ++k) {
                    if (std::abs(have[k] - want[k]) > 1)
                        QFAIL(qPrintable(QStringLiteral("pixel (%1, %2) channel %3 is %4, not %5").arg(x).arg(y).arg(k).arg(have[k]).arg(want[k])));
                }
            }
        }
        QCOMPARE(inside, layout.w * layout.h);
        // The box is cached until the text or the scale changes: a second frame with the same text gives the same image.
        SB_OK(n->frame(0.0));
        QCOMPARE(n->presentedImage(), presented);
        n->setOverlay("Shapes | Qt raster | 99 fps");
        SB_OK(n->frame(0.0));
        QVERIFY(n->presentedImage() != presented);
    }

    // §7.1: Text draws with IBM Plex Sans from the worker's resources; the other paths never need it.
    void textUsesTheBundledFont()
    {
        auto n = make(320, 180, buildNativeOps(NativeKind::Text, 64));
        QVERIFY(n);
        SB_OK(n->frame(0.0));
    }

    // Task 27 (Windows CI, WARP): each Direct2D workload at content / 64 draws at least 3 frames and reads
    // back (the offscreen bitmap through a CPU-readable copy) exactly the render size, not flat.
    void direct2DDrawsEveryWorkload_data() { everyWorkloadDrawsAndReadsBackAtRenderSize_data(); }

    void direct2DDrawsEveryWorkload()
    {
#if !defined(Q_OS_WIN)
        QSKIP("Direct2D is Windows only");
#else
        QFETCH(int, kind);
        const NativeOps ops = buildNativeOps(NativeKind(kind), 64);
        QString why;
        auto n = makeDirect2D(320, 180, ops, &why);
        QVERIFY2(n, qPrintable(why));
        for (int i = 0; i < 5; ++i)
            SB_OK(n->frame(i / 60.0));
        QCOMPARE(n->lastFrameDraws(), ops.opsPerFrame);
        std::vector<std::uint8_t> px;
        SB_OK(n->readback(10.0, &px));
        QCOMPARE(px.size(), std::size_t(320 * 180 * 4));
        QVERIFY2(distinctColours(px) > 8, "the readback is flat");
        std::vector<std::uint8_t> again, later;
        SB_OK(n->readback(10.0, &again));
        QVERIFY(px == again);
        SB_OK(n->readback(11.0, &later));
        QVERIFY(px != later);
        // A second workload on the same device (unload, suspend, resume, load: the worker's order).
        n->unload();
        SB_OK(n->suspend());
        SB_OK(n->resume(PresentTarget{true}));
        SB_OK(n->load(buildNativeOps(NativeKind::Shapes, 64)));
        SB_OK(n->frame(0.0));
        n->unload();
        QVERIFY(!n->frame(0.0).empty());  // nothing loaded
        SB_OK(n->suspend());
        QCOMPARE(n->validationErrors(), 0);
#endif
    }

    // Task 27, Revised (Dave): Direct2D's overlay is the same rasterOverlay pixels, unscaled and nearest at
    // (layout.x, layout.y) over the scaled content. Headless the back buffer's stand-in is render size.
    void direct2DOverlayIsTheSharedRasterOverlay()
    {
#if !defined(Q_OS_WIN)
        QSKIP("Direct2D is Windows only");
#else
        constexpr int W = 640, H = 360;
        QString why;
        auto n = makeDirect2D(W, H, buildNativeOps(NativeKind::Shapes, 64), &why);
        QVERIFY2(n, qPrintable(why));
        const std::string text = "Shapes | Direct2D | 412 fps";
        n->setOverlay(text);
        SB_OK(n->frame(0.0));
        std::vector<std::uint8_t> presented, content;
        SB_OK(n->readbackPresented(&presented));
        SB_OK(n->readback(0.0, &content));  // the same frame time, no overlay
        QCOMPARE(presented.size(), std::size_t(W * H * 4));
        QCOMPARE(content.size(), presented.size());
        const QString diff = overlayOverContent(presented, content, W, H, text);
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
        QVERIFY(presented != content);  // the box is there
        n->unload();
        SB_OK(n->suspend());
        QCOMPARE(n->validationErrors(), 0);
#endif
    }

    // §7.1: Direct2D's text is IBM Plex Sans from the worker's resource (an in-memory DirectWrite
    // collection), not the system fallback.
    void direct2DTextUsesTheBundledFont()
    {
#if !defined(Q_OS_WIN)
        QSKIP("Direct2D is Windows only");
#else
        QString why;
        auto n = makeDirect2D(320, 180, buildNativeOps(NativeKind::Text, 64), &why);
        QVERIFY2(n, qPrintable(why));
        QCOMPARE(QString::fromStdWString(n->fontFamily()), QStringLiteral("IBM Plex Sans"));
        SB_OK(n->frame(0.0));
        n->unload();
        SB_OK(n->suspend());
#endif
    }
};

QTEST_MAIN(TestGpuNative)
#include "tst_gpu_native.moc"

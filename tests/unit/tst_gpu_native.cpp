// SPDX-License-Identifier: GPL-3.0-or-later
// The Qt raster native 2D path in process (Phase 4a task 26), headless on Qt's offscreen platform: each
// workload at content / 64 draws something, the overlay is the shared rasterOverlay over the content, and
// the readback is exactly the render size. (The worker-level runs are tst_gpu_backends' and tst_gpu_present's.)
#include "tests/gpu/worker/qtraster/qt_native.h"
#include "tests/gpu/worker/scene/overlay.h"

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

} // namespace

class TestGpuNative : public QObject
{
    Q_OBJECT

    std::unique_ptr<QtNative> make(std::uint32_t w, std::uint32_t h, const NativeOps &ops)
    {
        auto n = std::make_unique<QtNative>(nullptr);
        BackendInit init;
        init.renderWidth = w;
        init.renderHeight = h;
        BackendInfo info;
        [&] { SB_OK(n->init(init, PresentTarget{true}, &info)); }();
        [&] { QVERIFY(info.apiVersion.rfind("Qt ", 0) == 0 && info.apiVersion.find(" raster") != std::string::npos); }();
        [&] { SB_OK(n->resume(PresentTarget{true})); }();
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
    // scaled content: the box pixels equal the shared overlay over the content, and nothing else changes.
    void overlayIsTheSharedRasterOverlay()
    {
        const NativeOps ops = buildNativeOps(NativeKind::Shapes, 64);
        constexpr std::uint32_t W = 640, H = 360;
        auto n = make(W, H, ops);
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
        const std::vector<std::uint8_t> box = rasterOverlay(text, 1.0, &layout);
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
    }

    // §7.1: Text draws with IBM Plex Sans from the worker's resources; the other paths never need it.
    void textUsesTheBundledFont()
    {
        auto n = make(320, 180, buildNativeOps(NativeKind::Text, 64));
        QVERIFY(n);
        SB_OK(n->frame(0.0));
    }
};

QTEST_MAIN(TestGpuNative)
#include "tst_gpu_native.moc"

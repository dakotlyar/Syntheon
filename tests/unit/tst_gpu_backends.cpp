// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU backends on software drivers (Phase 4a task 10: Vulkan on lavapipe,
// headless, with the Khronos validation layer and synchronisation
// validation; any validation error fails). The Vulkan cases are skipped where
// lavapipe isn't installed (Windows; task 16 makes it a CI requirement).
#include "gpu_worker_process.h"

#include "tests/gpu/worker/scene/overlay.h"
#include "tests/gpu/worker/scene/scenes.h"
#include "tests/gpu/worker/signature.h"
#include "tests/gpu/worker/vulkan/vk_backend.h"
#include "tests/gpu/worker/vulkan/vk_swapchain.h"

#include <QTest>

#include <cmath>
#include <cstdlib>
#include <memory>

using namespace sb::gpu;
using namespace sb::gpu::testing;

namespace {

const QString kWorker = QStringLiteral(SB_GPU_WORKER);

// A backend call that must succeed; its reason is the failure message.
#define SB_OK(expr)                                                                                                                  \
    do {                                                                                                                             \
        const std::string sbReason = (expr);                                                                                        \
        QVERIFY2(sbReason.empty(), sbReason.c_str());                                                                               \
    } while (false)
constexpr std::uint8_t kClear[4] = {64, 128, 192, 255};

BackendInit lavapipeInit(std::uint32_t w, std::uint32_t h)
{
    BackendInit init;
    init.gpuId = kLavapipeId.toStdString();
    init.vendorId = kLavapipeVendor;
    init.deviceId = 0;
    init.renderWidth = w;
    init.renderHeight = h;
    init.allowSoftware = true;
    init.validation = true;
    return init;
}

} // namespace

class TestGpuBackends : public QObject
{
    Q_OBJECT

    // In-process Vulkan: the backend at w x h on lavapipe, headless, loaded
    // with a content-scaled scene (the workloads that are still the clear
    // workload: Quads, Sprites, Shading, Particles) and resumed. Null (and the test skipped) without lavapipe.
    std::unique_ptr<vk::VulkanBackend> headlessVulkan(std::uint32_t w, std::uint32_t h, const SceneData &scene, QString *skip)
    {
        auto b = std::make_unique<vk::VulkanBackend>();
        BackendInfo info;
        const std::string e = b->init(lavapipeInit(w, h), PresentTarget{true}, &info);
        if (e.rfind("N/A: ", 0) == 0) {
            *skip = QStringLiteral("no software Vulkan driver: %1").arg(QString::fromStdString(e));
            return nullptr;
        }
        if (!e.empty()) {
            *skip = QString::fromStdString("init: " + e);  // a failure, not a skip: the caller QFAILs
            return nullptr;
        }
        if (const std::string r = b->resume(PresentTarget{true}); !r.empty()) {
            *skip = QString::fromStdString("resume: " + r);
            return nullptr;
        }
        if (const std::string l = b->load(scene); !l.empty()) {
            *skip = QString::fromStdString("load: " + l);
            return nullptr;
        }
        return b;
    }

private slots:
    void initTestCase()
    {
        // The in-process cases load the Vulkan loader in this process: only
        // lavapipe, and no implicit layers (overlays, recorders) (§4.4).
        if (!lavapipeIcd().isEmpty()) {
            qputenv("VK_DRIVER_FILES", lavapipeIcd().toLocal8Bit());
            qputenv("VK_ICD_FILENAMES", lavapipeIcd().toLocal8Bit());
            qputenv("VK_LOADER_LAYERS_DISABLE", "~implicit~");
        }
    }

    // The pure case: §5.5's order, IMMEDIATE > MAILBOX, FIFO-only is N/A.
    void choosePresentMode()
    {
        const std::string na = "N/A: Vsync can't be turned off (only FIFO present mode)";
        VkPresentModeKHR m{};
        SB_OK(vk::choosePresentMode({VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}, &m));
        QCOMPARE(m, VK_PRESENT_MODE_IMMEDIATE_KHR);
        QCOMPARE(QLatin1StringView(vk::presentModeName(m)), QLatin1StringView("IMMEDIATE"));
        SB_OK(vk::choosePresentMode({VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR}, &m));
        QCOMPARE(m, VK_PRESENT_MODE_MAILBOX_KHR);
        QCOMPARE(QLatin1StringView(vk::presentModeName(m)), QLatin1StringView("MAILBOX"));
        QCOMPARE(vk::choosePresentMode({VK_PRESENT_MODE_FIFO_KHR}, &m), na);
        QCOMPARE(vk::choosePresentMode({VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR}, &m), na);
        QCOMPARE(vk::choosePresentMode({}, &m), na);
    }

    // 30 frames of the clear workload; every read-back pixel is the clear
    // colour, the GPU times arrive in order, and the validation layer saw nothing.
    void vulkanClear()
    {
        QString why;
        const SceneData scene = buildScene(Workload::Quads, 64);
        auto b = headlessVulkan(320, 180, scene, &why);
        if (!b && why.startsWith(QLatin1StringView("no software")))
            SB_SKIP_OR_FAIL(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        std::vector<double> gpu;
        for (int i = 0; i < 30; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        QCOMPARE(b->lastFrameDraws(), 0u);
        SB_OK(b->drain(&gpu));
        QCOMPARE(gpu.size(), std::size_t(30));  // lavapipe has timestamps; one per frame, none lost
        for (const double ms : gpu)
            QVERIFY(ms >= 0 && ms < 1000);
        std::vector<std::uint8_t> pixels;
        SB_OK(b->readback(10.0, &pixels));
        QCOMPARE(pixels.size(), std::size_t(320 * 180 * 4));
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i] != kClear[0] || pixels[i + 1] != kClear[1] || pixels[i + 2] != kClear[2] || pixels[i + 3] != kClear[3])
                QFAIL(qPrintable(QStringLiteral("pixel %1 is (%2, %3, %4, %5)")
                                     .arg(i / 4)
                                     .arg(pixels[i])
                                     .arg(pixels[i + 1])
                                     .arg(pixels[i + 2])
                                     .arg(pixels[i + 3])));
        }
        // A second workload on the same device (serve mode's load .. unload, suspend, resume).
        b->unload();
        SB_OK(b->suspend());
        SB_OK(b->resume(PresentTarget{true}));
        SB_OK(b->load(buildScene(Workload::Sprites, 64)));
        for (int i = 0; i < 3; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        SB_OK(b->drain(&gpu));
        b->unload();
        SB_OK(b->suspend());
        b->shutdown();  // leaks would be validation errors here
        QCOMPARE(b->validationErrors(), 0);
    }

    // The worker end to end, headless (the dev-hook arguments of the GPU
    // tests): ready, one result whose signature is the clear colour, done;
    // no validation error (the worker reports one as "validation: ...").
    void vulkanWorker()
    {
        if (lavapipeIcd().isEmpty())
            SB_SKIP_OR_FAIL("lavapipe (the software Vulkan driver) isn't installed");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        addSoftwareVulkan(env);
        WorkerProcess w(kWorker,
                        {QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("vulkan"), QStringLiteral("--gpu"), kLavapipeId,
                         QStringLiteral("--allow-software"), QStringLiteral("--render-size"), QStringLiteral("320x180"),
                         QStringLiteral("--content-scale"), QStringLiteral("64"), QStringLiteral("--validation"),
                         QStringLiteral("--headless"), QStringLiteral("--plan"), QStringLiteral("quads:200:300")},
                        env);
        QVERIFY(w.started());
        const int code = w.finish();
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        QVERIFY2(errors(w.events()).isEmpty(), qPrintable(errors(w.events())));
        QVERIFY(is<pr::ReadyEvent>(w.events().first()));
        const auto ready = std::get<pr::ReadyEvent>(w.events().first());
        QCOMPARE(ready.api, QStringLiteral("vulkan"));
        QCOMPARE(ready.layers, QStringLiteral("VK_LAYER_KHRONOS_validation"));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 1);
        QVERIFY(rs[0].frames >= 3);
        QVERIFY(rs[0].present.isEmpty());  // headless: nothing presented
        QCOMPARE(rs[0].signature.size(), 432);
        for (qsizetype i = 0; i < rs[0].signature.size(); ++i)
            QCOMPARE(std::uint8_t(rs[0].signature[i]), kClear[i % 3]);
        QVERIFY(is<pr::DoneEvent>(w.events().last()));
        QVERIFY2(!w.stderrText().contains(QLatin1StringView("vulkan:")), qPrintable(w.stderrText()));
    }


    // The standard deviation of a signature's bytes: a flat image is 0.
    static double deviation(const std::vector<std::uint8_t> &sig)
    {
        double mean = 0, var = 0;
        for (const std::uint8_t b : sig)
            mean += b;
        mean /= double(sig.size());
        for (const std::uint8_t b : sig)
            var += (b - mean) * (b - mean);
        return std::sqrt(var / double(sig.size()));
    }

    // Task 11: Geometry and Draw calls at content / 64, headless on lavapipe.
    // The draw count is the scene's content-scaled drawsPerFrame (1 and 189),
    // the readback (tone mapped, no overlay, render size) isn't flat, and
    // the validation layers, with synchronisation validation, saw nothing.
    void vulkanScenes_data()
    {
        QTest::addColumn<int>("workload");
        QTest::addColumn<unsigned>("draws");
        QTest::newRow("geometry") << int(Workload::Geometry) << 1u;
        QTest::newRow("drawcalls") << int(Workload::DrawCalls) << 189u;
        // Task 12 (synchronisation validation on: any hazard is an error).
        QTest::newRow("shading") << int(Workload::Shading) << 6u;
        QTest::newRow("particles") << int(Workload::Particles) << 17u;
    }

    void vulkanScenes()
    {
        QFETCH(int, workload);
        QFETCH(unsigned, draws);
        QString why;
        const SceneData scene = buildScene(Workload(workload), 64);
        QCOMPARE(scene.drawsPerFrame, draws);
        auto b = headlessVulkan(320, 180, scene, &why);
        if (!b && why.startsWith(QLatin1StringView("no software")))
            SB_SKIP_OR_FAIL(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        std::vector<double> gpu;
        for (int i = 0; i < 8; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        QCOMPARE(b->lastFrameDraws(), draws);
        SB_OK(b->drain(&gpu));
        QCOMPARE(gpu.size(), std::size_t(8));
        std::vector<std::uint8_t> pixels;
        SB_OK(b->readback(2.0, &pixels));
        QCOMPARE(pixels.size(), std::size_t(320 * 180 * 4));
        QCOMPARE(b->lastFrameDraws(), draws);
        const std::vector<std::uint8_t> sig = blockSignature(pixels.data(), 320, 180);
        QCOMPARE(sig.size(), std::size_t(432));
        QVERIFY2(deviation(sig) > 2.0, qPrintable(QStringLiteral("a flat image (deviation %1)").arg(deviation(sig))));
        // The same scene again after a load .. unload (serve mode), and the overlay path.
        b->setOverlay("Geometry | Vulkan | 1 fps");
        SB_OK(b->frame(0.5, &gpu));
        SB_OK(b->drain(&gpu));
        b->unload();
        SB_OK(b->suspend());
        SB_OK(b->resume(PresentTarget{true}));
        SB_OK(b->load(scene));
        SB_OK(b->frame(0.0, &gpu));
        SB_OK(b->drain(&gpu));
        b->unload();
        SB_OK(b->suspend());
        b->shutdown();  // leaks would be validation errors here
        QCOMPARE(b->validationErrors(), 0);
    }

    // Task 11 through the worker: the 300 ms measure gives at least 3 frames, a
    // signature that isn't flat, no validation error.
    void vulkanSceneWorker_data()
    {
        QTest::addColumn<QString>("plan");
        QTest::newRow("geometry") << QStringLiteral("geometry:200:300");
        QTest::newRow("drawcalls") << QStringLiteral("drawcalls:200:300");
        QTest::newRow("shading") << QStringLiteral("shading:200:300");
        QTest::newRow("particles") << QStringLiteral("particles:200:300");
    }

    void vulkanSceneWorker()
    {
        QFETCH(QString, plan);
        if (lavapipeIcd().isEmpty())
            SB_SKIP_OR_FAIL("lavapipe (the software Vulkan driver) isn't installed");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        addSoftwareVulkan(env);
        WorkerProcess w(kWorker,
                        {QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("vulkan"), QStringLiteral("--gpu"), kLavapipeId,
                         QStringLiteral("--allow-software"), QStringLiteral("--render-size"), QStringLiteral("320x180"),
                         QStringLiteral("--content-scale"), QStringLiteral("64"), QStringLiteral("--validation"),
                         QStringLiteral("--headless"), QStringLiteral("--plan"), plan},
                        env);
        QVERIFY(w.started());
        const int code = w.finish();
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        QVERIFY2(errors(w.events()).isEmpty(), qPrintable(errors(w.events())));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 1);
        QVERIFY(rs[0].frames >= 3);
        QCOMPARE(rs[0].signature.size(), 432);
        std::vector<std::uint8_t> sig(rs[0].signature.begin(), rs[0].signature.end());
        QVERIFY2(deviation(sig) > 2.0, qPrintable(QStringLiteral("a flat image (deviation %1)").arg(deviation(sig))));
        QVERIFY(is<pr::DoneEvent>(w.events().last()));
        QVERIFY2(!w.stderrText().contains(QLatin1StringView("vulkan:")), qPrintable(w.stderrText()));
    }

    // The overlay drawn by the present pass (§5.9) equals rasterOverlay()
    // composited over the clear colour, and nothing else changes.
    void vulkanOverlay()
    {
        QString why;
        constexpr std::uint32_t W = 640, H = 360;
        auto b = headlessVulkan(W, H, buildScene(Workload::Quads, 64), &why);
        if (!b && why.startsWith(QLatin1StringView("no software")))
            SB_SKIP_OR_FAIL(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        const std::string text = "Geometry | Vulkan | 412 fps";
        b->setOverlay(text);
        std::vector<double> gpu;
        SB_OK(b->frame(0.0, &gpu));
        SB_OK(b->drain(&gpu));
        std::vector<std::uint8_t> out;
        SB_OK(b->readbackPresented(&out));
        QCOMPARE(out.size(), std::size_t(W * H * 4));

        OverlayLayout l;
        const std::vector<std::uint8_t> box = rasterOverlay(text, 1.0, &l);
        QCOMPARE(l.x, 8);
        QCOMPARE(l.y, 8);
        QCOMPARE(l.w, 334);
        QCOMPARE(l.h, 26);
        int background = 0, ink = 0;
        for (std::uint32_t y = 0; y < H; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::uint8_t *p = &out[(std::size_t(y) * W + x) * 4];
                const bool inBox = int(x) >= l.x && int(x) < l.x + l.w && int(y) >= l.y && int(y) < l.y + l.h;
                for (int c = 0; c < 3; ++c) {
                    int want = kClear[c];
                    if (inBox) {
                        // Premultiplied "over": src + dst x (1 - alpha).
                        const std::uint8_t *s = &box[(std::size_t(int(y) - l.y) * std::size_t(l.w) + std::size_t(int(x) - l.x)) * 4];
                        want = int(std::lround(s[c] + kClear[c] * (1.0 - s[3] / 255.0)));
                        if (c == 0)
                            (s[3] == 255 ? ink : background)++;
                    }
                    if (std::abs(int(p[c]) - want) > 1)
                        QFAIL(qPrintable(QStringLiteral("pixel (%1, %2) channel %3 is %4, not %5").arg(x).arg(y).arg(c).arg(p[c]).arg(want)));
                }
            }
        }
        QVERIFY(ink > 100 && background > 1000);  // both kinds of pixel were compared
        b->unload();
        SB_OK(b->suspend());
        b->shutdown();
        QCOMPARE(b->validationErrors(), 0);
    }
};

QTEST_GUILESS_MAIN(TestGpuBackends)
#include "tst_gpu_backends.moc"

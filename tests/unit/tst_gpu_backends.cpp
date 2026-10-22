// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU backends on software drivers (Phase 4a task 10: Vulkan on lavapipe,
// headless, with the Khronos validation layer and synchronisation
// validation; any validation error fails). The Vulkan cases are skipped where
// lavapipe isn't installed (Windows; task 16 makes it a CI requirement).
#include "gpu_reference_signatures.h"
#include "gpu_worker_process.h"

#include "tests/gpu/model/best_of.h"

#include "tests/gpu/worker/scene/native_ops.h"
#include "tests/gpu/worker/scene_constants.h"
#include "tests/gpu/worker/scene/overlay.h"
#include "tests/gpu/worker/scene/scenes.h"
#include "tests/gpu/worker/signature.h"
#include "tests/gpu/worker/vulkan/vk_backend.h"
#include "tests/gpu/worker/vulkan/vk_swapchain.h"
#include "tests/gpu/worker/gl/gl_backend.h"
#if defined(Q_OS_WIN)
#include "tests/gpu/worker/d3d11/d11_backend.h"
#include "tests/gpu/worker/d3d12/d12_backend.h"
#endif

#include <QTest>

#include <QStandardPaths>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(Q_OS_LINUX)
#include <dlfcn.h>
#endif

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
// The clear-only content of tasks 10, 18 and 21: Quads with no layers draws nothing, so ldrA is the clear colour
// (the fill workloads clear to it too). Since task 29 Quads and Sprites really draw.
SceneData clearScene()
{
    SceneData s = buildScene(Workload::Quads, 64);
    s.quadLayers = 0;
    return s;
}

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

#if defined(Q_OS_WIN)
// Task 18: D3D12 on WARP (headless, the debug layer where Windows has it).
BackendInit warpInit(std::uint32_t w, std::uint32_t h)
{
    BackendInit init;
    init.gpuId = kWarpId.toStdString();
    init.vendorId = kWarpVendor;
    init.deviceId = kWarpDevice;
    init.renderWidth = w;
    init.renderHeight = h;
    init.allowSoftware = true;
    init.validation = true;
    return init;
}

// The task 10 Vulkan pattern on D3D12: init headless, resume, load. Null and
// a reason when WARP can't run it (an "N/A: " reason starts with "no D3D12").
std::unique_ptr<d3d12::D3D12Backend> headlessD3D12(std::uint32_t w, std::uint32_t h, const SceneData &scene, QString *why)
{
    auto b = std::make_unique<d3d12::D3D12Backend>();
    BackendInfo info;
    const std::string e = b->init(warpInit(w, h), PresentTarget{true}, &info);
    if (e.rfind("N/A: ", 0) == 0) {
        *why = QStringLiteral("no D3D12 on WARP here: %1").arg(QString::fromStdString(e));
        return nullptr;
    }
    std::string r = e.empty() ? b->resume(PresentTarget{true}) : "init: " + e;
    if (r.empty())
        r = b->load(scene);
    if (!r.empty()) {
        *why = QString::fromStdString(r);
        return nullptr;
    }
    return b;
}

// The same on D3D11 (task 20): init headless, resume, load. Null and
// a reason when WARP can't run it (an "N/A: " reason starts with "no D3D11").
std::unique_ptr<d3d11::D3D11Backend> headlessD3D11(std::uint32_t w, std::uint32_t h, const SceneData &scene, QString *why)
{
    auto b = std::make_unique<d3d11::D3D11Backend>();
    BackendInfo info;
    const std::string e = b->init(warpInit(w, h), PresentTarget{true}, &info);
    if (e.rfind("N/A: ", 0) == 0) {
        *why = QStringLiteral("no D3D11 on WARP here: %1").arg(QString::fromStdString(e));
        return nullptr;
    }
    std::string r = e.empty() ? b->resume(PresentTarget{true}) : "init: " + e;
    if (r.empty())
        r = b->load(scene);
    if (!r.empty()) {
        *why = QString::fromStdString(r);
        return nullptr;
    }
    return b;
}
#endif

} // namespace

#if defined(Q_OS_LINUX)
// Task 21: an OpenGL 3.3 core debug context on Mesa's surfaceless EGL platform,
// software only (LIBGL_ALWAYS_SOFTWARE=1, set in initTestCase: llvmpipe), for
// the in-process GL cases. No window, no X or Wayland display, no GPU: the
// test process has no QGuiApplication. EGL is opened at run time (no headers).
class SurfacelessGl
{
public:
    ~SurfacelessGl()
    {
        if (m_display && m_makeCurrent)
            m_makeCurrent(m_display, nullptr, nullptr, nullptr);
        if (m_display && m_context && m_destroyContext)
            m_destroyContext(m_display, m_context);
        if (m_display && m_terminate)
            m_terminate(m_display);
        if (m_lib)
            dlclose(m_lib);
    }
    // "" when current, else why not (a skip, unless the CI requires software GPUs).
    QString open()
    {
        m_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!m_lib)
            return QStringLiteral("no libEGL.so.1");
        m_getProc = reinterpret_cast<GetProc>(dlsym(m_lib, "eglGetProcAddress"));
        if (!m_getProc)
            return QStringLiteral("no eglGetProcAddress");
        const auto fn = [this](const char *name) { return m_getProc(name); };
        const auto getPlatformDisplay = reinterpret_cast<void *(*)(unsigned, void *, const std::int32_t *)>(fn("eglGetPlatformDisplayEXT"));
        const auto initialize = reinterpret_cast<unsigned (*)(void *, std::int32_t *, std::int32_t *)>(fn("eglInitialize"));
        const auto bindApi = reinterpret_cast<unsigned (*)(unsigned)>(fn("eglBindAPI"));
        const auto chooseConfig = reinterpret_cast<unsigned (*)(void *, const std::int32_t *, void **, std::int32_t, std::int32_t *)>(fn("eglChooseConfig"));
        const auto createContext = reinterpret_cast<void *(*)(void *, void *, void *, const std::int32_t *)>(fn("eglCreateContext"));
        m_makeCurrent = reinterpret_cast<MakeCurrent>(fn("eglMakeCurrent"));
        m_destroyContext = reinterpret_cast<unsigned (*)(void *, void *)>(fn("eglDestroyContext"));
        m_terminate = reinterpret_cast<unsigned (*)(void *)>(fn("eglTerminate"));
        if (!getPlatformDisplay || !initialize || !bindApi || !chooseConfig || !createContext || !m_makeCurrent)
            return QStringLiteral("EGL lacks the surfaceless platform functions");
        constexpr unsigned kPlatformSurfaceless = 0x31DD, kOpenGlApi = 0x30A2;
        constexpr std::int32_t kNone = 0x3038, kRenderableType = 0x3040, kOpenGlBit = 0x0008, kMajor = 0x3098, kMinor = 0x30FB,
                               kProfileMask = 0x30FD, kCoreBit = 0x1, kDebug = 0x31B0;
        m_display = getPlatformDisplay(kPlatformSurfaceless, nullptr, nullptr);
        std::int32_t major = 0, minor = 0;
        if (!m_display || !initialize(m_display, &major, &minor))
            return QStringLiteral("no surfaceless EGL display");
        if (!bindApi(kOpenGlApi))
            return QStringLiteral("EGL can't bind OpenGL");
        // The default surface type is EGL_WINDOW_BIT, which the surfaceless platform has none of.
        constexpr std::int32_t kSurfaceType = 0x3033, kPbufferBit = 0x0001;
        const std::int32_t configAttribs[] = {kRenderableType, kOpenGlBit, kSurfaceType, kPbufferBit, kNone};
        void *config = nullptr;
        std::int32_t count = 0;
        if (!chooseConfig(m_display, configAttribs, &config, 1, &count) || count < 1)
            return QStringLiteral("no OpenGL EGL config");
        const std::int32_t contextAttribs[] = {kMajor, 3, kMinor, 3, kProfileMask, kCoreBit, kDebug, 1, kNone};
        m_context = createContext(m_display, config, nullptr, contextAttribs);
        if (!m_context || !m_makeCurrent(m_display, nullptr, nullptr, m_context))
            return QStringLiteral("no OpenGL 3.3 core context");
        // Never a real GPU here: only llvmpipe.
        const auto getString = reinterpret_cast<const unsigned char *(*)(unsigned)>(fn("glGetString"));
        const char *renderer = getString ? reinterpret_cast<const char *>(getString(0x1F01)) : nullptr;
        if (!renderer || !std::strstr(renderer, "llvmpipe"))
            return QStringLiteral("the surfaceless GL context isn't llvmpipe (%1)").arg(QString::fromLatin1(renderer ? renderer : "?"));
        return {};
    }
    PresentTarget target() const
    {
        PresentTarget t;
        t.headless = true;
        const GetProc get = m_getProc;
        t.glGetProcAddress = [get](const char *name) { return get(name); };
        return t;
    }

private:
    using GetProc = void *(*)(const char *);
    using MakeCurrent = unsigned (*)(void *, void *, void *, void *);
    void *m_lib = nullptr, *m_display = nullptr, *m_context = nullptr;
    GetProc m_getProc = nullptr;
    MakeCurrent m_makeCurrent = nullptr;
    unsigned (*m_destroyContext)(void *, void *) = nullptr;
    unsigned (*m_terminate)(void *) = nullptr;
};
#endif

// The overlay box of every API's present pass (§5.9): rasterOverlay() over the
// clear colour within 1/255, everything else the clear colour. "" or the first difference.
QString overlayDifference(const std::vector<std::uint8_t> &out, std::uint32_t W, std::uint32_t H, const std::string &text, int *ink, int *background)
{
    OverlayLayout l;
    const std::vector<std::uint8_t> box = rasterOverlay(text, 1.0, &l);
    for (std::uint32_t y = 0; y < H; ++y) {
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint8_t *p = &out[(std::size_t(y) * W + x) * 4];
            const bool inBox = int(x) >= l.x && int(x) < l.x + l.w && int(y) >= l.y && int(y) < l.y + l.h;
            for (int c = 0; c < 3; ++c) {
                int want = kClear[c];
                if (inBox) {
                    const std::uint8_t *s = &box[(std::size_t(int(y) - l.y) * std::size_t(l.w) + std::size_t(int(x) - l.x)) * 4];
                    want = int(std::lround(s[c] + kClear[c] * (1.0 - s[3] / 255.0)));
                    if (c == 0)
                        (s[3] == 255 ? *ink : *background)++;
                }
                if (std::abs(int(p[c]) - want) > 1)
                    return QStringLiteral("pixel (%1, %2) channel %3 is %4, not %5").arg(x).arg(y).arg(c).arg(p[c]).arg(want);
            }
        }
    }
    return {};
}

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
        // The in-process GL cases (task 21): Mesa's software rasteriser only. GLVND
        // would otherwise hand EGL to another vendor (NVIDIA's on Tiamat): Mesa's
        // manifest only, as gl_env does for a Mesa GPU (§4.3).
        {
            QProcessEnvironment gl;
            addSoftwareGl(gl);
            for (const QString &key : gl.keys())
                qputenv(qPrintable(key), gl.value(key).toLocal8Bit());
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
        const SceneData scene = clearScene();
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
        // Quads draws since task 29: the verification frame isn't the flat clear colour.
        QVERIFY2(deviation(std::vector<std::uint8_t>(rs[0].signature.begin(), rs[0].signature.end())) > 2.0, "a flat image");
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
    // The draw count is the scene's content-scaled drawsPerFrame (1 and 601),
    // the readback (tone mapped, no overlay, render size) isn't flat, and
    // the validation layers, with synchronisation validation, saw nothing.
    void vulkanScenes_data()
    {
        QTest::addColumn<int>("workload");
        QTest::addColumn<unsigned>("draws");
        QTest::newRow("geometry") << int(Workload::Geometry) << 1u;
        QTest::newRow("drawcalls") << int(Workload::DrawCalls) << 601u;
        // Task 12 (synchronisation validation on: any hazard is an error).
        QTest::newRow("shading") << int(Workload::Shading) << 6u;
        QTest::newRow("particles") << int(Workload::Particles) << 17u;
        QTest::newRow("quads") << int(Workload::Quads) << 1u;  // task 29: the fill workloads
        QTest::newRow("sprites") << int(Workload::Sprites) << 1u;
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
        auto b = headlessVulkan(W, H, clearScene(), &why);
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
    // The sync-validation gate (CI's old layer): below the threshold the worker still speaks clean
    // protocol (the note goes to stderr) and the run completes with core validation only. The dev
    // hook fakes the layer's version, so this runs on Tiamat's new layer too.
    void oldValidationLayerSkipsSyncValidation()
    {
        if (lavapipeIcd().isEmpty())
            SB_SKIP_OR_FAIL("lavapipe (the software Vulkan driver) isn't installed");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        addSoftwareVulkan(env);
        env.insert(QStringLiteral("SYNBENCH_GPU_FAKE_LAYER_SPEC"), QStringLiteral("1.3.204"));
        WorkerProcess w(kWorker,
                        {QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("vulkan"), QStringLiteral("--gpu"), kLavapipeId,
                         QStringLiteral("--allow-software"), QStringLiteral("--render-size"), QStringLiteral("320x180"),
                         QStringLiteral("--content-scale"), QStringLiteral("64"), QStringLiteral("--validation"),
                         QStringLiteral("--headless"), QStringLiteral("--plan"), QStringLiteral("geometry:100:200")},
                        env);
        QVERIFY(w.started());
        const int code = w.finish();
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        QVERIFY2(!w.problem().contains(QLatin1StringView("malformed")), qPrintable(w.problem()));  // every stdout line parsed
        QVERIFY(w.stderrText().contains(QLatin1StringView("sync validation skipped: validation layer 1.3.204 too old")));
        QCOMPARE(results(w.events()).size(), 1);
    }

    // Task 18 (Windows CI, WARP): task 10's vulkanClear on D3D12.
    void d3d12Clear()
    {
#if !defined(Q_OS_WIN)
        QSKIP("DirectX 12 is Windows only");
#else
        QString why;
        auto b = headlessD3D12(320, 180, clearScene(), &why);
        if (!b && why.startsWith(QLatin1StringView("no D3D12")))
            QSKIP(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        std::vector<double> gpu;
        for (int i = 0; i < 30; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        QCOMPARE(b->lastFrameDraws(), 0u);
        SB_OK(b->drain(&gpu));
        QVERIFY(gpu.size() <= std::size_t(30));
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
        // A second workload on the same device (serve mode's unload, suspend, resume, load).
        b->unload();
        SB_OK(b->suspend());
        SB_OK(b->resume(PresentTarget{true}));
        SB_OK(b->load(buildScene(Workload::Sprites, 64)));
        for (int i = 0; i < 3; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        SB_OK(b->drain(&gpu));
        b->unload();
        SB_OK(b->suspend());
        b->shutdown();
        QCOMPARE(b->validationErrors(), 0);
#endif
    }

    // Task 18 (Windows CI, WARP): task 10's vulkanOverlay on D3D12, the same
    // shader and constants, so the same box over the clear colour.
    void d3d12Overlay()
    {
#if !defined(Q_OS_WIN)
        QSKIP("DirectX 12 is Windows only");
#else
        QString why;
        constexpr std::uint32_t W = 640, H = 360;
        auto b = headlessD3D12(W, H, clearScene(), &why);
        if (!b && why.startsWith(QLatin1StringView("no D3D12")))
            QSKIP(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        const std::string text = "Geometry | DX12 | 412 fps";
        b->setOverlay(text);
        std::vector<double> gpu;
        SB_OK(b->frame(0.0, &gpu));
        SB_OK(b->drain(&gpu));
        std::vector<std::uint8_t> out;
        SB_OK(b->readbackPresented(&out));
        QCOMPARE(out.size(), std::size_t(W * H * 4));
        OverlayLayout l;
        const std::vector<std::uint8_t> box = rasterOverlay(text, 1.0, &l);
        int background = 0, ink = 0;
        for (std::uint32_t y = 0; y < H; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::uint8_t *p = &out[(std::size_t(y) * W + x) * 4];
                const bool inBox = int(x) >= l.x && int(x) < l.x + l.w && int(y) >= l.y && int(y) < l.y + l.h;
                for (int c = 0; c < 3; ++c) {
                    int want = kClear[c];
                    if (inBox) {
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
        QVERIFY(ink > 100 && background > 1000);
        b->unload();
        SB_OK(b->suspend());
        b->shutdown();
        QCOMPARE(b->validationErrors(), 0);
#endif
    }

    // Task 19 (Windows CI, WARP): the four scenes on D3D12, as vulkanScenes:
    // the draw counts of tasks 11-12, a readback that isn't flat, no debug-layer error.
    void d3d12Scenes_data()
    {
        QTest::addColumn<int>("workload");
        QTest::addColumn<unsigned>("draws");
        QTest::newRow("geometry") << int(Workload::Geometry) << 1u;
        QTest::newRow("drawcalls") << int(Workload::DrawCalls) << 601u;
        QTest::newRow("shading") << int(Workload::Shading) << 6u;
        QTest::newRow("particles") << int(Workload::Particles) << 17u;
        QTest::newRow("quads") << int(Workload::Quads) << 1u;  // task 30: the fill workloads
        QTest::newRow("sprites") << int(Workload::Sprites) << 1u;

    }

    void d3d12Scenes()
    {
#if !defined(Q_OS_WIN)
        QSKIP("DirectX 12 is Windows only");
#else
        QFETCH(int, workload);
        QFETCH(unsigned, draws);
        const SceneData scene = buildScene(Workload(workload), 64);
        QCOMPARE(scene.drawsPerFrame, draws);
        QString why;
        auto b = headlessD3D12(320, 180, scene, &why);
        if (!b && why.startsWith(QLatin1StringView("no D3D12")))
            QSKIP(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        std::vector<double> gpu;
        for (int i = 0; i < 8; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        QCOMPARE(b->lastFrameDraws(), draws);
        SB_OK(b->drain(&gpu));
        std::vector<std::uint8_t> pixels;
        SB_OK(b->readback(2.0, &pixels));
        QCOMPARE(pixels.size(), std::size_t(320 * 180 * 4));
        QCOMPARE(b->lastFrameDraws(), draws);
        const std::vector<std::uint8_t> sig = blockSignature(pixels.data(), 320, 180);
        QVERIFY2(deviation(sig) > 2.0, qPrintable(QStringLiteral("a flat image (deviation %1)").arg(deviation(sig))));
        // Again after unload .. resume .. load (serve mode), with the overlay on.
        b->setOverlay("Geometry | DX12 | 1 fps");
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
        b->shutdown();
        QCOMPARE(b->validationErrors(), 0);
#endif
    }

    // Task 20 (Windows CI, WARP): task 10's vulkanClear on D3D11.
    void d3d11Clear()
    {
#if !defined(Q_OS_WIN)
        QSKIP("DirectX 11 is Windows only");
#else
        QString why;
        auto b = headlessD3D11(320, 180, clearScene(), &why);
        if (!b && why.startsWith(QLatin1StringView("no D3D11")))
            QSKIP(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        std::vector<double> gpu;
        for (int i = 0; i < 30; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        QCOMPARE(b->lastFrameDraws(), 0u);
        SB_OK(b->drain(&gpu));
        QVERIFY(gpu.size() <= std::size_t(30));
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
        // A second workload on the same device (serve mode's unload, suspend, resume, load).
        b->unload();
        SB_OK(b->suspend());
        SB_OK(b->resume(PresentTarget{true}));
        SB_OK(b->load(buildScene(Workload::Sprites, 64)));
        for (int i = 0; i < 3; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        SB_OK(b->drain(&gpu));
        b->unload();
        SB_OK(b->suspend());
        b->shutdown();
        QCOMPARE(b->validationErrors(), 0);
#endif
    }

    // Task 20 (Windows CI, WARP): task 10's vulkanOverlay on D3D11, the same
    // shader and constants, so the same box over the clear colour.
    void d3d11Overlay()
    {
#if !defined(Q_OS_WIN)
        QSKIP("DirectX 11 is Windows only");
#else
        QString why;
        constexpr std::uint32_t W = 640, H = 360;
        auto b = headlessD3D11(W, H, clearScene(), &why);
        if (!b && why.startsWith(QLatin1StringView("no D3D11")))
            QSKIP(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        const std::string text = "Geometry | DX11 | 412 fps";
        b->setOverlay(text);
        std::vector<double> gpu;
        SB_OK(b->frame(0.0, &gpu));
        SB_OK(b->drain(&gpu));
        std::vector<std::uint8_t> out;
        SB_OK(b->readbackPresented(&out));
        QCOMPARE(out.size(), std::size_t(W * H * 4));
        OverlayLayout l;
        const std::vector<std::uint8_t> box = rasterOverlay(text, 1.0, &l);
        int background = 0, ink = 0;
        for (std::uint32_t y = 0; y < H; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::uint8_t *p = &out[(std::size_t(y) * W + x) * 4];
                const bool inBox = int(x) >= l.x && int(x) < l.x + l.w && int(y) >= l.y && int(y) < l.y + l.h;
                for (int c = 0; c < 3; ++c) {
                    int want = kClear[c];
                    if (inBox) {
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
        QVERIFY(ink > 100 && background > 1000);
        b->unload();
        SB_OK(b->suspend());
        b->shutdown();
        QCOMPARE(b->validationErrors(), 0);
#endif
    }

    // Task 20 (Windows CI, WARP): the four scenes on D3D11, as vulkanScenes:
    // the draw counts of tasks 11-12, a readback that isn't flat, no debug-layer error.
    void d3d11Scenes_data()
    {
        QTest::addColumn<int>("workload");
        QTest::addColumn<unsigned>("draws");
        QTest::newRow("geometry") << int(Workload::Geometry) << 1u;
        QTest::newRow("drawcalls") << int(Workload::DrawCalls) << 601u;
        QTest::newRow("shading") << int(Workload::Shading) << 6u;
        QTest::newRow("particles") << int(Workload::Particles) << 17u;
        QTest::newRow("quads") << int(Workload::Quads) << 1u;  // task 30: the fill workloads
        QTest::newRow("sprites") << int(Workload::Sprites) << 1u;

    }

    void d3d11Scenes()
    {
#if !defined(Q_OS_WIN)
        QSKIP("DirectX 11 is Windows only");
#else
        QFETCH(int, workload);
        QFETCH(unsigned, draws);
        const SceneData scene = buildScene(Workload(workload), 64);
        QCOMPARE(scene.drawsPerFrame, draws);
        QString why;
        auto b = headlessD3D11(320, 180, scene, &why);
        if (!b && why.startsWith(QLatin1StringView("no D3D11")))
            QSKIP(qPrintable(why));
        QVERIFY2(b, qPrintable(why));
        std::vector<double> gpu;
        for (int i = 0; i < 8; ++i)
            SB_OK(b->frame(i / 60.0, &gpu));
        QCOMPARE(b->lastFrameDraws(), draws);
        SB_OK(b->drain(&gpu));
        std::vector<std::uint8_t> pixels;
        SB_OK(b->readback(2.0, &pixels));
        QCOMPARE(pixels.size(), std::size_t(320 * 180 * 4));
        QCOMPARE(b->lastFrameDraws(), draws);
        const std::vector<std::uint8_t> sig = blockSignature(pixels.data(), 320, 180);
        QVERIFY2(deviation(sig) > 2.0, qPrintable(QStringLiteral("a flat image (deviation %1)").arg(deviation(sig))));
        // Again after unload .. resume .. load (serve mode), with the overlay on.
        b->setOverlay("Geometry | DX11 | 1 fps");
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
        b->shutdown();
        QCOMPARE(b->validationErrors(), 0);
#endif
    }

#if defined(Q_OS_LINUX)
    // Task 21: the GL backend on llvmpipe (surfaceless EGL), headless, as
    // vulkanClear: 30 frames, every pixel the clear colour (glReadPixels' rows
    // flipped to top-first), timestamps, unload .. resume .. load, no KHR_debug error.
    void openglClear()
    {
        SurfacelessGl egl;
        if (const QString why = egl.open(); !why.isEmpty())
            SB_SKIP_OR_FAIL(qPrintable(QStringLiteral("no software OpenGL here: %1").arg(why)));
        gl::GlBackend b;
        BackendInfo info;
        SB_OK(b.init(lavapipeInit(320, 180), egl.target(), &info));
        QVERIFY2(QString::fromStdString(info.deviceName).contains(QLatin1StringView("llvmpipe")), info.deviceName.c_str());
        SB_OK(b.resume(PresentTarget{true}));
        SB_OK(b.load(clearScene()));
        std::vector<double> gpu;
        for (int i = 0; i < 30; ++i)
            SB_OK(b.frame(i / 60.0, &gpu));
        QCOMPARE(b.lastFrameDraws(), 0u);
        SB_OK(b.drain(&gpu));
        if (info.gpuTimestamps)
            QCOMPARE(gpu.size(), std::size_t(30));
        std::vector<std::uint8_t> pixels;
        SB_OK(b.readback(10.0, &pixels));
        QCOMPARE(pixels.size(), std::size_t(320 * 180 * 4));
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i] != kClear[0] || pixels[i + 1] != kClear[1] || pixels[i + 2] != kClear[2] || pixels[i + 3] != kClear[3])
                QFAIL(qPrintable(QStringLiteral("pixel %1 is (%2, %3, %4, %5)").arg(i / 4).arg(pixels[i]).arg(pixels[i + 1]).arg(pixels[i + 2]).arg(pixels[i + 3])));
        }
        b.unload();
        SB_OK(b.suspend());
        SB_OK(b.resume(PresentTarget{true}));
        SB_OK(b.load(buildScene(Workload::Sprites, 64)));
        for (int i = 0; i < 3; ++i)
            SB_OK(b.frame(i / 60.0, &gpu));
        SB_OK(b.drain(&gpu));
        b.unload();
        SB_OK(b.suspend());
        b.shutdown();
        QVERIFY2(b.validationErrors() == 0, b.firstValidationError().c_str());
    }

    // Task 21: task 10's overlay check through the GL present pass (flipV = 1,
    // the box from the bottom): the top-first readback equals rasterOverlay over the clear colour.
    void openglOverlay()
    {
        SurfacelessGl egl;
        if (const QString why = egl.open(); !why.isEmpty())
            SB_SKIP_OR_FAIL(qPrintable(QStringLiteral("no software OpenGL here: %1").arg(why)));
        constexpr std::uint32_t W = 640, H = 360;
        gl::GlBackend b;
        BackendInfo info;
        SB_OK(b.init(lavapipeInit(W, H), egl.target(), &info));
        SB_OK(b.resume(PresentTarget{true}));
        SB_OK(b.load(clearScene()));
        const std::string text = "Geometry | OpenGL | 412 fps";
        b.setOverlay(text);
        std::vector<double> gpu;
        SB_OK(b.frame(0.0, &gpu));
        SB_OK(b.drain(&gpu));
        std::vector<std::uint8_t> out;
        SB_OK(b.readbackPresented(&out));
        QCOMPARE(out.size(), std::size_t(W * H * 4));
        int ink = 0, background = 0;
        const QString diff = overlayDifference(out, W, H, text, &ink, &background);
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
        QVERIFY(ink > 100 && background > 1000);
        b.unload();
        SB_OK(b.suspend());
        b.shutdown();
        QVERIFY2(b.validationErrors() == 0, b.firstValidationError().c_str());
    }

    // Task 22: the four scenes on GL (llvmpipe, surfaceless EGL), as vulkanScenes: the draw
    // counts of tasks 11-12, a readback that isn't flat, no KHR_debug error.
    void openglScenes_data()
    {
        QTest::addColumn<int>("workload");
        QTest::addColumn<unsigned>("draws");
        QTest::newRow("geometry") << int(Workload::Geometry) << 1u;
        QTest::newRow("drawcalls") << int(Workload::DrawCalls) << 601u;
        QTest::newRow("shading") << int(Workload::Shading) << 6u;
        QTest::newRow("particles") << int(Workload::Particles) << 17u;

    }

    void openglScenes()
    {
        SurfacelessGl egl;
        if (const QString why = egl.open(); !why.isEmpty())
            SB_SKIP_OR_FAIL(qPrintable(QStringLiteral("no software OpenGL here: %1").arg(why)));
        QFETCH(int, workload);
        QFETCH(unsigned, draws);
        const SceneData scene = buildScene(Workload(workload), 64);
        QCOMPARE(scene.drawsPerFrame, draws);
        gl::GlBackend b;
        BackendInfo info;
        SB_OK(b.init(lavapipeInit(320, 180), egl.target(), &info));
        SB_OK(b.resume(PresentTarget{true}));
        SB_OK(b.load(scene));
        std::vector<double> gpu;
        for (int i = 0; i < 8; ++i)
            SB_OK(b.frame(i / 60.0, &gpu));
        QCOMPARE(b.lastFrameDraws(), draws);
        SB_OK(b.drain(&gpu));
        std::vector<std::uint8_t> pixels;
        SB_OK(b.readback(2.0, &pixels));
        QCOMPARE(pixels.size(), std::size_t(320 * 180 * 4));
        QCOMPARE(b.lastFrameDraws(), draws);
        const std::vector<std::uint8_t> sig = blockSignature(pixels.data(), 320, 180);
        QVERIFY2(deviation(sig) > 2.0, qPrintable(QStringLiteral("a flat image (deviation %1)").arg(deviation(sig))));
        // Again after unload .. resume .. load (serve mode), with the overlay on.
        b.setOverlay("Geometry | OpenGL | 1 fps");
        SB_OK(b.frame(0.5, &gpu));
        SB_OK(b.drain(&gpu));
        b.unload();
        SB_OK(b.suspend());
        SB_OK(b.resume(PresentTarget{true}));
        SB_OK(b.load(scene));
        SB_OK(b.frame(0.0, &gpu));
        SB_OK(b.drain(&gpu));
        b.unload();
        SB_OK(b.suspend());
        b.shutdown();
        QVERIFY2(b.validationErrors() == 0, b.firstValidationError().c_str());
    }
#endif

    // Task 29 (§6.6): 100,000 sprites go out in batches of 4,096: 25 draws, the last of 1,696; and the closed-form motion.
    void spriteBatching()
    {
        const auto b = sc::spriteBatches(100000, 4096);
        QCOMPARE(b.size(), std::size_t(25));
        QCOMPARE(b.front().first, 0u);
        QCOMPARE(b[1].first, 4096u);
        QCOMPARE(b.back().first, 24u * 4096u);
        QCOMPARE(b.back().count, 1696u);
        std::uint32_t total = 0;
        for (const auto &x : b)
            total += x.count;
        QCOMPARE(total, 100000u);
        QCOMPARE(sc::spriteBatches(1563).size(), std::size_t(1));  // content / 64
        QCOMPARE(sc::spriteBatches(4096).size(), std::size_t(1));
        QCOMPARE(sc::spriteBatches(4097).size(), std::size_t(2));
        QVERIFY(sc::spriteBatches(0).empty());
        // x = mod(x0 + vx t, 1920), y = mod(y0 + vy t, 1080), also for a negative velocity; the rest is packed as specified.
        SceneData s;
        Sprite sp{};
        sp.x0 = 100.f; sp.y0 = 50.f; sp.vx = -300.f; sp.vy = 500.f;
        sp.atlasIndex = 5; sp.rotation16 = 0x1234; sp.colourRgba8 = 0xFF8040A0u;
        s.sprites = {sp};
        std::uint8_t out[16];
        sc::writeSprites(s, 2.0, out);  // x = 100 - 600 = -500 -> 1420; y = 50 + 1000 = 1050
        float pos[2];
        std::uint32_t packed, colour;
        std::memcpy(pos, out, 8);
        std::memcpy(&packed, out + 8, 4);
        std::memcpy(&colour, out + 12, 4);
        QVERIFY(std::fabs(pos[0] - 1420.f) < 1e-3f && std::fabs(pos[1] - 1050.f) < 1e-3f);
        QCOMPARE(packed, 0x00051234u);  // rotation16 | atlasIndex << 16
        QCOMPARE(colour, 0xFF8040A0u);
        sc::writeSprites(s, 4.0, out);  // y = 50 + 2000 = 2050 -> 970
        std::memcpy(pos, out, 8);
        QVERIFY(std::fabs(pos[1] - 970.f) < 1e-3f);
    }

    // Task 26: the Qt raster native 2D path through the worker, headless on Qt's offscreen platform (no
    // display needed): each workload at content / 64 runs at least 3 frames in 300 ms, reads back something
    // that isn't flat, and the result's fields give the MOps/s of §7.1 (ops x frames / seconds / 10^6).
    void qtRasterNative_data()
    {
        QTest::addColumn<QString>("workload");
        for (const char *w : {"shapes", "text", "blits", "gradients"})
            QTest::newRow(w) << QString::fromLatin1(w);
    }

    void qtRasterNative()
    {
        QFETCH(QString, workload);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        WorkerProcess w(kWorker,
                        {QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("qt"), QStringLiteral("--gpu"), kLavapipeId,
                         QStringLiteral("--render-size"), QStringLiteral("320x180"), QStringLiteral("--content-scale"), QStringLiteral("64"),
                         QStringLiteral("--headless"), QStringLiteral("--plan"), workload + QStringLiteral(":100:300")},
                        env);
        QVERIFY(w.started());
        const int code = w.finish(60000);
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        const auto ready = std::get<pr::ReadyEvent>(w.events().first());
        QCOMPARE(ready.api, QStringLiteral("qt"));
        QVERIFY2(ready.version.startsWith(QLatin1StringView("Qt ")) && ready.version.endsWith(QLatin1StringView(" raster")), qPrintable(ready.version));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 1);
        const pr::ResultEvent &r = rs[0];
        QCOMPARE(r.workload, workload);
        QVERIFY2(r.frames >= 3, qPrintable(QString::number(r.frames)));
        QVERIFY(r.seconds > 0);
        QCOMPARE(r.signature.size(), 432);
        QVERIFY2(deviation(std::vector<std::uint8_t>(r.signature.begin(), r.signature.end())) > 2.0, "a flat image");
        // MOps/s from the event: ops per frame x frames / seconds / 1e6, which is ops x fps / 1e6.
        const NativeKind kind = *nativeKindOf(*workloadFromId(workload.toStdString()));
        const double ops = double(buildNativeOps(kind, 64).opsPerFrame);
        const double mops = ops * double(r.frames) / r.seconds / 1e6;
        QVERIFY(mops > 0);
        QVERIFY2(std::fabs(mops - ops * r.fps / 1e6) <= 1e-9 * std::max(1.0, mops), qPrintable(QStringLiteral("%1 vs %2").arg(mops).arg(ops * r.fps / 1e6)));
    }

    // Task 27 (Windows CI, WARP): the Direct2D native 2D path through the worker, headless, with the D3D11
    // debug layer where Windows has it: each workload at content / 64 runs at least 3 frames in 300 ms,
    // reads back something that isn't flat, and gives MOps/s from the result's fields as Qt raster does.
    // The text is the bundled IBM Plex Sans (no fallback warning).
    void direct2DNative_data() { qtRasterNative_data(); }

    void direct2DNative()
    {
#if !defined(Q_OS_WIN)
        QSKIP("Direct2D is Windows only");
#else
        QFETCH(QString, workload);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        WorkerProcess w(kWorker,
                        {QStringLiteral("--run"), QStringLiteral("--api"), QStringLiteral("d2d"), QStringLiteral("--gpu"), kWarpId,
                         QStringLiteral("--allow-software"), QStringLiteral("--validation"), QStringLiteral("--render-size"), QStringLiteral("320x180"),
                         QStringLiteral("--content-scale"), QStringLiteral("64"), QStringLiteral("--headless"), QStringLiteral("--plan"),
                         workload + QStringLiteral(":100:300")},
                        env);
        QVERIFY(w.started());
        const int code = w.finish(60000);
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        const auto ready = std::get<pr::ReadyEvent>(w.events().first());
        QCOMPARE(ready.api, QStringLiteral("d2d"));
        QVERIFY2(ready.version.startsWith(QLatin1StringView("Direct2D 1.1")), qPrintable(ready.version));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 1);
        const pr::ResultEvent &r = rs[0];
        QCOMPARE(r.workload, workload);
        QVERIFY2(r.frames >= 3, qPrintable(QString::number(r.frames)));
        QVERIFY(r.seconds > 0);
        QCOMPARE(r.signature.size(), 432);
        QVERIFY2(deviation(std::vector<std::uint8_t>(r.signature.begin(), r.signature.end())) > 2.0, "a flat image");
        const NativeKind kind = *nativeKindOf(*workloadFromId(workload.toStdString()));
        const double ops = double(buildNativeOps(kind, 64).opsPerFrame);
        const double mops = ops * double(r.frames) / r.seconds / 1e6;
        QVERIFY(mops > 0);
        QVERIFY2(std::fabs(mops - ops * r.fps / 1e6) <= 1e-9 * std::max(1.0, mops), qPrintable(QStringLiteral("%1 vs %2").arg(mops).arg(ops * r.fps / 1e6)));
        const QString log = w.stderrText();
        QVERIFY2(!log.contains(QLatin1StringView("d2d: ")) || log.contains(QLatin1StringView("debug layer isn't installed")), qPrintable(log));
        QVERIFY2(!log.contains(QLatin1StringView("uses Segoe UI")), qPrintable(log));
#endif
    }

    // D20 (task 19): every API draws the four scenes like the pinned Vulkan
    // reference (gpu_reference_signatures.h), within §5.7's tolerance. Vulkan
    // on lavapipe here (any Mesa version), DX12 and DX11 (task 20) on WARP on Windows.
    void signaturesMatchVulkanReference_data()
    {
        QTest::addColumn<QString>("api");
#if defined(Q_OS_WIN)
        QTest::newRow("dx12") << QStringLiteral("dx12");
        QTest::newRow("dx11") << QStringLiteral("dx11");  // task 20
#else
        QTest::newRow("vulkan") << QStringLiteral("vulkan");
        QTest::newRow("opengl") << QStringLiteral("opengl");  // tasks 21 and 22: llvmpipe GL against the lavapipe pins
#endif
    }

    void signaturesMatchVulkanReference()
    {
        QFETCH(QString, api);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#if defined(Q_OS_WIN)
        const QString gpu = kWarpId;
        const QString program = kWorker;
        QStringList args;
#else
        QString program = kWorker;
        QStringList args;
        if (api == QLatin1StringView("opengl")) {
            // Headless GL needs an X display for Qt's offscreen GLX surface: the worker's own
            // xvfb-run, never the session's display (task 10's rule), and llvmpipe.
            program = QStandardPaths::findExecutable(QStringLiteral("xvfb-run"));
            if (program.isEmpty())
                SB_SKIP_OR_FAIL("xvfb-run isn't installed");
            args = {QStringLiteral("-a"), QStringLiteral("-s"), QStringLiteral("-screen 0 1920x1080x24"), kWorker};
            env.remove(QStringLiteral("WAYLAND_DISPLAY"));
            addSoftwareGl(env);  // llvmpipe through Mesa's GLX and EGL only (M4)
        } else {
            if (lavapipeIcd().isEmpty())
                SB_SKIP_OR_FAIL("lavapipe (the software Vulkan driver) isn't installed");
            addSoftwareVulkan(env);
        }
        const QString gpu = kLavapipeId;
#endif
        WorkerProcess w(program,
                        args + QStringList{QStringLiteral("--run"), QStringLiteral("--api"), api, QStringLiteral("--gpu"), gpu, QStringLiteral("--allow-software"),
                         QStringLiteral("--render-size"), QStringLiteral("320x180"), QStringLiteral("--content-scale"), QStringLiteral("64"),
                         QStringLiteral("--headless"), QStringLiteral("--plan"),
                         QStringLiteral("geometry:200:300,drawcalls:200:300,shading:200:300,particles:200:300")},
                        env);
        QVERIFY(w.started());
        const int code = w.finish(120000);
#if defined(Q_OS_WIN)
        if (code == 5)
            QSKIP(qPrintable(QStringLiteral("no %1 on WARP here: %2").arg(api, errors(w.events()))));
#endif
        QVERIFY2(code == 0, qPrintable(QStringLiteral("exit %1: %2 %3").arg(code).arg(errors(w.events()), w.stderrText())));
        const QList<pr::ResultEvent> rs = results(w.events());
        QCOMPARE(rs.size(), 4);
        QStringList report;
        bool allMatch = true;
        for (const ReferenceSignature &ref : kVulkanReference) {
            const pr::ResultEvent *r = nullptr;
            for (const pr::ResultEvent &e : rs) {
                if (e.workload == QLatin1StringView(ref.workload))
                    r = &e;
            }
            QVERIFY2(r, ref.workload);
            const auto diff = compareSignatures(r->signature, QByteArray::fromHex(ref.hex));
            QVERIFY2(diff, ref.workload);
            report << QStringLiteral("%1: mean %2/255, max %3/255").arg(QLatin1StringView(ref.workload)).arg(diff->mean, 0, 'f', 2).arg(diff->max);
            allMatch = allMatch && signaturesMatch(*diff);
        }
        qInfo("%s vs the Vulkan reference: %s", qPrintable(api), qPrintable(report.join(QStringLiteral("; "))));
        QVERIFY2(allMatch, qPrintable(report.join(QStringLiteral("; "))));
    }
};

QTEST_GUILESS_MAIN(TestGpuBackends)
#include "tst_gpu_backends.moc"

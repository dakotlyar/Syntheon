// SPDX-License-Identifier: GPL-3.0-or-later
// synbench-gpu: the GPU test's worker (Phase 4a design §2, D1, D12). One
// process per API: a driver crash, a GPU hang or a stuck present kills this
// process, never SynBench.
//
//   synbench-gpu --run --serve --api vulkan|dx12|dx11|opengl --gpu <id>
//   synbench-gpu --run --api vulkan|dx12|dx11|opengl|d2d|qt --gpu <id> --plan <plan> [--screen <name>]
//   synbench-gpu --probe | --probe-gl --gpu <id> | --self-test
//   dev hooks: --api null, --headless, --allow-software, --render-size WxH,
//              --content-scale F, --validation, --no-overlay; --print-hashes
//
// stdout: the JSON lines of §2.4 (tests/gpu/model/protocol.h). stdin: the
// lifeline (EOF -> exit 3 at once) and the commands: "stop" in every mode;
// with --serve also "run <workload>:<warmupMs>:<measureMs>" and "quit" (§2.3).
// Exit codes: 0 done; 1 error; 2 bad arguments or a protocol error; 3 stdin
// closed; 4 stopped; 5 the API is not available.
//
// Threads: the main thread owns Qt, the window and the backend; it renders
// and pumps events once per frame. The stdin thread only parses lines: it
// sets the stop flag, and hands run/quit/stop to the main thread as queued
// calls. A run or quit before the previous run's "idle" is a protocol error,
// decided on the stdin thread (the client must wait for idle), so it doesn't
// depend on when the main thread picks the command up.
//
// Each run (serve and plan mode alike): show the window and wait for its
// first expose -> resume() -> Runner::run() -> suspend() -> hide -> result
// (or error) -> idle (serve). init() is called once, before "ready", with no
// window shown (backend.h: the call order).
#include "tests/gpu/model/frame_stats.h"
#include "tests/gpu/model/ids.h"
#include "tests/gpu/model/linux_gpus.h"
#include "tests/gpu/model/windows_gpus.h"
#include "tests/gpu/model/protocol.h"
#include "tests/gpu/model/render_size.h"
#include "tests/gpu/worker/backend.h"
#include "tests/gpu/worker/emit.h"
#include "tests/gpu/worker/qt_glue.h"
#include "tests/gpu/worker/qtraster/qt_native.h"
#include "tests/gpu/worker/runner.h"
#include "tests/gpu/worker/scene/golden_hashes.inc"
#include "tests/gpu/worker/scene/native_ops.h"
#include "tests/gpu/worker/scene/scenes.h"
#include "tests/gpu/worker/vulkan/vk_probe.h"
#if defined(_WIN32)
#include "tests/gpu/worker/dxgi/dxgi_probe.h"
#endif
#include "tests/gpu/worker/worker_window.h"
#if defined(SYNBENCH_DEV_HOOKS)
#include "tests/gpu/worker/null/null_backend.h"
#endif

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QThread>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

using namespace sb::gpu;
namespace pr = sb::gpu::protocol;

namespace {

constexpr int kExitOk = 0, kExitError = 1, kExitArgs = 2, kExitStdin = 3, kExitStopped = 4, kExitNa = 5;

void hardenProcess()
{
#if defined(_WIN32)
    // d3d12, dxgi, d3d11, d2d1, dwrite and vulkan-1 from System32 only; Qt
    // from our own folder (§2.2, the cpucrypto P17 hardening).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
#elif defined(__linux__)
    // Never outlive SynBench, even if it is killed (the Phase 3 a6deee0 lesson).
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
}

[[noreturn]] void failArgs(const std::string &problem)
{
    std::fflush(stdout);
    std::fprintf(stderr, "synbench-gpu: %s\n", problem.c_str());
    std::fflush(stderr);
    std::exit(kExitArgs);
}

// ---- arguments ----

enum class Mode { None, Probe, ProbeGl, Run };

struct Args {
    Mode mode = Mode::None;
    std::string api;  // as given: vulkan dx12 dx11 opengl d2d qt (null: dev hook)
    QString gpu, screen, luid;  // luid: Windows, 16 hex digits (the GUI's GpuEntry::luid)
    std::optional<QString> planText;
    bool serve = false;
    bool headless = false, allowSoftware = false, validation = false, overlay = true;
    std::optional<Size> renderSize;
    double contentScale = 1.0;
};

bool knownApi(const std::string &api)
{
    for (const char *a : {"vulkan", "dx12", "dx11", "opengl", "d2d", "qt"}) {
        if (api == a)
            return true;
    }
#if defined(SYNBENCH_DEV_HOOKS)
    if (api == "null")
        return true;
#endif
    return false;
}

bool nativeApi(const std::string &api)
{
    return api == "d2d" || api == "qt";
}

Args parseArgs(const std::vector<std::string> &argv)
{
    Args a;
    const auto value = [&](std::size_t &i) -> std::string {
        if (i + 1 >= argv.size())
            failArgs("missing value for " + argv[i]);
        return argv[++i];
    };
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string &opt = argv[i];
        if (opt == "--probe" || opt == "--probe-gl" || opt == "--run") {
            if (a.mode != Mode::None)
                failArgs("more than one of --probe, --probe-gl and --run");
            a.mode = opt == "--probe" ? Mode::Probe : opt == "--probe-gl" ? Mode::ProbeGl : Mode::Run;
        } else if (opt == "--serve") {
            a.serve = true;
        } else if (opt == "--api") {
            a.api = value(i);
            if (!knownApi(a.api))
                failArgs("unknown --api '" + a.api + "'");
        } else if (opt == "--gpu") {
            a.gpu = QString::fromStdString(value(i));
        } else if (opt == "--luid") {
            a.luid = QString::fromStdString(value(i));
            std::array<quint8, 8> bytes;
            if (!parseLuidHex(a.luid, &bytes))
                failArgs("bad --luid '" + a.luid.toStdString() + "'");
        } else if (opt == "--plan") {
            a.planText = QString::fromStdString(value(i));
        } else if (opt == "--screen") {
            a.screen = QString::fromStdString(value(i));
#if defined(SYNBENCH_DEV_HOOKS)
        } else if (opt == "--headless") {
            a.headless = true;
        } else if (opt == "--allow-software") {
            a.allowSoftware = true;
        } else if (opt == "--validation") {
            a.validation = true;
        } else if (opt == "--no-overlay") {
            a.overlay = false;
        } else if (opt == "--render-size") {
            const std::string v = value(i);
            static const QRegularExpression re(QStringLiteral("^([1-9][0-9]{0,4})x([1-9][0-9]{0,4})$"));
            const auto m = re.match(QString::fromStdString(v));
            if (!m.hasMatch())
                failArgs("bad --render-size '" + v + "' (WxH)");
            a.renderSize = Size{m.captured(1).toInt(), m.captured(2).toInt()};
        } else if (opt == "--content-scale") {
            const std::string v = value(i);
            bool ok = false;
            a.contentScale = QString::fromStdString(v).toDouble(&ok);
            if (!ok || !(a.contentScale >= 1.0) || a.contentScale > 1e6)
                failArgs("bad --content-scale '" + v + "'");
#endif
        } else {
            failArgs("unknown option '" + opt + "'");
        }
    }
    if (a.mode == Mode::None)
        failArgs("one of --probe, --probe-gl, --run, --self-test is required");
    if (a.mode == Mode::ProbeGl && a.gpu.isEmpty())
        failArgs("--probe-gl needs --gpu");
    if (a.mode != Mode::Run)
        return a;
    if (a.api.empty())
        failArgs("--run needs --api");
    if (a.serve == a.planText.has_value())
        failArgs("--run needs exactly one of --serve and --plan");
    if (a.serve && nativeApi(a.api))
        failArgs("--serve is for the 3D APIs only");
    if (a.gpu.isEmpty() && a.api != "null")
        failArgs("--run needs --gpu");
    return a;
}

// "10de:2c05@0000:01:00.0", "10de:2c05#0" (P11) -> the BackendInit identity.
// A vendor id can be wider than 4 digits: makeLinuxGpuId's %04x gives
// lavapipe (VK_VENDOR_ID_MESA) "10005:0000#0" (task 10).
bool parseGpuId(const QString &id, BackendInit *init)
{
    static const QRegularExpression re(QStringLiteral("^([0-9a-f]{4,8}):([0-9a-f]{4,8})(?:@(.+)|#([0-9]+))$"));
    const auto m = re.match(id);
    if (!m.hasMatch())
        return false;
    init->gpuId = id.toStdString();
    init->vendorId = m.captured(1).toUInt(nullptr, 16);
    init->deviceId = m.captured(2).toUInt(nullptr, 16);
    init->pciSlot = m.captured(3).toStdString();
    return true;
}

std::vector<WorkloadPlan> toPlan(const QVector<pr::PlanItem> &items, std::string *error)
{
    std::vector<WorkloadPlan> plan;
    for (const pr::PlanItem &p : items) {
        const auto w = workloadFromId(p.workload.toStdString());
        if (!w) {
            *error = "unknown workload '" + p.workload.toStdString() + "'";
            return {};
        }
        plan.push_back({*w, p.warmupMs, p.measureMs});
    }
    return plan;
}

// ---- backends ----

std::unique_ptr<IBackend> makeBackend(const std::string &api)
{
#if defined(SYNBENCH_DEV_HOOKS)
    if (api == "null")
        return makeNullBackend();
#endif
    if (api == "vulkan")
        return makeVulkanBackend();
    if (api == "opengl")
        return makeOpenGlBackend();
#if defined(_WIN32)
    if (api == "dx12")
        return makeD3D12Backend();
    if (api == "dx11")
        return makeD3D11Backend();
#endif
    // Tasks 26/27 run the native paths (qt, d2d). Elsewhere "the <api>
    // backend isn't in this build".
    return nullptr;
}

QSurface::SurfaceType surfaceType(const std::string &api)
{
    if (api == "vulkan")
        return QSurface::VulkanSurface;
    if (api == "opengl")
        return QSurface::OpenGLSurface;
    if (api == "dx12" || api == "dx11" || api == "d2d")
        return QSurface::Direct3DSurface;
    return QSurface::RasterSurface;  // qt, null
}

// The window title's API name (§3.3): "Vulkan", "DirectX 12", ...
QString titleApiName(const std::string &api)
{
    if (const auto id = apiFromKey(QString::fromStdString(api)))
        return apiName(*id);
    if (api == "d2d")
        return nativePathName(NativePath::Direct2D);
    if (api == "qt")
        return nativePathName(NativePath::QtRaster);
    return QStringLiteral("Null");
}

// A backend reason -> an error event: "N/A: " = not available (na), "LOST: " = device lost.
pr::ErrorEvent errorEvent(Workload *w, const std::string &reason)
{
    pr::ErrorEvent e;
    if (w)
        e.workload = QString::fromLatin1(workloadId(*w));
    std::string text = reason;
    if (text.rfind("N/A: ", 0) == 0) {
        e.na = true;
        text.erase(0, 5);
    } else if (text.rfind("LOST: ", 0) == 0) {
        text.erase(0, 6);
    }
    e.reason = QString::fromStdString(text);
    return e;
}

double mean(const std::vector<double> &v)
{
    return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
}

// ---- stdin: the lifeline and the commands ----

struct Control {
    std::atomic<bool> stop{false};
    std::atomic<bool> busy{false};  // a run was accepted and its idle isn't out yet
    bool serve = false;
    // Run on the main thread (queued calls).
    std::function<void(WorkloadPlan)> onRun;
    std::function<void()> onQuit, onStop;
};

[[noreturn]] void protocolError(const std::string &what)
{
    emitLineAndExit(pr::ErrorEvent{{}, QString::fromStdString("protocol: " + what), false}, kExitArgs);
}

void queue(std::function<void()> f)
{
    QMetaObject::invokeMethod(QCoreApplication::instance(), std::move(f), Qt::QueuedConnection);
}

void handleLine(Control &c, std::string line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.empty())
        return;
    if (line == "stop") {
        c.stop = true;
        if (c.serve)
            queue([&c] { c.onStop(); });
        return;
    }
    if (!c.serve)
        return;  // a --plan run only listens for stop
    if (line == "quit") {
        if (c.busy)
            protocolError("quit while busy");
        c.busy = true;  // nothing after quit runs
        queue([&c] { c.onQuit(); });
        return;
    }
    if (line.rfind("run ", 0) == 0) {
        if (c.busy)
            protocolError("run while busy");
        QString error;
        const auto items = pr::parsePlan(QString::fromStdString(line.substr(4)), &error);
        std::string bad;
        const std::vector<WorkloadPlan> plan = items && items->size() == 1 ? toPlan(*items, &bad) : std::vector<WorkloadPlan>{};
        if (plan.size() != 1)
            protocolError("bad command '" + line + "'");
        c.busy = true;
        const WorkloadPlan p = plan.front();
        queue([&c, p] { c.onRun(p); });
        return;
    }
    protocolError("unknown command '" + line + "'");
}

// SynBench holds our stdin open while it lives: EOF means it is gone, so exit
// at once (_Exit: no destructors, no waiting for the main thread). Raw reads,
// not stdio (the cpucrypto lesson: a thread blocked in fgetc() holds stdin's
// lock, and exit() would wait for it).
void startStdinThread(Control &c)
{
    std::thread([&c] {
        std::string pending;
        char buf[512];
        for (;;) {
#if defined(_WIN32)
            const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            DWORD n = 0;
            if (in == INVALID_HANDLE_VALUE || in == nullptr || !ReadFile(in, buf, DWORD(sizeof buf), &n, nullptr) || n == 0)
                break;
#else
            const ssize_t n = ::read(0, buf, sizeof buf);
            if (n <= 0)
                break;
#endif
            pending.append(buf, std::size_t(n));
            for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                handleLine(c, std::move(line));
            }
            if (pending.size() > 4096)
                protocolError("command line too long");
        }
        std::fflush(stdout);
        std::_Exit(kExitStdin);
    }).detach();
}

// ---- the self-test and the golden hashes ----

int selfTest()
{
    for (std::size_t i = 0; i < kAllWorkloads.size(); ++i) {
        const std::uint64_t h = contentHash(buildScene(kAllWorkloads[i]));
        if (h != kGoldenHash[i]) {
            std::fprintf(stderr, "synbench-gpu: self-test: %s content hash 0x%016llx is not the golden 0x%016llx\n",
                         workloadId(kAllWorkloads[i]), (unsigned long long)h, (unsigned long long)kGoldenHash[i]);
            return kExitError;
        }
    }
    for (std::size_t i = 0; i < kNativeKinds.size(); ++i) {
        const std::uint64_t h = nativeHash(buildNativeOps(kNativeKinds[i]));
        if (h != kGoldenNativeHash[i]) {
            std::fprintf(stderr, "synbench-gpu: self-test: native %s hash 0x%016llx is not the golden 0x%016llx\n", nativeId(kNativeKinds[i]),
                         (unsigned long long)h, (unsigned long long)kGoldenNativeHash[i]);
            return kExitError;
        }
    }
    return kExitOk;
}

#if defined(SYNBENCH_DEV_HOOKS)
// Developer builds only: golden_hashes.inc as it should read for this build.
int printHashes()
{
    std::printf("// SPDX-License-Identifier: GPL-3.0-or-later\n"
                "// GENERATED by synbench-gpu --print-hashes (a developer build, on Linux); do not edit.\n"
                "// A change here changes the content the GPU test draws: bump its testVersion.\n"
                "// contentHash(buildScene(w)) at content scale 1, for w in kAllWorkloads order\n"
                "// (Phase 4a design §6.7); synbench-gpu --self-test checks them on every OS.\n"
                "#pragma once\n\n#include <array>\n#include <cstdint>\n\nnamespace sb::gpu {\n\n"
                "inline constexpr std::array<std::uint64_t, %d> kGoldenHash = {\n",
                int(kAllWorkloads.size()));
    for (const Workload w : kAllWorkloads)
        std::printf("    0x%016llxull,  // %s\n", (unsigned long long)contentHash(buildScene(w)), workloadId(w));
    std::printf("};\n\n// nativeHash(buildNativeOps(k)) at content scale 1, for k in kNativeKinds order (§7.1, task 25).\n"
                "inline constexpr std::array<std::uint64_t, %d> kGoldenNativeHash = {\n",
                int(kNativeKinds.size()));
    for (const NativeKind k : kNativeKinds)
        std::printf("    0x%016llxull,  // %s\n", (unsigned long long)nativeHash(buildNativeOps(k)), nativeId(k));
    std::printf("};\n\n} // namespace sb::gpu\n");
    return kExitOk;
}
#endif

// ---- a run ----

enum class Outcome { Done, Error, ErrorNa, Stopped };

class Worker
{
public:
    Worker(const Args &a, Control &c) : m_args(a), m_control(c) {}

    // Everything up to "ready"; an exit code if the worker can't go on.
    std::optional<int> start()
    {
        // Qt raster is made below, once the window it presents to exists.
        if (m_args.api != "qt") {
            m_backend = makeBackend(m_args.api);
            if (!m_backend) {
                emitLine(pr::ErrorEvent{{}, QStringLiteral("the %1 backend isn't in this build").arg(QString::fromStdString(m_args.api)), false});
                return kExitError;
            }
        }
        QString screenName;
        double scale = 1.0;
        if (!m_args.headless) {
            QScreen *screen = QGuiApplication::primaryScreen();
            if (!m_args.screen.isEmpty()) {
                screen = nullptr;
                for (QScreen *s : QGuiApplication::screens()) {
                    if (s->name() == m_args.screen)
                        screen = s;
                }
                if (!screen) {
                    std::fprintf(stderr, "synbench-gpu: unknown --screen '%s'\n", qPrintable(m_args.screen));
                    return kExitArgs;
                }
            }
            if (!screen) {
                emitLine(pr::ErrorEvent{{}, QStringLiteral("there is no screen to show the test window on"), false});
                return kExitError;
            }
            scale = screen->devicePixelRatio();
            m_scale = scale;
            const QRect avail = screen->availableGeometry();
            m_plan = planWindow({avail.width(), avail.height()}, scale);
            if (m_plan.tooSmall) {
                emitLine(pr::ErrorEvent{{}, QStringLiteral("screen too small for the test window"), true});
                return kExitNa;
            }
            screenName = screen->name();
            m_screenName = screenName;
            m_window = std::make_unique<WorkerWindow>(surfaceType(m_args.api), m_plan, screen);
        } else {
            m_plan.render = kLogical;
        }
        if (m_args.renderSize)
            m_plan.render = *m_args.renderSize;
        if (m_args.api == "qt")
            m_backend = makeQtRasterBackend(m_window.get());  // the native 2D path: Qt raster (task 26)
        m_native = dynamic_cast<INative2D *>(m_backend.get());

        BackendInit init;
        if (!m_args.gpu.isEmpty() && !parseGpuId(m_args.gpu, &init)) {
            std::fprintf(stderr, "synbench-gpu: bad --gpu '%s'\n", qPrintable(m_args.gpu));
            return kExitArgs;
        }
        if (!m_args.luid.isEmpty()) {
            // Vulkan and DXGI both select by LUID (checkpoint 3, M2): every API on the same physical GPU.
            std::array<quint8, 8> bytes{};
            parseLuidHex(m_args.luid, &bytes);
            for (std::size_t i = 0; i < 8; ++i)
                init.luid[i] = bytes[i];
            init.hasLuid = true;
        }
        init.renderWidth = std::uint32_t(m_plan.render.width);
        init.renderHeight = std::uint32_t(m_plan.render.height);
        init.allowSoftware = m_args.allowSoftware;
#if defined(Q_OS_LINUX)
        init.waylandSession = !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty() && QGuiApplication::platformName().startsWith(QLatin1StringView("wayland"));
#endif
        init.validation = m_args.validation;
        PresentTarget target = m_args.headless ? glue::headlessTarget() : glue::windowlessTarget(m_backend->api());
        if (m_backend->api() == Api::OpenGl) {
            // Task 21: the one GL context, current offscreen for init(); the window gets its
            // format (3.3 core, swap interval 0) before it is first shown.
            if (m_window)
                m_window->setFormat(glue::GlContext::format(m_args.validation));
            m_gl = std::make_unique<glue::GlContext>();
            if (const std::string e = m_gl->create(m_args.validation); !e.empty()) {
                const pr::ErrorEvent ev = errorEvent(nullptr, e);
                emitLine(ev);
                return ev.na ? kExitNa : kExitError;
            }
            target = m_gl->offscreenTarget(m_args.headless);
        }
        if (const std::string e = m_backend->init(init, target, &m_info); !e.empty()) {
            const pr::ErrorEvent ev = errorEvent(nullptr, e);
            emitLine(ev);
            return ev.na ? kExitNa : kExitError;
        }

        // The content, once per process (§5.1): every workload when serving,
        // the plan's otherwise.
        std::vector<Workload> wanted;
        if (m_args.serve)
            wanted.assign(kAllWorkloads.begin(), kAllWorkloads.end());
        for (const WorkloadPlan &p : m_runPlan)
            wanted.push_back(p.workload);
        for (const Workload w : wanted) {
            if (const auto kind = nativeKindOf(w)) {
                if (!m_nativeOps.count(w))
                    m_nativeOps.emplace(w, buildNativeOps(*kind, m_args.contentScale));
            } else if (!m_scenes.count(w)) {
                m_scenes.emplace(w, buildScene(w, m_args.contentScale));
            }
        }

        pr::ReadyEvent ready;
        ready.api = QString::fromStdString(m_args.api);
        ready.device = QString::fromStdString(m_info.deviceName);
        ready.version = QString::fromStdString(m_info.apiVersion);
        ready.driver = QString::fromStdString(m_info.driver);
        ready.present = QString::fromStdString(m_info.presentMode);
        ready.note = QString::fromStdString(m_info.note);
        ready.layers = QString::fromStdString(m_info.layers);
        ready.renderW = m_plan.render.width;
        ready.renderH = m_plan.render.height;
        ready.windowW = m_args.headless ? 0 : m_plan.windowLogical.width;  // logical px (window.size)
        ready.windowH = m_args.headless ? 0 : m_plan.windowLogical.height;
        ready.scale = scale;
        ready.screen = screenName;
        ready.fit = m_plan.fit;
        ready.gpuTimestamps = m_info.gpuTimestamps;
        emitLine(ready);

        Runner::Hooks hooks;
        hooks.stopRequested = [this] { return stopRequested(); };
        if (m_window) {
            hooks.windowOk = [this] {
#if defined(SYNBENCH_DEV_HOOKS)
                if (qEnvironmentVariable("SYNBENCH_GPU_FAKE_MOVE") == QLatin1StringView("measure"))
                    m_window->fakeMove();  // dev hook: the window "moved" during the measurement
#endif
                return m_window->windowOk();
            };
            hooks.windowReplan = [this] { return m_window->placementChanged() || m_fakeIndex < fakeSteps().size(); };
        }
        hooks.pumpEvents = [] { QCoreApplication::processEvents(); };
        hooks.stage = [](Workload w, const char *phase, int seconds) {
            emitLine(pr::StageEvent{QString::fromLatin1(workloadId(w)), QString::fromLatin1(phase), seconds});
        };
        m_runner = std::make_unique<Runner>(
            *m_backend, std::move(hooks),
            Runner::Settings{std::uint32_t(m_plan.render.width), std::uint32_t(m_plan.render.height), m_args.overlay});
        return std::nullopt;
    }

    void setPlan(std::vector<WorkloadPlan> plan) { m_runPlan = std::move(plan); }
    const std::vector<WorkloadPlan> &plan() const { return m_runPlan; }
    IBackend *backend() const { return m_backend.get(); }

    // One workload, start to end; emits its result or error.
    Outcome runOne(const WorkloadPlan &p)
    {
        Workload w = p.workload;
        PresentTarget target = glue::headlessTarget();
        if (m_window) {
            m_window->setWorkloadTitle(titleApiName(m_args.api), QString::fromLatin1(workloadName(w)));
            m_window->show();
            m_window->raise();
            m_window->requestActivate();
            // Wait for the first expose (§2.5: <= 2 s).
            QElapsedTimer waited;
            waited.start();
            while (!m_window->exposedSinceShow() && waited.elapsed() < 2000 && !stopRequested()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                if (!m_window->exposedSinceShow())
                    QThread::msleep(2);
            }
            if (stopRequested())
                return stopped(w);
            if (!m_window->exposedSinceShow()) {
                m_window->hide();
                emitLine(errorEvent(&w, "the test window didn't appear"));
                return Outcome::Error;
            }
        }
        // D1, DEF-1: the compositor, not this process, chooses the screen, and KWin reports the real
        // one only after the window's first frames. So the plan is made from the window as it is now
        // (its own fractional devicePixelRatio, DEF-3), and a change of screen or scale during the
        // warm-up re-plans, resizes and restarts the warm-up, at most 3 times. Only a change during
        // the measurement is an error.
        WorkloadResult r;
        std::string suspended;
        int replans = 0;
        for (;;) {
            if (m_window) {
                if (const auto outcome = replanFromWindow(w))
                    return *outcome;
                target = m_backend->api() == Api::Vulkan                                     ? glue::vulkanTarget(*m_window)
                         : m_backend->api() == Api::Dx12 || m_backend->api() == Api::Dx11 ? glue::nativeWindowTarget(*m_window)
                         : m_backend->api() == Api::OpenGl                                  ? m_gl->windowTarget(*m_window)
                                                                                          : glue::rasterTarget(*m_window);
            }
            if (const std::string e = m_backend->resume(target); !e.empty()) {
                if (m_gl)
                    m_gl->makeOffscreenCurrent();
                if (m_window)
                    m_window->hide();
                return failed(w, e);
            }
            m_present = m_backend->presentMode();
            if (nativeKindOf(w)) {
                if (!m_native)
                    return failed(w, "this API draws 3D scenes, not native 2D");
                r = m_runner->runNative(p, m_nativeOps.at(w), *m_native);
            } else {
                r = m_runner->run(p, m_scenes.at(w));
            }
            suspended = m_backend->suspend();
            // GL: the context leaves the window before it is hidden (a hidden Wayland
            // window has no surface); the backend's objects stay (Revised, task 21).
            if (m_gl)
                m_gl->makeOffscreenCurrent();
            if (r.error != kReplan)
                break;
            if (stopRequested())
                return stopped(w);
            if (++replans > 3) {
                if (m_window)
                    m_window->hide();
                emitLine(errorEvent(&w, "the test window kept moving between screens"));
                return Outcome::Error;
            }
        }
        // The window's reason before it is hidden: hiding clears its baseline, and every lost window
        // would read "hidden or minimised" (DEF-2).
        const QString windowProblem = m_window ? m_window->problem() : QString();
        if (m_window)
            m_window->hide();
        if (stopRequested())
            return stopped(w);
        if (r.error == kWindowLost && !windowProblem.isEmpty())
            r.error = windowProblem.toStdString();
        if (!r.error.empty())
            return failed(w, r.error);
        if (!suspended.empty())
            return failed(w, suspended);
        emitLine(resultEvent(r));
        return Outcome::Done;
    }

private:
    // The window's steps of SYNBENCH_GPU_FAKE_SCREEN ("WxH@scale;WxH@scale"): dev hook, see replanFromWindow().
    struct FakeStep {
        int w, h;
        double scale;
    };
    std::vector<FakeStep> fakeSteps() const
    {
        std::vector<FakeStep> steps;
#if defined(SYNBENCH_DEV_HOOKS)
        static const QRegularExpression one(QStringLiteral("^(\\d+)x(\\d+)@([0-9.]+)$"));
        for (const QString &part : qEnvironmentVariable("SYNBENCH_GPU_FAKE_SCREEN").split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            if (const auto m = one.match(part); m.hasMatch())
                steps.push_back({m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toDouble()});
        }
#endif
        return steps;
    }

    // Plans for the screen and scale the window has now, resizes it if the plan differs, waits for the
    // expose and takes the baseline. An outcome if the run can't go on (too small, Stop).
    std::optional<Outcome> replanFromWindow(Workload w)
    {
        QScreen *actual = m_window->screen();
        if (!actual)
            return std::nullopt;
        QRect avail = actual->availableGeometry();
        // The window's own ratio (fractional: 1.25), not QScreen's (Qt Wayland rounds that up, DEF-3).
        double scale = m_window->devicePixelRatio();
        const std::vector<FakeStep> steps = fakeSteps();
        if (!steps.empty()) {
            const FakeStep &step = steps[std::min(m_fakeIndex, steps.size() - 1)];
            avail = QRect(0, 0, step.w, step.h);
            scale = step.scale;
        }
        ++m_fakeIndex;
        const WindowPlan next = planWindow({avail.width(), avail.height()}, scale);
        if (next.tooSmall) {
            m_window->hide();
            emitLine(errorEvent(&w, "N/A: screen too small for the test window"));  // na:true, as at start (M1)
            return Outcome::ErrorNa;
        }
        m_screenName = actual->name();
        if (replan(next, scale)) {
            m_window->applyPlan(m_plan);
            QElapsedTimer settle;
            settle.start();
            while (!m_window->exposedSinceShow() && settle.elapsed() < 1000 && !stopRequested()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(2);
            }
            if (stopRequested())
                return stopped(w);
        }
        m_window->rebase();  // the baseline is the placement as it is now, resized or not
        return std::nullopt;
    }

    // The plan for the screen the window really got. True if the window size or the scale changed
    // (the render size follows the scale, unless --render-size is given).
    bool replan(const WindowPlan &next, double scale)
    {
        const bool changed = next.windowLogical != m_plan.windowLogical || scale != m_scale;
        m_scale = scale;
        if (!changed)
            return false;
        const Size render = m_args.renderSize ? *m_args.renderSize : next.render;
        const bool renderChanged = render != m_plan.render;
        m_plan = next;
        m_plan.render = render;
        if (renderChanged) {
            m_backend->setRenderSize(std::uint32_t(render.width), std::uint32_t(render.height));
            m_runner->setRenderSize(std::uint32_t(render.width), std::uint32_t(render.height));
        }
        return true;
    }

    bool stopRequested() const { return m_control.stop || (m_window && m_window->stopRequested()); }

    Outcome stopped(Workload w)
    {
        if (m_window) {
            m_window->hide();
            if (m_window->stopRequested())
                emitLine(errorEvent(&w, "stopped from the test window"));
        }
        return Outcome::Stopped;
    }

    Outcome failed(Workload w, const std::string &reason)
    {
        const pr::ErrorEvent e = errorEvent(&w, reason);
        emitLine(e);
        return e.na ? Outcome::ErrorNa : Outcome::Error;
    }

    pr::ResultEvent resultEvent(const WorkloadResult &r) const
    {
        const FrameStats s = frameStats(r.frameMs);
        pr::ResultEvent e;
        e.workload = QString::fromLatin1(workloadId(r.workload));
        e.frames = s.frames;
        e.seconds = s.seconds;
        e.fps = s.fps;
        e.ftP1 = s.p1;
        e.ftP50 = s.p50;
        e.ftP99 = s.p99;
        e.cpuMs = mean(r.cpuMs);
        if (m_info.gpuTimestamps && !r.gpuMs.empty())
            e.gpuMs = mean(r.gpuMs);
        e.series = bucketSeries(r.frameMs);
        e.signature = QByteArray(reinterpret_cast<const char *>(r.signature.data()), qsizetype(r.signature.size()));
        e.present = QString::fromStdString(m_present);
        if (m_window) {  // the screen it actually used (D1)
            e.screen = m_screenName;
            e.renderW = m_plan.render.width;
            e.renderH = m_plan.render.height;
            e.windowW = m_plan.windowLogical.width;
            e.windowH = m_plan.windowLogical.height;
            e.scale = m_scale;
            e.fit = m_plan.fit;
        }
        return e;
    }

public:
    ~Worker()
    {
        // The window's platform window (and a Vulkan surface Qt made for it)
        // goes before the backend's device and instance; then the backend,
        // then the window object.
        m_runner.reset();
        if (m_gl)
            m_gl->makeOffscreenCurrent();  // the GL backend frees its objects with the context current
        if (m_window)
            m_window->destroy();
        m_backend.reset();
        m_gl.reset();
    }

private:
    const Args &m_args;
    Control &m_control;
    WindowPlan m_plan;
    std::vector<WorkloadPlan> m_runPlan;
    std::map<Workload, SceneData> m_scenes;
    std::map<Workload, NativeOps> m_nativeOps;
    INative2D *m_native = nullptr;  // the backend, when it is a native 2D path
    BackendInfo m_info;
    // ~Worker() orders their destruction (the window's surface before the backend).
    std::unique_ptr<WorkerWindow> m_window;
    std::unique_ptr<IBackend> m_backend;
    std::unique_ptr<Runner> m_runner;
    std::unique_ptr<glue::GlContext> m_gl;  // OpenGL only (task 21)
    double m_scale = 1.0;
    std::size_t m_fakeIndex = 0;  // dev hook: how many re-plans have looked at SYNBENCH_GPU_FAKE_SCREEN
    QString m_screenName;
    std::string m_present;  // the last resume()'s present mode (the result's "present")
};

#if defined(SYNBENCH_DEV_HOOKS)
// The tests' check of the serve order (task 9): how often the null backend
// was resumed and suspended.
void reportNullCalls(const Args &a, IBackend *b)
{
    if (a.api == "null" && b)
        std::fprintf(stderr, "synbench-gpu: null backend: resume %d, suspend %d\n", static_cast<NullBackend *>(b)->resumeCount(),
                     static_cast<NullBackend *>(b)->suspendCount());
}
#endif

} // namespace

int main(int argc, char **argv)
{
    hardenProcess();
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args == std::vector<std::string>{"--self-test"})
        return selfTest();
#if defined(SYNBENCH_DEV_HOOKS)
    if (args == std::vector<std::string>{"--print-hashes"})
        return printHashes();
#endif
    const Args a = parseArgs(args);
    std::vector<WorkloadPlan> plan;
    if (a.planText) {
        QString error;
        const auto items = pr::parsePlan(*a.planText, &error);
        if (!items)
            failArgs(error.toStdString());
        if (items->isEmpty())
            failArgs("--plan is empty");
        std::string bad;
        plan = toPlan(*items, &bad);
        if (plan.empty())
            failArgs(bad);
    }

    // Qt sees only the program name (the options above are ours), plus the
    // offscreen platform when headless (no display needed).
    std::vector<char *> qtArgv{argv[0]};
    char platformOption[] = "-platform", offscreen[] = "offscreen";
    // A probe opens no window: the offscreen platform, so it never touches the user's display.
    // --probe-gl is not one: its context is made through the platform a real run uses (H2: Windows has no GL
    // on the offscreen platform; on Linux EGL on Wayland, GLX on X11), so the verdict matches the run.
    if (a.headless || a.mode == Mode::Probe) {
        qtArgv.push_back(platformOption);
        qtArgv.push_back(offscreen);
    }
    int qtArgc = int(qtArgv.size());
    qtArgv.push_back(nullptr);
    QGuiApplication app(qtArgc, qtArgv.data());

    Control control;
    control.serve = a.serve;
    // A probe is short and has no commands: it must not die on stdin EOF (run by hand, or with
    // </dev/null); the lifeline is for the long-lived run modes.
    if (a.mode == Mode::Run)
        startStdinThread(control);

    if (a.mode == Mode::Probe) {
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
        // Task 13 (Linux): the Vulkan devices merged with the DRM cards. Task 23 (Windows): the DXGI
        // adapters with their Direct3D availability, the Vulkan devices matched to them by LUID.
        std::string error;
        const std::vector<VkDeviceRecordStd> found = probeVulkanDevices(&error);
        QVector<VkDeviceRecord> records;
        for (const VkDeviceRecordStd &d : found) {
            VkDeviceRecord r;
            r.name = QString::fromStdString(d.name);
            r.vendorId = d.vendorId;
            r.deviceId = d.deviceId;
            r.type = QString::fromStdString(d.type);
            r.apiVersion = QString::fromStdString(d.apiVersion);
            r.driverName = QString::fromStdString(d.driverName);
            r.driverInfo = QString::fromStdString(d.driverInfo);
            r.deviceLocalBytes = d.deviceLocalBytes;
            r.pciSlot = QString::fromStdString(d.pciSlot);
            r.luidValid = d.luidValid;
            std::memcpy(r.luid, d.luid, sizeof r.luid);
            records.append(r);
        }
#if defined(Q_OS_LINUX)
        QVector<GpuEntry> gpus = mergeLinuxGpus(records, readDrmCards());
#else
        std::string dxgiError;
        const std::vector<dxgi::AdapterProbe> adapters = dxgi::probeAdapters(a.allowSoftware, &dxgiError);
        if (adapters.empty()) {
            emitLine(pr::ErrorEvent{{}, QString::fromStdString(dxgiError.empty() ? "no graphics adapter found" : dxgiError), false});
            return kExitError;
        }
        QVector<DxgiAdapterRecord> list;
        const auto api = [](const dxgi::ApiProbe &p) {
            ApiAvailability av;
            av.ok = p.ok;
            av.version = QString::fromStdString(p.version);
            av.reason = QString::fromStdString(p.reason);
            return av;
        };
        for (const dxgi::AdapterProbe &p : adapters) {
            DxgiAdapterRecord r;
            r.name = QString::fromStdString(p.name);
            r.vendorId = p.vendorId;
            r.deviceId = p.deviceId;
            r.software = p.software;
            r.dedicatedBytes = p.dedicatedBytes;
            r.sharedBytes = p.sharedBytes;
            r.umaKnown = p.umaKnown;
            r.uma = p.uma;
            std::memcpy(r.luid, p.luid.data(), sizeof r.luid);
            r.dx12 = api(p.dx12);
            r.dx11 = api(p.dx11);
            r.d2d = api(p.d2d);
            list.append(r);
        }
        QVector<GpuEntry> gpus = mergeWindowsGpus(list, records);
#endif
        for (GpuEntry &g : gpus) {
            // No loader at all: every card says so, not "this GPU has no Vulkan driver".
            if (error.rfind("N/A: ", 0) == 0 && !g.apis.value(QStringLiteral("vulkan")).ok)
                g.apis[QStringLiteral("vulkan")].reason = QString::fromStdString(error.substr(5));
            if (g.kind == GpuKind::Software && !a.allowSoftware)
                continue;  // P12: software rasterisers only through the dev hook
            emitLine(pr::GpuEvent{g});
        }
        emitLine(pr::DoneEvent{});
        return kExitOk;
#else
        emitLine(pr::ErrorEvent{{}, QStringLiteral("the GPU probe isn't in this build"), false});
        return kExitError;
#endif
    }
    if (a.mode == Mode::ProbeGl) {
        // Task 23: a context made exactly as the run's (GlContext::create, 3.3 core), described; the
        // GUI side verifies it against the chosen GPU (gl_env.h's verifyGl). The environment (the
        // offload variables) comes from the client.
        glue::GlContext context;
        const std::string made = context.create(false);
        pr::GlEvent gl;
        const glue::GlInfo info = made.empty() ? context.info() : glue::GlContext::defaultInfo();
        gl.ok = made.empty();
        gl.renderer = QString::fromStdString(info.renderer);
        gl.vendor = QString::fromStdString(info.vendor);
        const QString version = QString::fromStdString(info.version);  // "4.6.0 NVIDIA 550.1", "4.5 (Core Profile) Mesa 26.2"
        const QRegularExpressionMatch number = QRegularExpression(QStringLiteral("\\d+\\.\\d+")).match(version);
        if (made.empty()) {
            gl.version = QStringLiteral("OpenGL ") + (number.hasMatch() ? number.captured(0) : QStringLiteral("3.3"));
        } else {
            gl.version = version;  // "1.1.0": what a context without 3.3 core is
            gl.reason = QString::fromStdString(made.rfind("N/A: ", 0) == 0 ? made.substr(5) : made);
        }
        gl.vendorId = glVendorToPci(gl.vendor).value_or(0);
        emitLine(gl);
        emitLine(pr::DoneEvent{});
        return kExitOk;
    }
    if (a.mode != Mode::Run) {
        emitLine(pr::ErrorEvent{{}, QStringLiteral("unknown mode"), false});
        return kExitError;
    }

    Worker worker(a, control);
    worker.setPlan(plan);
    if (const auto code = worker.start())
        return *code;

    int code = kExitOk;
    if (a.serve) {
        control.onRun = [&](WorkloadPlan p) {
            if (worker.runOne(p) == Outcome::Stopped) {
                QCoreApplication::exit(kExitStopped);
                return;
            }
            control.busy = false;  // before the idle line: the client's next run may follow at once
            emitLine(pr::IdleEvent{});
        };
        control.onQuit = [] {
            emitLine(pr::DoneEvent{});
            QCoreApplication::exit(kExitOk);
        };
        control.onStop = [] { QCoreApplication::exit(kExitStopped); };
        code = app.exec();
    } else {
        for (const WorkloadPlan &p : worker.plan()) {
            const Outcome o = worker.runOne(p);
            if (o != Outcome::Done) {
                code = o == Outcome::Stopped ? kExitStopped : o == Outcome::ErrorNa ? kExitNa : kExitError;
                break;
            }
        }
        if (code == kExitOk)
            emitLine(pr::DoneEvent{});
    }
#if defined(SYNBENCH_DEV_HOOKS)
    reportNullCalls(a, worker.backend());
#endif
    return code;
}

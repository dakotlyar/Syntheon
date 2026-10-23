// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/gpu_test.h"

#include "core/units.h"
#include "tests/common/stub_clock.h"
#include "tests/cpu/cpu_context.h"
#include "tests/cpu/engine/time_scale.h"
#include "tests/gpu/client/gpu_client.h"
#include "tests/gpu/formula_v1.h"
#include "tests/gpu/model/best_of.h"
#include "tests/gpu/model/frame_stats.h"
#include "tests/gpu/model/gl_env.h"
#include "tests/gpu/model/ids.h"
#include "sysinfo/system_info.h"
#include "tests/gpu/model/render_size.h"

#include <QGuiApplication>
#include <QMutexLocker>
#include <QSysInfo>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace sb::tests {

using namespace sb::gpu;
using namespace std::chrono_literals;
namespace pr = sb::gpu::protocol;

namespace {

const QString kScenes3dId = QStringLiteral("scenes3d"), kNative2dId = QStringLiteral("native2d"), kFill2dId = QStringLiteral("fill2d");

#if defined(Q_OS_WIN)
constexpr int kNativePaths = 2;  // Direct2D and Qt raster
constexpr int kDefaultApis = 4;
#else
constexpr int kNativePaths = 1;  // Qt raster
constexpr int kDefaultApis = 2;  // Vulkan and OpenGL
#endif

const char *const kApiKeys[] = {"vulkan", "dx12", "dx11", "opengl"};

#if defined(SYNBENCH_DEV_HOOKS)
// Dev hook: SYNBENCH_GPU_FAKE_APIS=vulkan,opengl treats those APIs as available on any GPU
// (the tests serve them all with the null backend).
bool fakeApi(ApiId id)
{
    return qEnvironmentVariable("SYNBENCH_GPU_FAKE_APIS").split(QLatin1Char(','), Qt::SkipEmptyParts).contains(apiKey(id));
}
#else
bool fakeApi(ApiId) { return false; }
#endif

Metric number(const QString &key, const QString &sub, const QString &label, double value, const QString &unit, Direction dir = Direction::Neutral)
{
    Metric m;
    m.key = key;
    m.subTestId = sub;
    m.label = label;
    m.value = value;
    m.unit = unit;
    m.direction = dir;
    return m;
}

Metric text(const QString &key, const QString &sub, const QString &label, const QString &value)
{
    Metric m;
    m.key = key;
    m.subTestId = sub;
    m.label = label;
    m.value = value;
    return m;
}

std::chrono::milliseconds scaled(int ms)
{
    return cpu::scaledWindow(std::chrono::milliseconds(ms));
}

QString kindName(GpuKind k)
{
    return kindText(k);
}

QString sessionName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows");
#else
    const QString t = qEnvironmentVariable("XDG_SESSION_TYPE");
    return t == QLatin1StringView("wayland") ? QStringLiteral("Wayland") : t == QLatin1StringView("x11") ? QStringLiteral("X11") : t;
#endif
}

// The default GPU among those with a usable 3D API (a card without a driver, such as a
// GPU only the kernel knows, is never the default); software ones count here because
// the worker lists them only under its dev hook (P12).
std::optional<int> chooseDefault(const QVector<GpuEntry> &gpus)
{
    QVector<GpuEntry> usable;
    QVector<int> where;
    for (int i = 0; i < gpus.size(); ++i) {
        bool any = false;
        for (const char *key : kApiKeys)
            any = any || gpus[i].apis.value(QString::fromLatin1(key)).ok;
        if (any) {
            usable.append(gpus[i]);
            where.append(i);
        }
    }
    const auto def = defaultGpuIndex(usable, true);
    return def ? std::optional<int>(where[*def]) : std::nullopt;
}

// §4.2: OpenGL's availability per GPU, from one `--probe-gl` each (the GPU test's own environment:
// the offload variables of D17) verified against the machine's list; the default GPU first, within
// the time left until `deadline` (D22). A GPU whose entry already has an opengl answer is left as it
// is; one the time didn't reach has none, and its OpenGL worker checks it when the test starts.
// A Wayland session: WAYLAND_DISPLAY is set and Qt runs on the wayland platform (Dave's OpenGL rule).
bool waylandSession()
{
#if defined(Q_OS_LINUX)
    return !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty() && QGuiApplication::platformName().startsWith(QLatin1StringView("wayland"));
#else
    return false;
#endif
}

void probeOpenGl(QVector<GpuEntry> &list, std::chrono::steady_clock::time_point deadline)
{
#if defined(Q_OS_LINUX)
    const bool linuxArm = QSysInfo::currentCpuArchitecture() == QLatin1StringView("arm64"), windowsArm = false;
#elif defined(Q_OS_WIN)
    const bool linuxArm = false, windowsArm = QSysInfo::currentCpuArchitecture() == QLatin1StringView("arm64");
#else
    const bool linuxArm = false, windowsArm = false;
#endif
    QVector<int> order;
    if (const auto def = chooseDefault(list))
        order.append(*def);
    for (int i = 0; i < list.size(); ++i) {
        if (!order.contains(i))
            order.append(i);
    }
    probeOpenGlOver(list, order, deadline, linuxArm, windowsArm, [](const GpuEntry &g, std::chrono::milliseconds left) {
        return GpuClient::probeGl(g, glEnvironment(g), left);
    }, waylandSession());
}

// One native 2D path's state through a run (§7.1, §8): Direct2D (Windows, GPU, shown) then Qt raster (CPU, scored).
struct NativeRun {
    NativePath path = NativePath::QtRaster;
    bool available = false;    // a worker runs for it
    bool errored = false;
    QString status;            // "OK", "N/A: ...", "Error: ..."
    QString errorWorkload, errorReason;  // the first workload without a result, and why
    QString present;
    QStringList notes;         // warnings (D23 a, a fallback the worker reported)
    std::optional<pr::ReadyEvent> ready;
    std::map<NativeWorkload, pr::ResultEvent> results;
};

// §9's frame floor (n >= 20). Dev hook SYNBENCH_GPU_MIN_FRAMES=<n >= 1> lowers it for the CI smoke only, whose windows are
// 0.03 x the real ones (SYNBENCH_SMOKE): 600 ms of Geometry on a CI runner's lavapipe is about 20 frames, so the real floor
// there would test the runner's speed, not the GPU path. A packaged build always uses 20.
int minFrames()
{
#if defined(SYNBENCH_DEV_HOOKS)
    bool ok = false;
    const int n = qEnvironmentVariableIntValue("SYNBENCH_GPU_MIN_FRAMES", &ok);
    if (ok && n >= 1)
        return n;
#endif
    return kMinFrames;
}

// The fill workloads' real per-frame counts (L3): 128 layers and 100,000 sprites, divided as the worker's scenes divide
// them (max(1, ceil(n / scale)), scenes.cpp) when the dev hook SYNBENCH_GPU_WORKER_ARGS passes --content-scale (smoke, CI).
// A packaged build has no worker arguments: the full counts.
double fillCount(FillId f)
{
    const double n = f == FillId::Quads ? double(kFillQuadLayers) : double(kFillSprites);
    const QStringList args = GpuClient::extraArgs();
    const qsizetype at = args.lastIndexOf(QStringLiteral("--content-scale"));
    bool ok = false;
    const double scale = at >= 0 && at + 1 < args.size() ? args[at + 1].toDouble(&ok) : 1.0;
    if (!ok || !(scale > 1.0))
        return n;
    return std::max(1.0, std::ceil(n / scale));
}

// Dev hook: SYNBENCH_GPU_FAKE_D2D=1 runs the Direct2D path on any OS (the tests serve it with the fake worker).
bool fakeD2d()
{
#if defined(SYNBENCH_DEV_HOOKS)
    return qEnvironmentVariable("SYNBENCH_GPU_FAKE_D2D") == QLatin1StringView("1");
#else
    return false;
#endif
}

QVector<pr::PlanItem> nativePlan()
{
    // D14: warm-up 1 s, measure 10 s per workload, at the dev time scale.
    QVector<pr::PlanItem> plan;
    for (const NativeWorkload w : kNativeWorkloads)
        plan.append({nativeKey(w), int(scaled(1000).count()), int(scaled(10000).count())});
    return plan;
}

struct ApiRun;
QString naReason(const std::vector<ApiRun> &apis);

// One API's state through a run.
struct ApiRun {
    ApiId id;
    bool active = false;       // a worker is running for it
    bool errored = false;
    QString status;            // "OK", "N/A: ...", "Error: ..."
    QString errorScene;        // first Error: the workload (scene or fill name) and reason
    QString errorReason;
    QString errorSub;          // the sub-test that workload belongs to (scenes3d or fill2d)
    std::optional<pr::ReadyEvent> ready;
    QString present;
    QStringList notes;         // warnings for api.<api>.note (§9: GPU timestamps that look wrong)
    std::unique_ptr<GpuSession> session;
    std::map<SceneId, pr::ResultEvent> results;
    std::map<FillId, pr::ResultEvent> fills;   // Quads and Sprites (§6.5, §6.6), after the four scenes
};

// A workload of the 3D plan: one of the four scenes or one of the two fill workloads.
struct WorkloadRef {
    std::optional<SceneId> scene;
    std::optional<FillId> fill;
    QString name() const { return scene ? sceneName(*scene) : fillName(*fill); }
    QString sub() const { return scene ? kScenes3dId : kFill2dId; }
};
WorkloadRef workloadRef(const QString &key) { return {sceneFromKey(key), fillFromKey(key)}; }

// Why no API ran, in the APIs' own words: their distinct N/A reasons ("No Vulkan driver is
// installed; DirectX is Windows only"), or the generic text when there are none.
QString naReason(const std::vector<ApiRun> &apis)
{
    QStringList reasons;
    for (const ApiRun &a : apis) {
        QString r = a.status;
        if (r.startsWith(QLatin1StringView("N/A: ")))
            r.remove(0, 5);
        else
            continue;
        if (!reasons.contains(r))
            reasons << r;
    }
    return reasons.isEmpty() ? QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found") : reasons.join(QStringLiteral("; "));
}

// One GPU test run (§8): the sessions, the scene-by-scene loop, the metrics and the status.
class GpuRun
{
public:
    GpuRun(RunContext &ctx, const GpuEntry &gpu) : m_ctx(ctx), m_gpu(gpu) {}

    TestResult run();

private:
    bool cancelled() const { return m_ctx.cancelled(); }
    void report(double at, const QString &stage)
    {
        m_last = std::clamp(std::max(m_last, at / m_total), 0.0, 0.999);  // never backwards
        m_ctx.progress(m_last, stage);
    }
    void fail(ApiRun &a, const QString &workloadName, const QString &reason, const QString &sub = kScenes3dId)
    {
        a.errored = true;
        a.active = false;
        a.status = QStringLiteral("Error: ") + reason;
        if (a.errorScene.isEmpty()) {
            a.errorScene = workloadName;
            a.errorReason = reason;
            a.errorSub = sub;
        }
        if (a.session)
            a.session->kill();  // its remaining workloads are skipped
    }
    void compareSignatures(int item, const QVector<pr::PlanItem> &plan);
    // §8: after the 3D workers, Direct2D (Windows) then Qt raster, one worker per path. `done` is the planned seconds so far.
    void runNatives(double *done, bool *stopped);
    TestResult finish(bool stopped);

    RunContext &m_ctx;
    const GpuEntry m_gpu;
    std::vector<ApiRun> m_apis;
    std::vector<NativeRun> m_natives;  // Direct2D, Qt raster
    TestResult m_r;
    double m_total = 1, m_last = 0;
    std::map<std::pair<ApiId, SceneId>, QString> m_mismatch;  // api, scene -> "drew X differently from Y (mean 9.3/255)"
    std::map<std::pair<ApiId, FillId>, QString> m_fillMismatch;  // the same for the fill workloads
};

void GpuRun::compareSignatures(int item, const QVector<pr::PlanItem> &plan)
{
    const WorkloadRef w = workloadRef(plan[item].workload);
    const QString workloadName = w.name();
    // §5.7, D20: in run order; an API that didn't run (or sent no usable signature) is an empty entry.
    QVector<QByteArray> sigs;
    int ran = 0;
    for (const ApiRun &a : m_apis) {
        const pr::ResultEvent *res = nullptr;
        if (w.scene) {
            const auto it = a.results.find(*w.scene);
            res = it != a.results.end() ? &it->second : nullptr;
        } else {
            const auto it = a.fills.find(*w.fill);
            res = it != a.fills.end() ? &it->second : nullptr;
        }
        const bool have = a.active && res && res->signature.size() == 432;
        sigs.append(have ? res->signature : QByteArray());
        ran += have ? 1 : 0;
    }
    if (ran < 2)
        return;  // a single API: no comparison
    const SignatureVerdict verdict = judgeSignatures(sigs);
    for (const int i : verdict.outliers) {
        ApiRun &a = m_apis[std::size_t(i)];
        const QString why = QStringLiteral("drew %1 differently from %2 (mean %3/255)")
                                .arg(workloadName, apiName(m_apis[std::size_t(verdict.reference)].id))
                                .arg(verdict.diffToReference[i].mean, 0, 'f', 1);
        if (w.scene)
            m_mismatch[{a.id, *w.scene}] = why;
        else
            m_fillMismatch[{a.id, *w.fill}] = why;
        m_ctx.log(LogLevel::Error, QStringLiteral("%1: %2").arg(apiName(a.id), why));
        fail(a, workloadName, why, w.sub());  // an Error (D20): the score is withheld through D6
    }
}

TestResult GpuRun::run()
{
    QVector<pr::PlanItem> basePlan = GpuTest::plan3d();
    basePlan += GpuTest::planFill();  // §8: Quads and Sprites follow the four scenes
    QVector<pr::PlanItem> plan;
    for (const pr::PlanItem &p : basePlan)
        plan.append({p.workload, int(scaled(p.warmupMs).count()), int(scaled(p.measureMs).count())});

    // The APIs that run, and why each of the others doesn't (§4.2).
    for (const ApiId id : kApis) {
        ApiRun a;
        a.id = id;
        // Every API the probe found runs, scene by scene (D12). One the probe has no answer for
        // (OpenGL when the 3 s probe budget ran out) is started anyway: its worker checks itself and
        // answers N/A when it can't run.
        const auto found = m_gpu.apis.constFind(apiKey(id));
        if (fakeApi(id) || found == m_gpu.apis.cend() || found->ok) {
            a.active = true;
        } else {
            a.status = QStringLiteral("N/A: ") + (found->reason.isEmpty() ? QStringLiteral("not available") : found->reason);
        }
        m_apis.push_back(std::move(a));
    }
    int activeCount = 0;
    for (const ApiRun &a : m_apis)
        activeCount += a.active ? 1 : 0;
    if (activeCount == 0) {
        TestResult r = TestResult::notAvailable(naReason(m_apis));
        for (const ApiRun &a : m_apis)
            r.metrics.append(text(apiStatusKey(apiKey(a.id), u"status"), {}, apiName(a.id) + QStringLiteral(" status"), a.status));
        return r;
    }

    // The windows' total (the budget), once the APIs are known.
    double perApi = 0;
    for (const pr::PlanItem &p : plan)
        perApi += double(p.warmupMs + p.measureMs) / 1000.0;
    m_total = perApi * activeCount + 10.0;  // + starting the workers and switching windows
    // The native 2D paths (§7.1): Direct2D only where the probe says it is available (Windows), Qt raster always.
    {
        NativeRun d2d;
        d2d.path = NativePath::Direct2D;
        const auto entry = m_gpu.apis.constFind(QStringLiteral("d2d"));
#if defined(Q_OS_WIN)
        d2d.available = entry != m_gpu.apis.cend() && entry->ok;
#endif
        if (fakeD2d())
            d2d.available = true;
        if (!d2d.available) {
            // The probe's own reason; with no entry, the truth: on Windows the GPU list came from the fallback (never checked).
#if defined(Q_OS_WIN)
            const QString unchecked = QStringLiteral("Direct2D wasn't checked (the GPU list came from the fallback)");
#else
            const QString unchecked = QStringLiteral("Direct2D is Windows only");
#endif
            d2d.status = QStringLiteral("N/A: ") + (entry != m_gpu.apis.cend() && !entry->reason.isEmpty() ? entry->reason : unchecked);
        }
        NativeRun qt;
        qt.path = NativePath::QtRaster;
        qt.available = true;
        m_natives = {d2d, qt};
        double perNative = 2.0;  // the start
        for (const pr::PlanItem &p : nativePlan())
            perNative += double(p.warmupMs + p.measureMs) / 1000.0;
        for (const NativeRun &n : m_natives)
            m_total += n.available ? perNative : 0.0;
    }

    // M8 / the mockup: the row says where the test is before the interface is held.
    m_ctx.progress(0, QStringLiteral("Running in its own window"));
    m_ctx.log(LogLevel::Info, QStringLiteral("Starting GPU: %1 (%2), %3 3D API(s), about %4")
                                  .arg(m_gpu.name, kindName(m_gpu.kind))
                                  .arg(activeCount)
                                  .arg(GpuTest::estimateSeconds(activeCount, kNativePaths) / 60 > 0
                                           ? QStringLiteral("%1 min").arg(GpuTest::estimateSeconds(activeCount, kNativePaths) / 60)
                                           : QStringLiteral("%1 s").arg(GpuTest::estimateSeconds(activeCount, kNativePaths))));
    m_ctx.holdInterface(true);
    if (const auto load = cpu::sampleBackgroundLoadPercent(cpu::scaledWindow(1000ms), [this] { return cancelled(); }))
        m_r.metrics.append(number(QStringLiteral("background.load"), {}, QStringLiteral("Background load"), *load, QStringLiteral("%")));

    // Start every worker at once, then wait for each (§2.5).
    for (ApiRun &a : m_apis) {
        if (!a.active)
            continue;
        a.session = std::make_unique<GpuSession>();
        QString err;
        // D17: OpenGL runs in the environment that puts it on the chosen GPU (the offload variables).
        if (!a.session->start(apiKey(a.id), m_gpu.id, a.id == ApiId::OpenGl ? glEnvironment(m_gpu) : QProcessEnvironment(), &err, m_gpu.luid))
            fail(a, QStringLiteral("starting"), err);
    }
    bool stopped = cancelled();
    for (ApiRun &a : m_apis) {
        if (!a.active || stopped)
            continue;
        const ClientOutcome ready = a.session->waitReady(std::chrono::seconds(30), [this] { return cancelled(); });
        switch (ready.status) {
        case ClientOutcome::Status::Ok:
            a.ready = ready.ready;
            m_ctx.log(LogLevel::Info, QStringLiteral("%1 ready on %2").arg(apiName(a.id), ready.ready ? ready.ready->device : QString()));
            break;
        case ClientOutcome::Status::NotAvailable:
            a.active = false;
            a.status = QStringLiteral("N/A: ") + ready.reason;
            a.session->kill();
            m_ctx.log(LogLevel::Info, QStringLiteral("%1: N/A (%2)").arg(apiName(a.id), ready.reason));
            break;
        case ClientOutcome::Status::Cancelled:
            stopped = true;
            break;
        case ClientOutcome::Status::Failed:
            fail(a, QStringLiteral("starting"), ready.reason);
            m_ctx.log(LogLevel::Error, QStringLiteral("%1: %2").arg(apiName(a.id), ready.reason));
            break;
        }
    }

    // §8: each workload on every API before the next one (D12).
    double done = 4.0;
    for (int item = 0; item < plan.size() && !stopped; ++item) {
        const pr::PlanItem &p = plan[item];
        const WorkloadRef work = workloadRef(p.workload);
        const QString workName = work.name();
        const double windows = double(p.warmupMs + p.measureMs) / 1000.0;
        for (ApiRun &a : m_apis) {
            if (!a.active || stopped)
                continue;
            m_ctx.setPart(QStringLiteral("%1 · %2").arg(workName, apiName(a.id)));
            const auto stage = [&](const QString &what) {
                return QStringLiteral("%1 on %2 · %3").arg(workName, apiShort(a.id), what);
            };
            report(done, stage(QStringLiteral("starting")));
            const ClientOutcome o = a.session->runWorkload(p, [this] { return cancelled(); }, [&](const pr::Event &e) {
                const auto *s = std::get_if<pr::StageEvent>(&e);
                if (!s)
                    return;
                if (s->phase == QLatin1StringView("warmup")) {
                    report(done + double(s->seconds), stage(QStringLiteral("warming up")));
                } else if (s->phase == QLatin1StringView("measure")) {
                    report(done + double(p.warmupMs) / 1000.0 + double(s->seconds),
                           stage(QStringLiteral("%1 s of %2 s").arg(s->seconds).arg(int(std::lround(double(p.measureMs) / 1000.0)))));
                }
            });
            switch (o.status) {
            case ClientOutcome::Status::Ok: {
                pr::ResultEvent res = o.results.first();
                // §9's plausibility rules (H1): an implausible frame rate or fewer than 20 frames is an API Error in this
                // workload (its numbers aren't stored); a median GPU time over 1.5 x p50 drops `gpu` with a warning.
                const Plausibility check = plausibility(res.frames, res.fps, res.ftMin, res.ftP50, res.gpuMs, minFrames());
                if (!check.error.isEmpty()) {
                    fail(a, workName, check.error, work.sub());
                    m_ctx.log(LogLevel::Error, QStringLiteral("%1 failed in %2: %3 (%4 frames, %5 fps)")
                                                   .arg(apiName(a.id), workName, check.error).arg(res.frames).arg(res.fps, 0, 'f', 1));
                    break;
                }
                if (check.dropGpu) {
                    m_ctx.log(LogLevel::Warning, QStringLiteral("%1 on %2: GPU time %3 ms against a frame time p50 of %4 ms: %5")
                                                     .arg(workName, apiName(a.id)).arg(*res.gpuMs, 0, 'f', 3).arg(res.ftP50, 0, 'f', 3).arg(kGpuTimeWarning));
                    res.gpuMs.reset();
                    if (!a.notes.contains(kGpuTimeWarning))
                        a.notes << kGpuTimeWarning;
                }
                if (work.scene)
                    a.results[*work.scene] = res;
                else
                    a.fills[*work.fill] = res;
                if (a.present.isEmpty())
                    a.present = res.present;
                m_ctx.log(LogLevel::Info, QStringLiteral("%1 on %2: %3 fps").arg(workName, apiName(a.id)).arg(res.fps, 0, 'f', 1));
                break;
            }
            case ClientOutcome::Status::Cancelled:
                stopped = true;
                break;
            case ClientOutcome::Status::NotAvailable:
                a.active = false;  // a run-time N/A (a FIFO-only surface): the API isn't available, not an error
                a.status = QStringLiteral("N/A: ") + o.reason;
                a.session->kill();
                m_ctx.log(LogLevel::Info, QStringLiteral("%1: N/A (%2)").arg(apiName(a.id), o.reason));
                break;
            case ClientOutcome::Status::Failed:
                fail(a, workName, o.reason, work.sub());
                m_ctx.log(LogLevel::Error, QStringLiteral("%1 failed in %2: %3").arg(apiName(a.id), workName, o.reason));
                break;
            }
            // Only a window that ran counts: an API that dropped out skips its windows, so the progress
            // never runs ahead of the work (the orchestrator sets 1.0 at the end).
            if (o.status == ClientOutcome::Status::Ok)
                done += windows;
        }
        if (!stopped)
            compareSignatures(item, plan);
    }
    for (ApiRun &a : m_apis) {
        if (!a.session || !a.session->alive())
            continue;
        if (stopped || cancelled())
            a.session->kill();  // Stop: no polite quit per API, so it stays within 250 ms with several of them (L6)
        else
            a.session->quit();
    }
    if (!stopped && !cancelled())
        runNatives(&done, &stopped);
    m_ctx.holdInterface(false);
    return finish(stopped || cancelled());
}

void GpuRun::runNatives(double *done, bool *stopped)
{
    const QVector<pr::PlanItem> plan = nativePlan();
    for (NativeRun &n : m_natives) {
        if (*stopped || cancelled())
            break;
        if (!n.available)
            continue;
        const QString pathName = nativePathName(n.path);
        m_ctx.setPart(QStringLiteral("Native 2D · %1").arg(pathName));
        const auto stage = [&](const QString &what) { return QStringLiteral("%1 · %2").arg(pathName, what); };
        report(*done, stage(QStringLiteral("starting")));
        ClientRunSpec spec;
        spec.api = nativePathKey(n.path);
        spec.gpuId = m_gpu.id;
        spec.luid = m_gpu.luid;
        spec.plan = plan;
        double itemStart = *done + 2.0;  // the worker's start
        const ClientOutcome o = GpuClient::run(spec, [this] { return cancelled(); }, [&](const pr::Event &e) {
            const auto *s = std::get_if<pr::StageEvent>(&e);
            if (!s)
                return;
            double before = 0;  // the windows of the workloads before this one
            for (const pr::PlanItem &p : plan) {
                if (p.workload == s->workload)
                    break;
                before += double(p.warmupMs + p.measureMs) / 1000.0;
            }
            const auto item = std::find_if(plan.begin(), plan.end(), [&](const pr::PlanItem &p) { return p.workload == s->workload; });
            const double warm = item == plan.end() ? 0.0 : double(item->warmupMs) / 1000.0;
            const auto kind = nativeFromKey(s->workload);
            const QString what = kind ? nativeName(*kind) : s->workload;
            if (s->phase == QLatin1StringView("warmup"))
                report(itemStart + before + double(s->seconds), stage(what + QStringLiteral(" · warming up")));
            else if (s->phase == QLatin1StringView("measure"))
                report(itemStart + before + warm + double(s->seconds), stage(what));
        });
        n.ready = o.ready;
        n.notes += o.warnings;
        for (const pr::ResultEvent &r : o.results) {
            if (const auto w = nativeFromKey(r.workload)) {
                n.results[*w] = r;
                if (n.present.isEmpty())
                    n.present = r.present;
                if (r.seconds > 0)
                    m_ctx.log(LogLevel::Info, QStringLiteral("%1 on %2: %3 MOps/s")
                                                  .arg(nativeName(*w), pathName)
                                                  .arg(double(nativeOpsPerFrame(*w)) * double(r.frames) / r.seconds / 1e6, 0, 'f', 1));
            }
        }
        double windows = 2.0;
        for (const pr::PlanItem &p : plan)
            windows += double(p.warmupMs + p.measureMs) / 1000.0;
        // The first workload with no result is the one that failed (or the one the worker never reached).
        QString failedIn;  // empty: every workload finished, or none did (start-up)
        for (const NativeWorkload w : kNativeWorkloads) {
            if (!n.results.count(w) && !n.results.empty()) {
                failedIn = nativeName(w);
                break;
            }
        }
        const bool everyResult = n.results.size() == std::size_t(kNativeWorkloads.size());
        switch (o.status) {
        case ClientOutcome::Status::Ok:
            *done += windows;
            break;
        case ClientOutcome::Status::Cancelled:
            *stopped = true;
            break;
        case ClientOutcome::Status::NotAvailable:
            n.available = false;
            n.status = QStringLiteral("N/A: ") + o.reason;
            m_ctx.log(LogLevel::Info, QStringLiteral("%1: N/A (%2)").arg(pathName, o.reason));
            break;
        case ClientOutcome::Status::Failed:
            *done += windows;  // its windows are over: the progress never runs ahead of the work
            if (everyResult) {
                // All four results arrived and the worker then exited badly: the numbers stand (a note, not an error).
                n.notes << QStringLiteral("the %1 worker exited with code %2 after finishing").arg(pathName).arg(o.exitCode);
                m_ctx.log(LogLevel::Warning, QStringLiteral("%1: the worker exited with code %2 after finishing").arg(pathName).arg(o.exitCode));
                break;
            }
            n.errored = true;
            n.errorWorkload = failedIn;  // empty: it never produced a result (start-up)
            n.errorReason = o.reason;
            n.status = QStringLiteral("Error: ") + o.reason;
            // D23 a: a Direct2D failure is a warning and its own status, never an API error for D6.
            if (n.path == NativePath::Direct2D)
                n.notes << QStringLiteral("Direct2D failed during %1 (%2). It isn't scored, so the GPU score stands.")
                               .arg(failedIn.isEmpty() ? QStringLiteral("start-up") : failedIn, o.reason);
            m_ctx.log(n.path == NativePath::Direct2D ? LogLevel::Warning : LogLevel::Error,
                      QStringLiteral("%1 failed %2: %3").arg(pathName, failedIn.isEmpty() ? QStringLiteral("during start-up") : QStringLiteral("in ") + failedIn, o.reason));
            break;
        }
    }
}

TestResult GpuRun::finish(bool stopped)
{
    TestResult &r = m_r;
    const QString scenes = kScenes3dId;

    // The machine, the GPU and each API.
    r.metrics.append(text(QStringLiteral("gpu.name"), {}, QStringLiteral("GPU"), m_gpu.name));
    r.metrics.append(text(QStringLiteral("gpu.id"), {}, QStringLiteral("GPU id"), m_gpu.id));
    r.metrics.append(text(QStringLiteral("gpu.kind"), {}, QStringLiteral("GPU kind"), kindName(m_gpu.kind)));
    r.metrics.append(text(QStringLiteral("gpu.vram"), {}, QStringLiteral("Video memory"), vramText(m_gpu.vramBytes)));
    r.metrics.append(text(QStringLiteral("session"), {}, QStringLiteral("Session"), sessionName()));
    for (const ApiRun &a : m_apis) {
        const QString key = apiKey(a.id);
        const QString name = apiName(a.id);
        QString status = a.status;
        if (status.isEmpty())  // an API a Stop cut short never reads "OK" (L4)
            status = stopped && a.results.size() + a.fills.size() < kScenes.size() + kFills.size() ? QStringLiteral("Stopped") : QStringLiteral("OK");
        r.metrics.append(text(apiStatusKey(key, u"status"), {}, name + QStringLiteral(" status"), status));
        if (a.ready) {
            const ApiAvailability av = m_gpu.apis.value(key);
            r.metrics.append(text(apiStatusKey(key, u"version"), {}, name + QStringLiteral(" version"), av.version.isEmpty() ? a.ready->version : av.version));
            r.metrics.append(text(apiStatusKey(key, u"driver"), {}, name + QStringLiteral(" driver"), av.driver.isEmpty() ? a.ready->driver : av.driver));
            if (!a.present.isEmpty())
                r.metrics.append(text(apiStatusKey(key, u"present"), {}, name + QStringLiteral(" present mode"), a.present));
            QStringList notes = a.notes;
            if (!a.ready->note.isEmpty())
                notes.prepend(a.ready->note);
            if (!notes.isEmpty())
                r.metrics.append(text(apiStatusKey(key, u"note"), {}, name + QStringLiteral(" note"), notes.join(QStringLiteral(" · "))));
            if (!a.ready->layers.isEmpty())
                r.metrics.append(text(apiStatusKey(key, u"layers"), {}, name + QStringLiteral(" layers"), a.ready->layers));
        }
    }
    // The window: the first API that measured says, with what its result recorded (the screen
    // the window really landed on, D1); else what `ready` said.
    for (const ApiRun &a : m_apis) {
        if (!a.ready)
            continue;
        pr::ReadyEvent e = *a.ready;
        if (!a.results.empty() && !a.results.begin()->second.screen.isEmpty()) {
            const pr::ResultEvent &res = a.results.begin()->second;
            e.screen = res.screen;
            e.renderW = res.renderW;
            e.renderH = res.renderH;
            e.windowW = res.windowW;
            e.windowH = res.windowH;
            e.scale = res.scale;
            e.fit = res.fit;
        }
        r.metrics.append(text(QStringLiteral("render.size"), {}, QStringLiteral("Render size"), sizeText({e.renderW, e.renderH})));
        r.metrics.append(number(QStringLiteral("render.scale"), {}, QStringLiteral("Display scale"), double(scalePercent(e.scale)), QStringLiteral("%")));
        QString window = e.windowW > 0 ? sizeText({e.windowW, e.windowH}) : QStringLiteral("headless");
        if (e.fit)
            window += QStringLiteral(" (scaled to fit the screen)");
        r.metrics.append(text(QStringLiteral("window.size"), {}, QStringLiteral("Window"), window));
        if (!e.screen.isEmpty())
            r.metrics.append(text(QStringLiteral("screen"), {}, QStringLiteral("Screen"), e.screen));
        break;
    }

    // Per API, per scene: the numbers (§10).
    bool anyMeasured = false, apiError = false;
    for (const ApiRun &a : m_apis) {
        apiError = apiError || a.errored;
        QVector<double> fpsAll, lowAll;
        for (const SceneId scene : kScenes) {
            const auto it = a.results.find(scene);
            if (it == a.results.end())
                continue;
            const pr::ResultEvent &res = it->second;
            anyMeasured = true;
            const QString what = QStringLiteral("%1 on %2").arg(sceneName(scene), apiShort(a.id));
            const double low = res.ftP99 > 0 ? 1000.0 / res.ftP99 : 0.0;
            r.metrics.append(number(sceneApiKey(a.id, scene, u"fps"), scenes, what + QStringLiteral(": average"), res.fps, QStringLiteral("fps"), Direction::HigherIsBetter));
            r.metrics.append(number(sceneApiKey(a.id, scene, u"low"), scenes, what + QStringLiteral(": 1% low"), low, QStringLiteral("fps"), Direction::HigherIsBetter));
            r.metrics.append(number(sceneApiKey(a.id, scene, u"ft.p1"), scenes, what + QStringLiteral(": frame time p1"), res.ftP1, QStringLiteral("ms")));
            r.metrics.append(number(sceneApiKey(a.id, scene, u"ft.p50"), scenes, what + QStringLiteral(": frame time p50"), res.ftP50, QStringLiteral("ms")));
            r.metrics.append(number(sceneApiKey(a.id, scene, u"ft.p99"), scenes, what + QStringLiteral(": frame time p99"), res.ftP99, QStringLiteral("ms")));
            if (res.gpuMs)
                r.metrics.append(number(sceneApiKey(a.id, scene, u"gpu"), scenes, what + QStringLiteral(": GPU time"), *res.gpuMs, QStringLiteral("ms")));
            r.metrics.append(number(sceneApiKey(a.id, scene, u"cpu"), scenes, what + QStringLiteral(": CPU time"), res.cpuMs, QStringLiteral("ms")));
            r.metrics.append(number(sceneApiKey(a.id, scene, u"frames"), scenes, what + QStringLiteral(": frames"), double(res.frames), QString()));
            if (const auto mm = m_mismatch.find({a.id, scene}); mm != m_mismatch.end())
                r.metrics.append(text(sceneApiKey(a.id, scene, u"match"), scenes, what + QStringLiteral(": image"), mm->second));
            if (!res.series.isEmpty()) {
                Series s{seriesKey(apiKey(a.id), sceneKey(scene)), what + QStringLiteral(": frame time"), QStringLiteral("Time"), QStringLiteral("s"), QStringLiteral("ms"), {}};
                s.points = res.series;
                r.series.append(s);
            }
            if (m_mismatch.count({a.id, scene}))
                continue;  // not counted in the API's own mean either
            fpsAll.append(res.fps);
            lowAll.append(low);
        }
        // The stub keys kept (P1): the geometric mean over the 4 scenes, only when all 4 are there.
        if (fpsAll.size() == int(kScenes.size())) {
            r.metrics.append(number(apiSummaryKey(a.id, u"fps"), scenes, apiShort(a.id) + QStringLiteral(": average over the scenes"),
                                    geometricMean({fpsAll.begin(), fpsAll.end()}), QStringLiteral("fps"), Direction::HigherIsBetter));
            r.metrics.append(number(apiSummaryKey(a.id, u"low"), scenes, apiShort(a.id) + QStringLiteral(": 1% low over the scenes"),
                                    geometricMean({lowAll.begin(), lowAll.end()}), QStringLiteral("fps"), Direction::HigherIsBetter));
        }
    }
    // The best API's number per scene (D5), shown bold in the tables.
    bool everyScene = true;
    for (const SceneId scene : kScenes) {
        QVector<Candidate> candidates;
        for (const ApiRun &a : m_apis) {
            const auto it = a.results.find(scene);
            // An API that errored in this scene, or drew it differently from the reference, never
            // gives the headline number (M2).
            if (it != a.results.end() && !m_mismatch.count({a.id, scene}) && !(a.errored && a.errorSub == kScenes3dId && a.errorScene == sceneName(scene)))
                candidates.append({apiKey(a.id), it->second.fps});
        }
        const auto best = bestOf(candidates);
        if (!best) {
            everyScene = false;
            continue;
        }
        const ApiId bestApi = *apiFromKey(best->source);
        const pr::ResultEvent &res = m_apis[std::size_t(bestApi)].results.at(scene);
        const QString what = sceneName(scene);
        r.metrics.append(number(sceneBestKey(scene, u"fps"), scenes, what + QStringLiteral(": best API, average"), best->value, QStringLiteral("fps"), Direction::HigherIsBetter));
        r.metrics.append(text(sceneBestKey(scene, u"api"), scenes, what + QStringLiteral(": best API"), apiName(bestApi)));
        r.metrics.append(number(sceneBestKey(scene, u"low"), scenes, what + QStringLiteral(": best API, 1% low"),
                                res.ftP99 > 0 ? 1000.0 / res.ftP99 : 0.0, QStringLiteral("fps"), Direction::HigherIsBetter));
    }

    // Fill-rate (§6.5, §6.6, §10): per API `fill2d.<api>` (Gpx/s), `fill2d.<api>.sprites` (MOps/s), `.quads.fps` / `.sprites.fps`, the
    // `ft.<api>.quads|sprites` series, `.match` for a signature outlier; then the best API's `fill2d.quads|sprites` (+ `.api`), which
    // is what the score reads (D4). The rates use the units of ids.h (divided by a dev hook's --content-scale, L3) and the
    // render size the worker reported.
    const QString fill = kFill2dId;
    bool everyFill = true;
    const double quadLayers = fillCount(FillId::Quads), sprites = fillCount(FillId::Sprites);
    const auto fillRate = [&](const ApiRun &a, FillId f, const pr::ResultEvent &res) {
        if (res.seconds <= 0)
            return 0.0;
        if (f == FillId::Sprites)
            return sprites * double(res.frames) / res.seconds / 1e6;
        const int w = res.renderW > 0 ? res.renderW : (a.ready ? a.ready->renderW : 0);
        const int h = res.renderH > 0 ? res.renderH : (a.ready ? a.ready->renderH : 0);
        return quadLayers * double(w) * double(h) * double(res.frames) / res.seconds / 1e9;
    };
    const auto fillUnit = [](FillId f) { return f == FillId::Quads ? QStringLiteral("Gpx/s") : QStringLiteral("MOps/s"); };
    const auto fillBad = [&](const ApiRun &a, FillId f) {
        return m_fillMismatch.count({a.id, f}) || (a.errored && a.errorSub == kFill2dId && a.errorScene == fillName(f));
    };
    for (const ApiRun &a : m_apis) {
        for (const FillId f : kFills) {
            const auto it = a.fills.find(f);
            if (it == a.fills.end())
                continue;
            const pr::ResultEvent &res = it->second;
            anyMeasured = true;
            const QString what = QStringLiteral("%1 on %2").arg(fillName(f), apiShort(a.id));
            if (const double rate = fillRate(a, f, res); rate > 0)
                r.metrics.append(number(fillApiKey(a.id, f), fill, what, rate, fillUnit(f), Direction::HigherIsBetter));
            r.metrics.append(number(fillApiFpsKey(a.id, f), fill, what + QStringLiteral(": average"), res.fps, QStringLiteral("fps"), Direction::HigherIsBetter));
            if (const auto mm = m_fillMismatch.find({a.id, f}); mm != m_fillMismatch.end())
                r.metrics.append(text(fillApiMatchKey(a.id, f), fill, what + QStringLiteral(": image"), mm->second));
            if (!res.series.isEmpty()) {
                Series s{seriesKey(apiKey(a.id), fillKey(f)), what + QStringLiteral(": frame time"), QStringLiteral("Time"), QStringLiteral("s"), QStringLiteral("ms"), {}};
                s.points = res.series;
                r.series.append(s);
            }
        }
    }
    for (const FillId f : kFills) {
        QVector<Candidate> candidates;
        for (const ApiRun &a : m_apis) {
            const auto it = a.fills.find(f);
            // As for the scenes: an API that errored here or drew it differently never gives the headline number.
            if (it != a.fills.end() && !fillBad(a, f))
                candidates.append({apiKey(a.id), fillRate(a, f, it->second)});
        }
        const auto best = bestOf(candidates);
        if (!best) {
            everyFill = false;
            continue;
        }
        r.metrics.append(number(fillBestKey(f), fill, fillName(f), best->value, fillUnit(f), Direction::HigherIsBetter));
        r.metrics.append(text(fillBestKey(f, true), fill, fillName(f) + QStringLiteral(": best API"), apiName(*apiFromKey(best->source))));
    }

    // The native 2D paths (§10): api.<path>.status for both (N/A ones included), the version / driver / present of the
    // ones that ran, the notes (D23 a warnings), `native2d.<path>.<w>` MOps/s per path and `native2d.<w>` = Qt raster's
    // (the scored number, D8). Direct2D is shown, not scored.
    const QString native = kNative2dId;
    for (const NativeRun &n : m_natives) {
        const QString key = nativePathKey(n.path), name = nativePathName(n.path);
        r.metrics.append(text(apiStatusKey(key, u"status"), {}, name + QStringLiteral(" status"), n.status.isEmpty() ? (stopped && n.available && n.results.size() < std::size_t(kNativeWorkloads.size()) ? QStringLiteral("Stopped") : QStringLiteral("OK")) : n.status));
        if (n.ready) {
            r.metrics.append(text(apiStatusKey(key, u"version"), {}, name + QStringLiteral(" version"), n.ready->version));
            r.metrics.append(text(apiStatusKey(key, u"driver"), {}, name + QStringLiteral(" driver"), n.ready->driver));
            if (!n.present.isEmpty())
                r.metrics.append(text(apiStatusKey(key, u"present"), {}, name + QStringLiteral(" present mode"), n.present));
        }
        QStringList notes = n.notes;
        if (n.ready && !n.ready->note.isEmpty())
            notes.prepend(n.ready->note);
        if (!notes.isEmpty())
            r.metrics.append(text(apiStatusKey(key, u"note"), {}, name + QStringLiteral(" note"), notes.join(QStringLiteral(" · "))));
        for (const NativeWorkload w : kNativeWorkloads) {
            const auto it = n.results.find(w);
            if (it == n.results.end() || it->second.seconds <= 0)
                continue;
            const double mops = double(nativeOpsPerFrame(w)) * double(it->second.frames) / it->second.seconds / 1e6;  // unscaled ops (§7.1)
            r.metrics.append(number(nativePathMetricKey(n.path, w), native, QStringLiteral("%1 on %2").arg(nativeName(w), name), mops,
                                    QStringLiteral("MOps/s"), Direction::HigherIsBetter));
            if (n.path == NativePath::QtRaster)
                r.metrics.append(number(nativeBestKey(w), native, nativeName(w), mops, QStringLiteral("MOps/s"), Direction::HigherIsBetter));
        }
    }

    // Sub-results (§12): scenes3d and fill2d are an Error when any API errored in one of their workloads (the first such error,
    // in API order) or a workload has no Ok API; native2d is Ok when Qt raster finished all four workloads.
    SubResult s3d{kScenes3dId, Status::Ok, {}, std::nullopt};
    SubResult sFill{kFill2dId, Status::Ok, {}, std::nullopt};
    QString firstReason, firstFillReason;
    for (const ApiRun &a : m_apis) {
        if (!a.errored)
            continue;
        const QString why = QStringLiteral("%1 failed in %2: %3").arg(apiName(a.id), a.errorScene, a.errorReason);
        QString &slot = a.errorSub == kFill2dId ? firstFillReason : firstReason;
        if (slot.isEmpty())
            slot = why;
    }
    if (!firstReason.isEmpty() || !everyScene) {
        s3d.status = Status::Error;
        s3d.reason = !firstReason.isEmpty() ? firstReason : QStringLiteral("not every scene ran");
    }
    if (!firstFillReason.isEmpty() || !everyFill) {
        sFill.status = Status::Error;
        // L2: no fill failed, but an API that died in a 3D scene was the only one left for them: say so.
        bool anyFill = false;
        for (const ApiRun &a : m_apis)
            anyFill = anyFill || !a.fills.empty();
        sFill.reason = !firstFillReason.isEmpty()            ? firstFillReason
                       : !anyFill && !firstReason.isEmpty() ? QStringLiteral("no API ran the fill workloads (%1)").arg(firstReason)
                                                            : QStringLiteral("not every fill workload ran");
    }
    if (stopped) {
        s3d.status = Status::Cancelled;
        sFill.status = Status::Cancelled;
    }
    SubResult n2d{kNative2dId, Status::Ok, {}, std::nullopt};
    QString nativeReason;  // a Qt raster failure, for the test's status
    {
        const NativeRun &qt = m_natives.back();
        if (stopped) {
            n2d.status = Status::Cancelled;
        } else if (qt.errored) {
            nativeReason = QStringLiteral("Qt raster failed %1: %2")
                               .arg(qt.errorWorkload.isEmpty() ? QStringLiteral("during start-up") : QStringLiteral("in ") + qt.errorWorkload, qt.errorReason);
            n2d.status = Status::Error;
            n2d.reason = nativeReason;
        } else if (!qt.available) {
            n2d.status = Status::NotAvailable;  // Qt raster itself said N/A (for instance a screen too small for the window)
            n2d.reason = qt.status.startsWith(QLatin1StringView("N/A: ")) ? qt.status.mid(5) : qt.status;
        } else if (qt.results.size() != std::size_t(kNativeWorkloads.size())) {
            n2d.status = Status::Error;
            n2d.reason = QStringLiteral("not every native 2D workload ran");
        }
    }
    r.subResults = {s3d, n2d, sFill};

    // The test's status (§12, D6): all three Ok -> Ok; otherwise Partial with "No GPU score: ..." when at least one part is Ok;
    // none Ok -> Error. The reason is the first API failure (3D before fill), then Qt raster's, then the generic text.
    const QString apiFailure = !firstReason.isEmpty() ? firstReason : firstFillReason;
    const int okParts = int(s3d.status == Status::Ok) + int(n2d.status == Status::Ok) + int(sFill.status == Status::Ok);
    if (stopped) {
        r.status = Status::Cancelled;
        r.statusReason = defaultStoppedReason();
    } else if (!anyMeasured && !apiError) {
        // Every API said "not available" when it started (no driver, FIFO-only): no 3D API ran.
        r.status = Status::NotAvailable;
        r.statusReason = naReason(m_apis);
    } else if (!anyMeasured) {
        r.status = Status::Error;
        r.statusReason = !firstReason.isEmpty() ? firstReason : QStringLiteral("no 3D API produced a result");
    } else if (okParts == 3) {
        r.status = Status::Ok;
    } else if (okParts == 0) {
        r.status = Status::Error;
        r.statusReason = !apiFailure.isEmpty() ? apiFailure : !nativeReason.isEmpty() ? nativeReason : QStringLiteral("not every part ran");
    } else {
        r.status = Status::Partial;
        r.statusReason = QStringLiteral("No GPU score: ")
                         + (!apiFailure.isEmpty() ? apiFailure : !nativeReason.isEmpty() ? nativeReason : QStringLiteral("not every part ran"));
    }

    // score.3d (M4, P21): the 3D sub-score, when its group is complete and Ok.
    TestResult probe = r;
    const ScoreBreakdown b = gpu::formula_v1::score(probe);
    if (const auto it = b.subScores.constFind(kScenes3dId); it != b.subScores.cend())
        r.metrics.append(number(QStringLiteral("score.3d"), {}, QStringLiteral("3D score"), *it, QStringLiteral("points"), Direction::HigherIsBetter));
    // score.2d (M4, P21): round(1000 x sqrt(G_native x G_fill)), when both groups are complete and Ok.
    if (const auto s2d = gpu::formula_v1::score2d(probe))
        r.metrics.append(number(QStringLiteral("score.2d"), {}, QStringLiteral("2D score"), *s2d, QStringLiteral("points"), Direction::HigherIsBetter));
    return r;
}

} // namespace

GpuTest::GpuTest()
{
    m_info.id = QStringLiteral("gpu");
    m_info.name = QStringLiteral("GPU");
    m_info.summary = QStringLiteral("3D, 2D and fill-rate on 4 graphics APIs");  // M7
    m_info.colorToken = QStringLiteral("t-gpu");
    m_info.testVersion = 0;     // provisional anchors (task 36 freezes them)
    m_info.formulaVersion = 0;
    // D13: every run runs all three; none is selectable.
    m_info.subTests = {{kScenes3dId, QStringLiteral("3D scenes"), false, true},
                       {kNative2dId, QStringLiteral("Native 2D"), false, true},
                       {kFill2dId, QStringLiteral("2D fill-rate"), false, true}};
}

int GpuTest::estimateSeconds(int apis3d, int nativePaths)
{
    const int raw = 16 + 124 * apis3d + 46 * nativePaths;
    return (raw + 9) / 10 * 10;
}

QVector<pr::PlanItem> GpuTest::planFill()
{
    return {{QStringLiteral("quads"), 2000, 10000},
            {QStringLiteral("sprites"), 2000, 10000}};
}

QVector<pr::PlanItem> GpuTest::plan3d()
{
    return {{QStringLiteral("geometry"), 5000, 20000},
            {QStringLiteral("drawcalls"), 3000, 20000},
            {QStringLiteral("shading"), 3000, 20000},
            {QStringLiteral("particles"), 3000, 20000}};
}

QVector<GpuEntry> GpuTest::gpus() const
{
    QMutexLocker lock(&m_mutex);
    return m_gpus;
}

Availability GpuTest::probe(const ProbeContext &) const
{
    if (GpuClient::workerPath().isEmpty())
        return Availability::unavailable(QStringLiteral("The GPU worker is missing from this installation"));
    QString error;
    bool timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;  // D22: 3 s in all
    QVector<GpuEntry> found = GpuClient::probe(3s, &error, &timedOut);
    Availability a;
    if (found.isEmpty() && !error.isEmpty()) {
        if (timedOut) {
            // A cold driver load: say so, and look again when the test starts.
            a.capabilities.append({QStringLiteral("GPU details: checked when the test starts"), Tone::Warn});
            return a;
        }
        return Availability::unavailable(error);
    }
    if (!found.isEmpty())
        probeOpenGl(found, deadline);
    {
        QMutexLocker lock(&m_mutex);
        m_gpus = found;
    }
    const std::optional<int> def = chooseDefault(found);  // the worker lists software ones only with its dev hook (P12)
    if (!def)
        return Availability::unavailable(QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found"));
    const GpuEntry &g = found[*def];
    bool any = false;
    for (const char *key : kApiKeys) {
        const ApiAvailability av = g.apis.value(QString::fromLatin1(key));
        if (av.ok) {
            any = true;
            a.capabilities.append({av.version.isEmpty() ? QString::fromLatin1(key) : av.version, Tone::Good});
        }
    }
    if (!any)
        return Availability::unavailable(QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found"));
    // L4 (the approved mockup): a missing-component hint (Windows on ARM's compatibility pack) is the
    // probe hint of Run tests, not of the Detail page.
    for (const char *key : kApiKeys) {
        const ApiAvailability av = g.apis.value(QString::fromLatin1(key));
        if (!av.ok && !av.hint.isEmpty()) {
            a.hint = Hint{QStringLiteral("Get the compatibility pack for OpenGL"), av.hint};
            break;
        }
    }
    if (g.vramBytes > 0 && g.vramBytes < (qint64(512) << 20))
        a.capabilities.append({QStringLiteral("Little video memory (%1)").arg(vramText(g.vramBytes)), Tone::Warn});
    return a;
}

OptionSchema GpuTest::options(const ProbeContext &ctx, const Availability &) const
{
    const QVector<GpuEntry> list = gpus();
    OptionSchema o;
    OptionDef gpu;
    gpu.key = QStringLiteral("gpu");
    gpu.label = QStringLiteral("GPU");
    gpu.kind = OptionKind::DynamicSelect;
    for (const GpuEntry &g : list) {
        QStringList apis;
        for (const char *key : kApiKeys) {
            const ApiAvailability av = g.apis.value(QString::fromLatin1(key));
            if (av.ok)
                apis << apiName(*apiFromKey(QString::fromLatin1(key)));
        }
        OptionItem item;
        item.value = g.id;
        item.label = g.name;
        item.detail = QStringLiteral("%1 · %2 · %3").arg(kindName(g.kind), vramText(g.vramBytes), apis.isEmpty() ? QStringLiteral("no 3D API") : apis.join(QStringLiteral(", ")));
        item.enabled = !apis.isEmpty();
        if (!item.enabled)
            item.disabledReason = QStringLiteral("no 3D API");
        gpu.items.append(item);
    }
    if (const auto def = chooseDefault(list))
        gpu.defaultValue = list[*def].id;
    if (!gpu.items.isEmpty())
        o.options.append(gpu);
    const auto info = [&](const char *key, const char *label, const QString &value, Tone tone = Tone::Neutral) {
        OptionDef d;
        d.key = QLatin1StringView(key);
        d.label = QLatin1StringView(label);
        d.kind = OptionKind::Info;
        d.infoValue = value;
        d.infoTone = tone;
        o.options.append(d);
    };
    // The APIs of a GPU: versions and N/A reasons. The row follows the GPU select (DEF-3): its text for
    // every GPU is in infoByValue, and the default GPU's is the fallback.
    const auto apisText = [](const GpuEntry &g) {
        QStringList ok, na;
        for (const char *key : kApiKeys) {
            const ApiAvailability av = g.apis.value(QString::fromLatin1(key));
            const QString name = apiName(*apiFromKey(QString::fromLatin1(key)));
            if (av.ok)
                ok << (av.version.isEmpty() ? name : av.version);
            else if (!av.reason.isEmpty())
                na << (av.hint.isEmpty() ? QStringLiteral("%1: %2").arg(name, av.reason) : QStringLiteral("%1: %2 (%3)").arg(name, av.reason, av.hint));
        }
        return (ok.isEmpty() ? QStringLiteral("none found") : ok.join(QStringLiteral(", "))) + (na.isEmpty() ? QString() : QStringLiteral(" · N/A: ") + na.join(QStringLiteral("; ")));
    };
    {
        OptionDef d;
        d.key = QStringLiteral("apis");
        d.label = QStringLiteral("APIs");
        d.kind = OptionKind::Info;
        const auto def = chooseDefault(list);
        d.infoValue = def ? apisText(list[*def]) : QStringLiteral("none found");
        if (!list.isEmpty()) {
            d.infoFollows = QStringLiteral("gpu");
            for (const GpuEntry &g : list)
                d.infoByValue.insert(g.id, apisText(g));
        }
        o.options.append(d);
    }
    // M6: what the window will be on this screen (the snapshot's primary screen).
    const DisplayInfo display = displayFromSnapshotData(ctx.system().data);
    if (!display.known()) {
        info("window", "Window", QStringLiteral("1920×1080 logical at the screen's scale · vsync off"));
    } else {
        const WindowPlan plan = planWindow({display.availWidth, display.availHeight}, display.scale);
        const QString head = QStringLiteral("1920×1080 logical · vsync off\n");
        if (plan.tooSmall)
            info("window", "Window", head + QStringLiteral("here: the screen is too small for the test window"), Tone::Warn);
        else if (plan.fit)
            info("window", "Window", head + QStringLiteral("here: %1 px, window scaled to fit").arg(sizeText(plan.render)), Tone::Warn);
        else
            info("window", "Window", head + QStringLiteral("here: %1 px, fits").arg(sizeText(plan.render)));
    }
    const int secs = int(estimate({}).count());
    info("runs", "Runs", QStringLiteral("Every API and part runs (about %1 min %2 s)").arg(secs / 60).arg(secs % 60));
    return o;
}

std::chrono::seconds GpuTest::estimate(const OptionValues &values) const
{
    const QVector<GpuEntry> list = gpus();
    int apis = kDefaultApis;
    if (!list.isEmpty()) {
        const int at = findGpu(list, values.value(QStringLiteral("gpu")).toString());
        const auto def = at >= 0 ? std::optional<int>(at) : chooseDefault(list);
        if (def) {
            apis = 0;
            for (const char *key : kApiKeys)
                apis += list[*def].apis.value(QString::fromLatin1(key)).ok ? 1 : 0;
            apis = std::max(apis, 1);
        }
    }
    return std::chrono::seconds(estimateSeconds(apis, kNativePaths));
}

std::chrono::seconds GpuTest::timeLimit(const OptionValues &o) const
{
    // 2 x the estimate + 60 s, at the developer time scale for the windows (the
    // starts and switches don't shrink with it): never under the floor below real time.
    const double scale = stubTimeScale();
    const double secs = 2.0 * double(estimate(o).count()) * scale + 60.0;
    const long long floor = scale < 1.0 ? stubTimeLimitFloor().count() : 0;
    return std::chrono::seconds(std::max<long long>(floor, std::llround(secs)));
}

TestResult GpuTest::run(RunContext &ctx, const OptionValues &options)
{
    QVector<GpuEntry> list = gpus();
    if (list.isEmpty()) {
        QString error;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        list = GpuClient::probe(10s, &error);  // the start-up probe ran out of time (D22)
        if (list.isEmpty())
            return TestResult::notAvailable(error.isEmpty() ? QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found") : error);
        probeOpenGl(list, deadline);
        QMutexLocker lock(&m_mutex);
        m_gpus = list;
    }
    int at = findGpu(list, options.value(QStringLiteral("gpu")).toString());
    if (at < 0) {
        const auto def = chooseDefault(list);
        if (!def)
            return TestResult::notAvailable(QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found"));
        at = *def;  // an unknown id falls back to the default (P11)
    }
    GpuRun run(ctx, list[at]);
    return run.run();
}

ScoreBreakdown GpuTest::score(const TestResult &result) const
{
    return gpu::formula_v1::score(result);
}

DetailLayout GpuTest::detail(const TestResult &result) const
{
    return genericDetailLayout(result);  // the real page comes with task 34
}

} // namespace sb::tests

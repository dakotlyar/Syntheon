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
#include "tests/gpu/model/ids.h"
#include "sysinfo/system_info.h"
#include "tests/gpu/model/render_size.h"

#include <QMutexLocker>

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

// The APIs this build can drive (task 15: Vulkan; tasks 18, 20 and 21 add the others).
bool builtIn(ApiId id)
{
    return id == ApiId::Vulkan;
}

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

// One API's state through a run.
struct ApiRun {
    ApiId id;
    bool active = false;       // a worker is running for it
    bool errored = false;
    QString status;            // "OK", "N/A: ...", "Error: ..."
    QString errorScene;        // first Error: the scene (name) and reason
    QString errorReason;
    std::optional<pr::ReadyEvent> ready;
    QString present;
    std::unique_ptr<GpuSession> session;
    std::map<SceneId, pr::ResultEvent> results;
};

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
    void fail(ApiRun &a, const QString &sceneName, const QString &reason)
    {
        a.errored = true;
        a.active = false;
        a.status = QStringLiteral("Error: ") + reason;
        if (a.errorScene.isEmpty()) {
            a.errorScene = sceneName;
            a.errorReason = reason;
        }
        if (a.session)
            a.session->kill();  // its remaining workloads are skipped
    }
    void compareSignatures(int item, const QVector<pr::PlanItem> &plan);
    TestResult finish(bool stopped);

    RunContext &m_ctx;
    const GpuEntry m_gpu;
    std::vector<ApiRun> m_apis;
    TestResult m_r;
    double m_total = 1, m_last = 0;
    std::map<std::pair<ApiId, SceneId>, QString> m_mismatch;  // api, scene -> "drew X differently from Y (mean 9.3/255)"
};

void GpuRun::compareSignatures(int item, const QVector<pr::PlanItem> &plan)
{
    const SceneId scene = *sceneFromKey(plan[item].workload);
    struct Entry {
        ApiRun *api;
        QByteArray sig;
    };
    std::vector<Entry> have;
    for (ApiRun &a : m_apis) {
        const auto it = a.results.find(scene);
        if (a.active && it != a.results.end() && it->second.signature.size() == 432)
            have.push_back({&a, it->second.signature});
    }
    if (have.size() < 2)
        return;  // a single API: no comparison
    // §5.7: the reference is the API that matches the most others (ties: the earlier API).
    std::vector<int> matches(have.size(), 0);
    for (std::size_t i = 0; i < have.size(); ++i) {
        for (std::size_t j = 0; j < have.size(); ++j) {
            if (i == j)
                continue;
            const auto d = gpu::compareSignatures(have[i].sig, have[j].sig);
            if (d && signaturesMatch(*d))
                ++matches[i];
        }
    }
    std::size_t ref = 0;
    for (std::size_t i = 1; i < have.size(); ++i) {
        if (matches[i] > matches[ref])
            ref = i;
    }
    for (std::size_t i = 0; i < have.size(); ++i) {
        if (i == ref)
            continue;
        const auto d = gpu::compareSignatures(have[i].sig, have[ref].sig);
        if (d && signaturesMatch(*d))
            continue;
        const QString why = QStringLiteral("drew %1 differently from %2 (mean %3/255)")
                                .arg(sceneName(scene), apiName(have[ref].api->id))
                                .arg(d ? d->mean : 0.0, 0, 'f', 1);
        m_mismatch[{have[i].api->id, scene}] = why;
        m_ctx.log(LogLevel::Error, QStringLiteral("%1: %2").arg(apiName(have[i].api->id), why));
        fail(*have[i].api, sceneName(scene), why);
    }
}

TestResult GpuRun::run()
{
    const QVector<pr::PlanItem> basePlan = GpuTest::plan3d();
    QVector<pr::PlanItem> plan;
    for (const pr::PlanItem &p : basePlan)
        plan.append({p.workload, int(scaled(p.warmupMs).count()), int(scaled(p.measureMs).count())});

    // The APIs that run, and why each of the others doesn't (§4.2).
    for (const ApiId id : kApis) {
        ApiRun a;
        a.id = id;
        const ApiAvailability av = m_gpu.apis.value(apiKey(id));
        if (fakeApi(id) || (av.ok && builtIn(id))) {
            a.active = true;
        } else if (!av.reason.isEmpty() && !av.ok) {
            a.status = QStringLiteral("N/A: ") + av.reason;
        } else {
            a.status = QStringLiteral("N/A: not built yet (Phase 4a-2)");
        }
        m_apis.push_back(std::move(a));
    }
    int activeCount = 0;
    for (const ApiRun &a : m_apis)
        activeCount += a.active ? 1 : 0;
    if (activeCount == 0) {
        TestResult r = TestResult::notAvailable(QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found"));
        for (const ApiRun &a : m_apis)
            r.metrics.append(text(apiStatusKey(apiKey(a.id), u"status"), {}, apiName(a.id) + QStringLiteral(" status"), a.status));
        return r;
    }

    // The windows' total (the budget), once the APIs are known.
    double perApi = 0;
    for (const pr::PlanItem &p : plan)
        perApi += double(p.warmupMs + p.measureMs) / 1000.0;
    m_total = perApi * activeCount + 10.0;  // + starting the workers and switching windows

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
        if (!a.session->start(apiKey(a.id), m_gpu.id, {}, &err))
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
        const SceneId scene = *sceneFromKey(p.workload);
        const double windows = double(p.warmupMs + p.measureMs) / 1000.0;
        for (ApiRun &a : m_apis) {
            if (!a.active || stopped)
                continue;
            m_ctx.setPart(QStringLiteral("%1 · %2").arg(sceneName(scene), apiName(a.id)));
            const auto stage = [&](const QString &what) {
                return QStringLiteral("%1 on %2 · %3").arg(sceneName(scene), apiShort(a.id), what);
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
                a.results[scene] = o.results.first();
                if (a.present.isEmpty())
                    a.present = o.results.first().present;
                m_ctx.log(LogLevel::Info, QStringLiteral("%1 on %2: %3 fps").arg(sceneName(scene), apiName(a.id)).arg(o.results.first().fps, 0, 'f', 1));
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
                fail(a, sceneName(scene), o.reason);
                m_ctx.log(LogLevel::Error, QStringLiteral("%1 failed in %2: %3").arg(apiName(a.id), sceneName(scene), o.reason));
                break;
            }
            done += windows;
        }
        if (!stopped)
            compareSignatures(item, plan);
        done = double(item + 1) * windows * activeCount + 4.0;  // however many APIs ran
    }
    for (ApiRun &a : m_apis) {
        if (a.session && a.session->alive())
            a.session->quit();
    }
    m_ctx.holdInterface(false);
    return finish(stopped || cancelled());
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
        if (status.isEmpty())
            status = QStringLiteral("OK");
        r.metrics.append(text(apiStatusKey(key, u"status"), {}, name + QStringLiteral(" status"), status));
        if (a.ready) {
            const ApiAvailability av = m_gpu.apis.value(key);
            r.metrics.append(text(apiStatusKey(key, u"version"), {}, name + QStringLiteral(" version"), av.version.isEmpty() ? a.ready->version : av.version));
            r.metrics.append(text(apiStatusKey(key, u"driver"), {}, name + QStringLiteral(" driver"), av.driver.isEmpty() ? a.ready->driver : av.driver));
            if (!a.present.isEmpty())
                r.metrics.append(text(apiStatusKey(key, u"present"), {}, name + QStringLiteral(" present mode"), a.present));
            if (!a.ready->note.isEmpty())
                r.metrics.append(text(apiStatusKey(key, u"note"), {}, name + QStringLiteral(" note"), a.ready->note));
            if (!a.ready->layers.isEmpty())
                r.metrics.append(text(apiStatusKey(key, u"layers"), {}, name + QStringLiteral(" layers"), a.ready->layers));
        }
    }
    // The window: the first API that came up says (they all share the plan).
    for (const ApiRun &a : m_apis) {
        if (!a.ready)
            continue;
        const pr::ReadyEvent &e = *a.ready;
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
            if (it != a.results.end())
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

    // Sub-results (§12): scenes3d is an Error when any API errored or a scene has no API; native2d
    // and fill2d are placeholders until tasks 28 and 32.
    SubResult s3d{kScenes3dId, Status::Ok, {}, std::nullopt};
    QString firstReason;
    for (const ApiRun &a : m_apis) {
        if (a.errored && firstReason.isEmpty())
            firstReason = QStringLiteral("%1 failed in %2: %3").arg(apiName(a.id), a.errorScene, a.errorReason);
    }
    if (!firstReason.isEmpty() || !everyScene) {
        s3d.status = stopped ? Status::Cancelled : Status::Error;
        s3d.reason = !firstReason.isEmpty() ? firstReason : QStringLiteral("not every scene ran");
    }
    if (stopped)
        s3d.status = Status::Cancelled;
    r.subResults = {s3d,
                    SubResult{kNative2dId, Status::NotAvailable, QStringLiteral("not built yet (Phase 4a-3)"), std::nullopt},
                    SubResult{kFill2dId, Status::NotAvailable, QStringLiteral("not built yet (Phase 4a-4)"), std::nullopt}};

    // The test's status (§12, D6).
    if (stopped) {
        r.status = Status::Cancelled;
        r.statusReason = defaultStoppedReason();
    } else if (!anyMeasured && !apiError) {
        // Every API said "not available" when it started (no driver, FIFO-only): no 3D API ran.
        r.status = Status::NotAvailable;
        r.statusReason = QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found");
    } else if (!anyMeasured) {
        r.status = Status::Error;
        r.statusReason = !firstReason.isEmpty() ? firstReason : QStringLiteral("no 3D API produced a result");
    } else if (s3d.status == Status::Ok) {
        r.status = Status::Partial;  // native 2D and fill-rate aren't built yet
        r.statusReason = QStringLiteral("No GPU score: not every part ran");
    } else {
        r.status = Status::Partial;
        r.statusReason = apiError ? QStringLiteral("No GPU score: ") + firstReason : QStringLiteral("No GPU score: not every part ran");
    }

    // score.3d (M4, P21): the 3D sub-score, when its group is complete and Ok.
    TestResult probe = r;
    const ScoreBreakdown b = gpu::formula_v1::score(probe);
    if (const auto it = b.subScores.constFind(kScenes3dId); it != b.subScores.cend())
        r.metrics.append(number(QStringLiteral("score.3d"), {}, QStringLiteral("3D score"), *it, QStringLiteral("points"), Direction::HigherIsBetter));
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
    const QVector<GpuEntry> found = GpuClient::probe(3s, &error);  // D22: 3 s at most
    Availability a;
    if (found.isEmpty() && !error.isEmpty()) {
        if (error.contains(QLatin1StringView("didn't finish"))) {
            // A cold driver load: say so, and look again when the test starts.
            a.capabilities.append({QStringLiteral("GPU details: checked when the test starts"), Tone::Warn});
            return a;
        }
        return Availability::unavailable(error);
    }
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
    // The APIs of the default GPU.
    QStringList ok, na;
    if (const auto def = chooseDefault(list)) {
        for (const char *key : kApiKeys) {
            const ApiAvailability av = list[*def].apis.value(QString::fromLatin1(key));
            const QString name = apiName(*apiFromKey(QString::fromLatin1(key)));
            if (av.ok)
                ok << (av.version.isEmpty() ? name : av.version);
            else if (!av.reason.isEmpty())
                na << QStringLiteral("%1: %2").arg(name, av.reason);
        }
    }
    info("apis", "APIs", ok.isEmpty() ? QStringLiteral("none found") : ok.join(QStringLiteral(", ")) + (na.isEmpty() ? QString() : QStringLiteral(" · N/A: ") + na.join(QStringLiteral("; "))));
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
        list = GpuClient::probe(10s, &error);  // the start-up probe ran out of time (D22)
        if (list.isEmpty())
            return TestResult::notAvailable(error.isEmpty() ? QStringLiteral("No GPU with Vulkan, DirectX or OpenGL found") : error);
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

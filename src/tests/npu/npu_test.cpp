// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU test (Phase 4c design): the ITest surface (§11) and the run loop
// (§5, §7 — the NpuRun class, a RamRun/StorageRun-style steady_clock loop with
// progress monotonicity and Stop polling, no WorkerPool and no core pinning).
// Detection/vendor/runtime §3; the model pack + downloader §6; the workloads §2.
#include "tests/npu/npu_test.h"

#include "core/units.h"
#include "tests/npu/detect/detect.h"
#include "tests/npu/engine/engine.h"
#include "tests/npu/formula_v1.h"
#include "tests/npu/ids.h"
#include "tests/npu/models/downloader.h"
#include "tests/npu/workloads/workloads.h"

#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <numeric>

namespace sb::tests {

namespace {

// §2.2 run budget: 3 workloads × 3 windows × 5 s of measurement = 45 s, plus a
// one-time engine load + model compile/load + a short warm-up. The per-vendor
// constants are conservative upper bounds (QNN graph prepare is the 60 s worst
// case). estimate() has no detected vendor (there's no device picker, §3.4),
// so it reports this single bound.
constexpr int kMeasureSeconds = 3 * 3 * 5;   // 45 s
constexpr int kEngineLoadSeconds = 60;       // QNN graph prepare (§2.2 worst case)
constexpr int kModelLoadSeconds = 20;        // AMD/Vitis compile or QNN context
constexpr int kWarmupSeconds = 5;

// §3.3 / D4 guided-setup and N/A-link URLs (stable enough for now; confirm the
// exact targets with Dave before the first release, like the pack base URL).
const QString kIntelRuntimeUrl = QStringLiteral("https://docs.openvino.ai/latest/get-started.html");
const QString kAmdRyzenAiUrl = QStringLiteral("https://ryzenai.docs.amd.com/");
const QString kQualcommUrl = QStringLiteral("https://aihub.qualcomm.com/");

// The platform name for §3.3's "%1 isn't supported on %2".
QString platformText()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macOS");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Linux");
#else
    return QStringLiteral("this platform");
#endif
}

// Renders a detect() reason for availability / run short-circuits, with the
// right %1/%2/%3 args (§3.3).
QString naText(const sb::npu::NpuDetect &det)
{
    using namespace sb::npu;
    QString s;
    switch (det.reason) {
    case NaReason::NoNpu:
        s = reasonString(NaReason::NoNpu);
        break;
    case NaReason::UnsupportedVendor:
        s = reasonString(NaReason::UnsupportedVendor, {vendorName(det.vendor), platformText()});
        break;
    default:
        s = reasonString(det.reason, {vendorName(det.vendor)});
        break;
    }
    // AMD's missing runtime is "detect + report N/A with a link" (locked), not a
    // guided setup — but the UI doesn't render Availability::hint, so the setup
    // URL must be part of the *visible* reason (the gpu_test.cpp "(%3)" trick).
    // Both probe()'s N/A reason and run()'s Detail NaStateBlock take this path.
    if (det.reason == NaReason::RuntimeMissing && det.vendor == NpuVendor::Amd && !s.isEmpty())
        s += QStringLiteral(" — install the AMD Ryzen AI software: ") + kAmdRyzenAiUrl;
    return s;
}

// The guided-setup URL for the Intel/Qualcomm runtime-precondition hint (D4).
QString guidedSetupUrl(sb::npu::NpuVendor vendor)
{
    using namespace sb::npu;
    if (vendor == NpuVendor::Intel)
        return kIntelRuntimeUrl;
    if (vendor == NpuVendor::Qualcomm)
        return kQualcommUrl;
    return {};
}

#if defined(SYNTHEON_DEV_HOOKS)
// Dev hook (§13, N8): SYNTHEON_NPU_FAKE_VENDOR=null drives the assembled test
// through the null engine on a box with no NPU. The null engine is
// vendor-agnostic, so its models resolve from the manifest's "intel" entries
// (the anchor vendor, always present in a well-formed pack).
bool fakeNullVendor()
{
    return qEnvironmentVariable("SYNTHEON_NPU_FAKE_VENDOR") == QLatin1StringView("null");
}
#else
bool fakeNullVendor() { return false; }
#endif

double cvPercent(const std::array<double, 3> &v)
{
    if (v.size() < 2)
        return 0.0;
    const double mean = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    double sq = 0;
    for (double x : v)
        sq += (x - mean) * (x - mean);
    const double sd = std::sqrt(sq / double(v.size()));
    return mean > 0 ? sd / mean * 100.0 : 0.0;
}

// The headline field of a workload (its scored throughput) and its three raw
// per-window fields (§9).
sb::npu::Field headlineField(sb::npu::Workload w)
{
    return w == sb::npu::Workload::Llm ? sb::npu::Field::Tps : sb::npu::Field::Imgs;
}
std::array<sb::npu::Field, 3> windowFields(sb::npu::Workload w)
{
    if (w == sb::npu::Workload::Llm)
        return {sb::npu::Field::Tps0, sb::npu::Field::Tps1, sb::npu::Field::Tps2};
    return {sb::npu::Field::Img0, sb::npu::Field::Img1, sb::npu::Field::Img2};
}

// The first model of the given workload in the downloaded vendor's list.
const sb::npu::ManifestModel *findModel(const QVector<sb::npu::ManifestModel> &models, sb::npu::Workload w)
{
    const QString id = sb::npu::workloadId(w);
    for (const sb::npu::ManifestModel &m : models)
        if (m.workload == id)
            return &m;
    return nullptr;
}

// One NPU run (the much smaller RamRun/StorageRun analogue, §5): detect →
// download the model pack (a precondition, off the run clock) → load the
// engine → warm up → three workloads → finish. A direct steady_clock loop, no
// WorkerPool / core pinning; progress is monotonic and clamped to 0.999
// (P-PROG); Stop is polled between windows and workloads (P-STOP).
class NpuRun
{
public:
    explicit NpuRun(RunContext &ctx) : m_ctx(ctx), m_budgetTotal(kRunSteps) {}

    TestResult run();

private:
    bool cancelled() const { return m_ctx.cancelled(); }
    std::function<bool()> cancelFn() const { return [this] { return cancelled(); }; }

    // Progress against a fixed step budget, never backwards (P-PROG). The
    // budget is the measured run only: engine load + warm-up + 3 workloads × 3
    // windows (the model-pack download is a precondition, off the run clock).
    static constexpr double kRunSteps = 2.0 + 3.0 * 3.0;
    void report(double at, const QString &stage)
    {
        m_lastFraction = std::clamp(std::max(m_lastFraction, at / m_budgetTotal), 0.0, 0.999);
        m_ctx.progress(m_lastFraction, stage);
    }
    void step(const QString &stage)
    {
        m_done += 1.0;
        report(m_done, stage);
    }

    void metric(const QString &key, const QString &sub, const QString &label, double value, const QString &unit,
                Direction direction = Direction::Neutral, bool headline = false)
    {
        m_result.metrics.append({key, sub, label, value, unit, direction, headline});
    }

    // Detect the primary vendor (or the dev-hook null fake), then map its
    // EngineVendor.
    sb::npu::NpuDetect resolveDetect() const;
    void finish(sb::npu::NpuDetect det, TestResult &r);

    RunContext &m_ctx;

    double m_done = 0;
    double m_budgetTotal = 1;
    double m_lastFraction = 0;
    bool m_stopped = false;
    double m_worstSpread = 0;
    std::map<QString, Status> m_status;
    std::map<QString, QString> m_reason;
    TestResult m_result;

    // Resolved at run time, remembered for finish().
    std::map<sb::npu::Workload, sb::npu::LoadedModel> m_loaded;
    QString m_packVersion;
    QString m_engineBackend;
    QString m_runtimeVersion;
    sb::npu::NpuVendor m_vendor = sb::npu::NpuVendor::Null;
};

sb::npu::NpuDetect NpuRun::resolveDetect() const
{
    if (fakeNullVendor()) {
        sb::npu::NpuDetect det;
        det.vendor = sb::npu::NpuVendor::Null;
        det.deviceName = QStringLiteral("null");
        det.reason = sb::npu::NaReason::None;   // runnable: the null engine needs no runtime
        return det;
    }
    return sb::npu::detect(m_ctx.system());
}

void NpuRun::finish(sb::npu::NpuDetect det, TestResult &r)
{
    const auto str = [&r](const QString &key, const QString &label, const QString &v) {
        r.metrics.append({key, {}, label, v, {}, Direction::Neutral, false});
    };
    str(sb::npu::kDeviceVendor, QStringLiteral("Vendor"), sb::npu::vendorName(det.vendor));
    str(sb::npu::kDeviceName, QStringLiteral("NPU"),
        det.deviceName.isEmpty() ? sb::npu::vendorName(det.vendor) + QStringLiteral(" NPU") : det.deviceName);
    str(sb::npu::kDeviceDriver, QStringLiteral("Driver"), det.driver);
    str(sb::npu::kDeviceRuntime, QStringLiteral("Runtime"), m_runtimeVersion);
    str(sb::npu::kEngineName, QStringLiteral("Inference engine"), m_engineBackend);
    const auto modelText = [&](sb::npu::Workload w) {
        const auto it = m_loaded.find(w);
        return it == m_loaded.end() ? QString() : it->second.name + QStringLiteral(" · ") + it->second.precision;
    };
    str(sb::npu::kModelClassify, QStringLiteral("Model (Classify)"), modelText(sb::npu::Workload::Classify));
    str(sb::npu::kModelDetect, QStringLiteral("Model (Detect)"), modelText(sb::npu::Workload::Detect));
    str(sb::npu::kModelLlm, QStringLiteral("Model (LLM)"), modelText(sb::npu::Workload::Llm));
    str(sb::npu::kModelsPackVersion, QStringLiteral("Model pack"), m_packVersion);
    r.metrics.append({sb::npu::kStabilitySpread, {}, QStringLiteral("Worst window CV"), m_worstSpread, QStringLiteral("%"),
                      Direction::Neutral, false});
    // §3.5: the power source metric + on-battery warning, as CPU/RAM do.
    const QJsonObject battery = m_ctx.system().data.value(QStringLiteral("battery")).toObject();
    const bool onBattery = battery.value(QStringLiteral("present")).toBool() && !battery.value(QStringLiteral("ac_online")).toBool();
    str(sb::npu::kPowerSource, QStringLiteral("Power source"), onBattery ? QStringLiteral("Battery") : QStringLiteral("AC"));
    if (onBattery)
        m_ctx.log(LogLevel::Warning, QStringLiteral("On battery: plug in AC power for repeatable numbers"));
}

TestResult NpuRun::run()
{
    const sb::npu::NpuDetect det = resolveDetect();
    // AMD/Qualcomm have no working engine yet: refuse before the model-pack
    // download rather than fail in init() afterwards (same rule as probe()).
    if (sb::npu::engineNotYetSupported(det.vendor)
        && (det.reason == sb::npu::NaReason::None || det.reason == sb::npu::NaReason::RuntimeMissing))
        return TestResult::notAvailable(sb::npu::engineNotYetSupportedText(det.vendor));
    if (det.reason != sb::npu::NaReason::None)
        return TestResult::notAvailable(naText(det));

    const QString vendorName = sb::npu::vendorName(det.vendor);
    const QString manifestVendor =
        det.vendor == sb::npu::NpuVendor::Null ? QStringLiteral("intel") : sb::npu::vendorId(det.vendor);

    m_ctx.log(LogLevel::Info,
              QStringLiteral("Starting NPU: %1%2").arg(det.vendor == sb::npu::NpuVendor::Null
                                                          ? QStringLiteral("null engine")
                                                          : vendorName)
                  .arg(det.runtimeVersion.isEmpty() ? QString() : QStringLiteral(" runtime ") + det.runtimeVersion));

    // §7 step 2: the model-pack precondition, before (and off) the measured run
    // clock. The base URL and cache root are read LIVE from the context.
    const QString baseUrl = m_ctx.npuModelsBaseUrl();
    const QString cacheRoot = m_ctx.npuCacheRoot();
    // A missing base URL or cache root means the pack can't be downloaded; an
    // empty cache root would make the downloader's QDir("") write to the process
    // CWD (P-HOME leak), so refuse both up front.
    if (baseUrl.isEmpty() || cacheRoot.trimmed().isEmpty())
        return TestResult::notAvailable(sb::npu::reasonString(sb::npu::NaReason::ModelMissing, {vendorName}));

    report(0.0, QStringLiteral("Downloading the model pack"));
    // The calibration manifest pin (§6.1): production always pins the calibrated
    // pack; a context that supplies its own (tests) overrides it so the check
    // still runs — never silently off.
    const QString manifestPin = m_ctx.npuManifestPin().isEmpty()
        ? sb::npu::calibratedManifestSha256()
        : m_ctx.npuManifestPin();
    sb::npu::NpuDownloader downloader(baseUrl, cacheRoot, manifestPin);
    const sb::npu::DownloadResult dl = downloader.download(manifestVendor, cancelFn());
    if (!dl.ok) {
        if (cancelled())
            return TestResult::withStatus(Status::Cancelled, defaultStoppedReason());
        m_ctx.log(LogLevel::Error, dl.error);
        // A tampered/offline pack fails closed (P-NET): model_missing, never a
        // run against an unverified model.
        return TestResult::notAvailable(sb::npu::reasonString(sb::npu::NaReason::ModelMissing, {vendorName}));
    }
    m_packVersion = dl.manifest->pack_version;

    // §7 step 3: resolve the three models, then build + load the engine.
    for (sb::npu::Workload w : sb::npu::kWorkloads) {
        const sb::npu::ManifestModel *mm = findModel(dl.models, w);
        if (!mm)
            return TestResult::notAvailable(sb::npu::reasonString(sb::npu::NaReason::ModelMissing, {vendorName}));
        sb::npu::LoadedModel lm;
        lm.name = mm->id;
        lm.precision = mm->precision;
        // `path` is the primary .xml/.bin for classify/detect, taken from the
        // FIRST manifest file. The fragile files.first() ORDERING is retired
        // for the OpenVINO engine (4c-5): a stateful LLM is multi-file
        // (openvino_model.xml/.bin + tokenizer/detokenizer), so the engine finds
        // its decoder by name inside `dir` — the model's cache directory — not
        // via path/first(). `dir` is set for every workload; classify/detect
        // still use `path` directly.
        lm.path = mm->files.isEmpty() ? QString() : downloader.localPath(*dl.manifest, mm->files.first());
        lm.dir = mm->files.isEmpty() ? QString() : QFileInfo(downloader.localPath(*dl.manifest, mm->files.first())).absolutePath();
        m_loaded[w] = lm;
    }

    QString engineError;
    std::unique_ptr<sb::npu::INpuEngine> engine;
    if (det.vendor == sb::npu::NpuVendor::Null) {
        engine = sb::npu::nullEngine();
    } else {
        static const std::map<sb::npu::NpuVendor, sb::npu::EngineVendor> kEngineVendor = {
            {sb::npu::NpuVendor::Intel, sb::npu::EngineVendor::Intel},
            {sb::npu::NpuVendor::Amd, sb::npu::EngineVendor::Amd},
            {sb::npu::NpuVendor::Qualcomm, sb::npu::EngineVendor::Qualcomm},
        };
        engine = sb::npu::makeEngine(kEngineVendor.at(det.vendor), det.runtimePath, &engineError);
    }
    if (!engine)
        return TestResult::notAvailable(
            sb::npu::reasonString(sb::npu::NaReason::RuntimeLoadFailed, {vendorName, engineError}));

    // Load the engine (a no-op for the null engine; the real engines dlopen the
    // runtime and compile the classify model here). Any failure maps to the
    // RuntimeLoadFailed taxonomy reason. The engine runs one model at a time;
    // the real-vendor engines re-load per workload in 4c-5 validation.
    step(QStringLiteral("Loading the engine"));
    if (!engine->init(m_loaded[sb::npu::Workload::Classify], &engineError)) {
        return TestResult::notAvailable(
            sb::npu::reasonString(sb::npu::NaReason::RuntimeLoadFailed, {vendorName, engineError}));
    }
    m_engineBackend = engine->backendName();
    // The runtime version comes from the engine (which dlopened the runtime),
    // not detect — detect() doesn't dlopen, so det.runtimeVersion is "" and the
    // Device/runtime row would otherwise always read "—". This first read covers
    // the vision-only case; a re-read after the workload loop (below) appends
    // the lazily-loaded GenAI marker when the LLM ran.
    m_runtimeVersion = engine->runtimeVersion();

    // Warm-up window: one discarded window so the first measured window doesn't
    // include the engine's own ramp (design §2.2).
    step(QStringLiteral("Warming up"));
    {
        sb::npu::Inference warm;
        QString err;
        if (!engine->runWindow(sb::npu::workloadId(sb::npu::Workload::Classify),
                               m_loaded[sb::npu::Workload::Classify], warm, &err, cancelFn())) {
            return TestResult::notAvailable(
                sb::npu::reasonString(sb::npu::NaReason::RuntimeLoadFailed, {vendorName, err}));
        }
    }
    if (cancelled())
        return TestResult::withStatus(Status::Cancelled, defaultStoppedReason());

    // §7 step 4: the three workloads, in canonical order.
    for (sb::npu::Workload w : sb::npu::kWorkloads) {
        const QString id = sb::npu::workloadId(w);
        m_status[id] = Status::Ok;
        m_ctx.subTestBegin(id);
        m_ctx.setPart(sb::npu::workloadLabel(w));

        const sb::npu::LoadedModel &model = m_loaded[w];
        const sb::npu::workloads::WorkloadResult wr =
            sb::npu::workloads::runWindows(id, *engine, model, cancelFn(), [this, w](int window) {
                step(QStringLiteral("%1 · window %2 of 3").arg(sb::npu::workloadLabel(w)).arg(window + 1));
            });

        if (wr.status == sb::npu::workloads::WorkloadStatus::Cancelled) {
            m_stopped = true;
            m_status[id] = Status::Cancelled;
        } else if (wr.status == sb::npu::workloads::WorkloadStatus::Error) {
            m_status[id] = Status::Error;
            m_reason[id] = wr.error;
            m_ctx.log(LogLevel::Error, wr.error);
        } else {
            const sb::npu::Field hf = headlineField(w);
            metric(sb::npu::key(w, hf), id, sb::npu::label(w, hf), wr.median, sb::npu::unit(w, hf),
                   Direction::HigherIsBetter, true);
            const auto wf = windowFields(w);
            for (int i = 0; i < 3; ++i)
                metric(sb::npu::key(w, wf[i]), id, sb::npu::label(w, wf[i]), wr.windows[i], sb::npu::unit(w, wf[i]),
                       Direction::Neutral);
            if (w == sb::npu::Workload::Llm && wr.ttftMs > 0)
                metric(sb::npu::key(w, sb::npu::Field::Ttft), id, sb::npu::label(w, sb::npu::Field::Ttft), wr.ttftMs,
                       sb::npu::unit(w, sb::npu::Field::Ttft), Direction::LowerIsBetter);
            m_worstSpread = std::max(m_worstSpread, cvPercent(wr.windows));
        }
        m_ctx.subTestEnd(id, m_status[id]);
        if (m_stopped)
            break;
    }

    TestResult r = std::move(m_result);
    // Re-read the runtime version now that every workload has run: the OpenVINO
    // engine's LLM workload lazily loads the GenAI second runtime (DD-1 §5.4),
    // and the Runtime row should show it once present ("OpenVINO <desc> · GenAI"
    // — the C ABI has no GenAI version getter, so no number is appended).
    m_runtimeVersion = engine->runtimeVersion();
    finish(det, r);
    for (sb::npu::Workload w : sb::npu::kWorkloads) {
        const QString id = sb::npu::workloadId(w);
        const auto it = m_status.find(id);
        if (it == m_status.end())
            continue;   // never started (Stop landed before it)
        r.subResults.append({id, it->second, m_reason[id], {}});
    }

    if (m_stopped) {
        r.status = Status::Cancelled;
    } else {
        int ok = 0;
        QString firstProblem;
        for (const SubResult &s : std::as_const(r.subResults)) {
            if (s.status == Status::Ok)
                ++ok;
            else if (firstProblem.isEmpty())
                firstProblem = s.reason;
        }
        if (ok == 3)
            r.status = Status::Ok;
        else if (ok > 0) {
            r.status = Status::Partial;
            r.statusReason = QStringLiteral("No NPU score: not every workload ran");
        } else {
            r.status = Status::Error;
            r.statusReason = firstProblem;
        }
    }
    m_ctx.progress(1.0, QStringLiteral("Done"));
    return r;
}

} // namespace

NpuTest::NpuTest()
{
    m_info.id = QStringLiteral("npu");
    m_info.name = QStringLiteral("NPU");
    m_info.summary = QStringLiteral("Dedicated NPU inference: classification, detection, small LLM");
    m_info.colorToken = QStringLiteral("t-npu");
    // testVersion 1: this is the real measurement (replaces the stub).
    // formulaVersion 1: the task-19 calibration froze the anchors to Nivedellir
    // (Intel Core Ultra 7 268V / Intel AI Boost), pack_version "2" (§16.3).
    m_info.testVersion = 1;
    m_info.formulaVersion = 1;
    // D15: every run runs all three workloads; none is selectable.
    m_info.subTests = {{QStringLiteral("classify"), QStringLiteral("Classify"), false, true},
                       {QStringLiteral("detect"), QStringLiteral("Detect"), false, true},
                       {QStringLiteral("llm"), QStringLiteral("LLM"), false, true}};
}

Availability NpuTest::probe(const ProbeContext &ctx) const
{
    const sb::npu::NpuDetect det = sb::npu::detect(ctx.system());
    if (sb::npu::engineNotYetSupported(det.vendor)
        && (det.reason == sb::npu::NaReason::None || det.reason == sb::npu::NaReason::RuntimeMissing))
        return Availability::unavailable(sb::npu::engineNotYetSupportedText(det.vendor));
    switch (det.reason) {
    case sb::npu::NaReason::None: {
        Availability a = Availability::available();
        a.capabilities.append({sb::npu::vendorName(det.vendor) + QStringLiteral(" NPU"), Tone::Good});
        a.capabilities.append({det.runtimeVersion.isEmpty() ? QStringLiteral("runtime installed")
                                                            : QStringLiteral("runtime %1").arg(det.runtimeVersion),
                               Tone::Neutral});
        a.capabilities.append({ctx.npuModelsBaseUrl().isEmpty() ? QStringLiteral("model pack not configured")
                                                                : QStringLiteral("model pack configured"),
                               ctx.npuModelsBaseUrl().isEmpty() ? Tone::Warn : Tone::Neutral});
        return a;
    }
    case sb::npu::NaReason::RuntimeMissing:
        if (det.vendor == sb::npu::NpuVendor::Amd) {
            // AMD missing runtime: detect + report N/A with a link (locked), not a
            // guided setup (§3.3).
            Availability a = Availability::unavailable(naText(det));
            a.hint = Hint{QStringLiteral("Install the AMD Ryzen AI software"), kAmdRyzenAiUrl};
            return a;
        }
        // Intel/Qualcomm missing runtime: a fixable user install — available, but
        // a guided-setup Precondition (preconditions()) blocks the run (D4).
        return Availability::available();
    default:
        return Availability::unavailable(naText(det));
    }
}

OptionSchema NpuTest::options(const ProbeContext &ctx, const Availability &) const
{
    // §3.4: no device picker — Info rows only.
    const sb::npu::NpuDetect det = sb::npu::detect(ctx.system());
    OptionSchema o;
    const auto info = [&](const char *key, const char *label, const QString &value, Tone tone = Tone::Neutral) {
        OptionDef d;
        d.key = QLatin1StringView(key);
        d.label = QLatin1StringView(label);
        d.kind = OptionKind::Info;
        d.infoValue = value;
        d.infoTone = tone;
        o.options.append(d);
    };

    info("device", "NPU",
         det.vendor == sb::npu::NpuVendor::Null
             ? QStringLiteral("None")
             : det.deviceName.isEmpty() ? sb::npu::vendorName(det.vendor) + QStringLiteral(" NPU") : det.deviceName);

    QString runtime;
    if (det.reason == sb::npu::NaReason::None)
        runtime = det.runtimeVersion.isEmpty() ? QStringLiteral("installed") : det.runtimeVersion;
    else if (det.reason == sb::npu::NaReason::RuntimeMissing && det.vendor == sb::npu::NpuVendor::Amd)
        runtime = QStringLiteral("missing — N/A with link");
    else if (det.reason == sb::npu::NaReason::RuntimeMissing)
        runtime = QStringLiteral("missing — guided setup");
    else
        runtime = naText(det);
    info("runtime", "Runtime", runtime);

    info("models", "Model pack",
         ctx.npuModelsBaseUrl().isEmpty() ? QStringLiteral("not configured") : QStringLiteral("downloads on first run"));

    info("workload.classify", "Classify", QStringLiteral("image classification · images/s"));
    info("workload.detect", "Detect", QStringLiteral("object detection · images/s"));
    info("workload.llm", "LLM", QStringLiteral("small LLM text generation · tokens/s"));
    return o;
}

QList<Precondition> NpuTest::preconditions(const OptionValues &, const ProbeContext &ctx) const
{
    // A no-NPU / unsupported / AMD-missing-runtime machine is Unavailable: the
    // test reports N/A and can't run, so there's nothing to block pre-start (a
    // Model-pack precondition would be a red herring on a machine that can't run
    // the test at all, and would fail a no-NPU snapshot's pre-start check).
    if (probe(ctx).state == Availability::State::Unavailable)
        return {};

    QList<Precondition> out;
    const sb::npu::NpuDetect det = sb::npu::detect(ctx.system());
    const QString vendor = sb::npu::vendorName(det.vendor);

    // Guided setup (Intel/Qualcomm, runtime missing) — blocks the run until the
    // runtime is installed (D4). AMD keeps its N/A + link in probe(). The
    // descriptive text + URL live in `label` (the fill-width text); `detail` is
    // a short status badge so the Pill doesn't overflow (AP-5 5a).
    if (det.reason == sb::npu::NaReason::RuntimeMissing && det.vendor != sb::npu::NpuVendor::Amd) {
        Precondition p;
        p.id = QStringLiteral("runtime");
        p.label = QStringLiteral("An %1 NPU was found, but its runtime isn't installed — install it: %2")
                      .arg(vendor, guidedSetupUrl(det.vendor));
        p.state = Precondition::State::NotMet;
        p.detail = QStringLiteral("Not installed");
        out.append(p);
    }

    // Model pack (D2): no download source → NotMet; otherwise it downloads on
    // first run (the run re-checks and downloads before the measured work).
    Precondition models;
    models.id = QStringLiteral("models");
    if (ctx.npuModelsBaseUrl().isEmpty()) {
        models.state = Precondition::State::NotMet;
        models.label = QStringLiteral("Model pack — no download source is set (choose one in Settings)");
        models.detail = QStringLiteral("Not set");
    } else {
        models.state = Precondition::State::Met;
        models.label = QStringLiteral("Model pack");
        models.detail = QStringLiteral("Downloads on first run");
    }
    out.append(models);
    return out;
}

Validation NpuTest::validate(const OptionValues &) const
{
    // §11: nothing to validate beyond the generic checks (there's no free-form
    // input; the pack URL is a Settings value).
    return Validation::success();
}

std::chrono::seconds NpuTest::estimate(const OptionValues &) const
{
    return std::chrono::seconds(kMeasureSeconds + kEngineLoadSeconds + kModelLoadSeconds + kWarmupSeconds);
}

std::chrono::seconds NpuTest::timeLimit(const OptionValues &o) const
{
    // 2 × the estimate + 60 s (§2.2), plain wall-clock (no stub time scale —
    // NPU inference is real wall-clock, P-HOOK).
    return std::chrono::seconds(2 * estimate(o).count() + 60);
}

TestResult NpuTest::run(RunContext &ctx, const OptionValues &)
{
    NpuRun run(ctx);
    return run.run();
}

ScoreBreakdown NpuTest::score(const TestResult &result) const
{
    return sb::npu::formula_v1::score(result);
}

} // namespace sb::tests

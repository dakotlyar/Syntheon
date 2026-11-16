// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/engine/engine.h"

// The null engine (Phase 4c design §4.1, §13) and the `makeEngine` dispatch.
//
// The null engine is the CI/dev fake: it loads no vendor runtime and returns
// deterministic, SANE numbers (a fixed fake `seconds`, per-workload units in a
// realistic band, and `valid == units` — every synthetic inference passes the
// correctness sentinel), seeded from a fixed default so the assembled test and
// its unit tests run everywhere in milliseconds. It is always available; the
// *selection* of the null engine for the assembled test (SYNTHEON_NPU_FAKE_VENDOR
// dev hook) is task 14's job, but `nullEngine()` itself is callable directly
// here, so it is unit-testable before the assembled test lands.
//
// makeEngine() builds the real-vendor shim for a detected runtime path. It
// returns null (a missing runtime — the caller reports the N/A reason) only when
// there is no runnable runtime; a present-but-unloadable runtime is a shim whose
// init() fails cleanly with a clear error (RuntimeLoadFailed, §3.3).

#include <QString>

#include <cmath>
#include <memory>
#include <random>

namespace sb::npu {

namespace {

constexpr double kNullSeconds = 5.0;   // the fixed fake window duration (§2.2: default 5 s)
constexpr std::mt19937::result_type kNullSeed = 0x4E55BADDu;

// The deterministic PRNG seed: a fixed default, overridable via dev hook so a
// fixture can vary the sequence without weakening reproducibility else.
std::mt19937::result_type seedFor()
{
#if defined(SYNTHEON_DEV_HOOKS)
    bool ok = false;
    const qulonglong seed = qEnvironmentVariable("SYNTHEON_NPU_FAKE_SEED").toULongLong(&ok);
    if (ok)
        return static_cast<std::mt19937::result_type>(seed);
#endif
    return kNullSeed;
}
} // namespace

// The three real-vendor factories, defined in their own translation units. They
// return a valid engine (whose init() does the actual dlopen + symbol resolve and
// reports the first error); makeEngine() only calls them with a non-empty path.
std::unique_ptr<INpuEngine> makeOpenVinoEngine(const QString &runtimePath);
std::unique_ptr<INpuEngine> makeRyzenAiEngine(const QString &runtimePath);
#if defined(SYNTHEON_NPU_QNN_AVAILABLE)
std::unique_ptr<INpuEngine> makeQnnEngine(const QString &runtimePath);
#endif

namespace {

// The CI/dev fake: deterministic seeded numbers, no vendor runtime, never fails.
class NullEngine final : public INpuEngine
{
public:
    QString backendName() const override { return QStringLiteral("null"); }
    QString runtimeVersion() const override { return QStringLiteral("null"); }

    bool init(const LoadedModel &model, QString *) override
    {
        m_model = model;
        m_rng.seed(seedFor());
        return true;   // no runtime to load: the fake always initialises
    }

    bool runWindow(const QString &workload, const LoadedModel &, Inference &out, QString *) override
    {
        // Sane per-workload rates in a realistic NPU band (img/s / tok/s), with a
        // tiny deterministic (±3 %) jitter so the three windows spread slightly for
        // the median-of-3. Draws from std::mt19937's *raw* output (the engine is
        // fully specified) rather than std::uniform_real_distribution, whose results
        // are not portable — this keeps the numbers bit-identical across platforms.
        const double u01 = double(m_rng() % 65536u) / 65535.0;
        const double jitter = 0.97 + 0.06 * u01;
        const double rate = baseRate(workload) * jitter;

        out.seconds = kNullSeconds;
        out.units = std::int64_t(std::llround(rate * kNullSeconds));
        out.valid = out.units;   // every synthetic inference passes the sentinel
        return true;
    }

private:
    static double baseRate(const QString &workload)
    {
        if (workload == QLatin1StringView("classify"))
            return 1450.0;                       // ResNet-50 INT8 imagery/s on a typical NPU
        if (workload == QLatin1StringView("detect"))
            return 900.0;                        // YOLOv8n INT8 imagery/s
        if (workload == QLatin1StringView("llm"))
            return 55.0;                         // ~1.5 B INT4 generation tok/s
        return 100.0;                            // unknown workload: still sane, still deterministic
    }

    LoadedModel m_model;
    std::mt19937 m_rng{kNullSeed};
};

} // namespace

std::unique_ptr<INpuEngine> nullEngine()
{
    return std::make_unique<NullEngine>();
}

std::unique_ptr<INpuEngine> makeEngine(EngineVendor vendor, const QString &runtimePath, QString *error)
{
    const auto fail = [&](const QString &message) -> std::unique_ptr<INpuEngine> {
        if (error)
            *error = message;
        return {};
    };

    switch (vendor) {
    case EngineVendor::Null:
        return nullEngine();
    case EngineVendor::Intel:
        if (runtimePath.isEmpty())
            return fail(QStringLiteral("no OpenVINO runtime path resolved for the Intel NPU"));
        return makeOpenVinoEngine(runtimePath);
    case EngineVendor::Amd:
        if (runtimePath.isEmpty())
            return fail(QStringLiteral("no Ryzen AI runtime path resolved for the AMD NPU"));
        return makeRyzenAiEngine(runtimePath);
    case EngineVendor::Qualcomm:
#if defined(SYNTHEON_NPU_QNN_AVAILABLE)
        if (runtimePath.isEmpty())
            return fail(QStringLiteral("no QNN runtime path resolved for the Qualcomm NPU"));
        return makeQnnEngine(runtimePath);
#else
        return fail(QStringLiteral("Qualcomm QNN runs only on Windows on ARM64"));
#endif
    }
    return fail(QStringLiteral("unknown NPU engine vendor"));
}

} // namespace sb::npu

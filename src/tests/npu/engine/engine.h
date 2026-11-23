// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU engine interface (Phase 4c design §4.1, verbatim). One `INpuEngine`
// wraps one vendor's runtime; implementations dlopen/LoadLibrary the
// USER-INSTALLED vendor runtime at run time (never bundled, never linked —
// P-VENDOR) and drive inference through a hand-written C shim, so no vendor
// header or .lib is ever compiled in.
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

#include <cstdint>

namespace sb::npu {

// QNN runs only on Windows-on-ARM (design N4, mirrored from detect.cpp's
// qualcommSupported()). Single source of truth: the QNN translation unit and
// makeEngine() (engine_null.cpp) both key off this macro, so the Qualcomm
// branch can't drift between the two. Defined here — the shared interface
// header every engine translation unit and tst_npu_engine.cpp already include —
// so the gate is visible everywhere it is needed without a second include
// (AP-5 C3: the macro must not be a private detail of engine_dlopen.h).
#if defined(Q_OS_WIN) && (defined(Q_PROCESSOR_ARM_64) || defined(_M_ARM64))
#  define SYNTHEON_NPU_QNN_AVAILABLE 1
#endif

// One vendor backend. Implementations dlopen/LoadLibrary the USER-INSTALLED
// vendor runtime (never bundled, never linked — P-VENDOR) and resolve a
// hand-written C shim; no vendor header or .lib is compiled in.
enum class EngineVendor { Intel, Amd, Qualcomm, Null };

struct LoadedModel {
    QString name;      // "resnet50-int8", "yolov8n-int8", "llm-1b-int4"
    QString precision; // "INT8" / "INT4" / "FP16" (text, for display + model.<w> metrics)
    QString path;      // the verified model file(s) on disk
    // The per-model cache DIRECTORY (<cacheRoot>/npu-models/<pack>/<model>/1/),
    // holding every file of the model (Phase 4c §5.1). A stateful LLM is
    // multi-file (openvino_model.xml/.bin + tokenizer), so `path` alone can't
    // name the whole model; the LLM decoder is found by name inside `dir`.
    // Empty when the engine has no directory (classify/detect use `path`).
    QString dir;
};

// One inference's outcome, for the correctness sentinel (§2).
struct Inference {
    double seconds = 0.0;   // wall-clock for this batch (excl. first-load compile)
    std::int64_t units = 0; // images processed, or tokens generated
    std::int64_t valid = 0; // sanity check: output shape + all-finite (vision, once per window) / tokens > 0 (LLM)
    // llm only: the time from the prompt to the first generated token, in
    // seconds (0 = not measured). The workload reports it as the `llm.ttft`
    // metric (§9, reported, not scored); the real-vendor engines fill it during
    // validation (4c-5), the null engine reports a deterministic sane value now.
    double ttftSeconds = 0.0;
};

class INpuEngine {
public:
    virtual ~INpuEngine() = default;
    virtual QString backendName() const = 0;      // "openvino-npu" / "ryzenai-vitis" / "qnn" / "null"
    virtual QString runtimeVersion() const = 0;   // resolved from the loaded module, "" if unknown
    // Load the runtime and the given model; report the first error that occurs.
    // Returns false + sets *error on any failure; must never throw.
    virtual bool init(const LoadedModel &model, QString *error) = 0;
    // Run one measurement window (5 s) of the named workload against `model`;
    // append to *out. Returns false only on a real failure (not cancel). The
    // optional `cancelled` predicate is polled between inferences so a Stop can
    // break the loop early (P-STOP ≤ 250 ms); a clean break still returns true
    // with `out.seconds` set to the actual elapsed span.
    virtual bool runWindow(const QString &workload, const LoadedModel &model,
                           Inference &out, QString *error,
                           const std::function<bool()> &cancelled = {}) = 0;
};

// Build the engine for the detected vendor's runtime. Returns null if there is
// no runnable runtime (the caller reports the N/A reason). `nullEngine()` is the
// CI/dev fake that returns deterministic numbers without any vendor runtime.
std::unique_ptr<INpuEngine> makeEngine(EngineVendor vendor, const QString &runtimePath, QString *error);
std::unique_ptr<INpuEngine> nullEngine();

} // namespace sb::npu

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU engine interface (Phase 4c design §4.1, verbatim). One `INpuEngine`
// wraps one vendor's runtime; implementations dlopen/LoadLibrary the
// USER-INSTALLED vendor runtime at run time (never bundled, never linked —
// P-VENDOR) and drive inference through a hand-written C shim, so no vendor
// header or .lib is ever compiled in.
#include <QByteArray>
#include <QString>
#include <memory>

#include <cstdint>

namespace sb::npu {

// One vendor backend. Implementations dlopen/LoadLibrary the USER-INSTALLED
// vendor runtime (never bundled, never linked — P-VENDOR) and resolve a
// hand-written C shim; no vendor header or .lib is compiled in.
enum class EngineVendor { Intel, Amd, Qualcomm, Null };

struct LoadedModel {
    QString name;      // "resnet50-int8", "yolov8n-int8", "llm-1b-int4"
    QString precision; // "INT8" / "INT4" / "FP16" (text, for display + model.<w> metrics)
    QString path;      // the verified model file(s) on disk
};

// One inference's outcome, for the correctness sentinel (§2).
struct Inference {
    double seconds = 0.0;   // wall-clock for this batch (excl. first-load compile)
    std::int64_t units = 0; // images processed, or tokens generated
    std::int64_t valid = 0; // sanity check: argmax in range / boxes plausible / tokens > 0
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
    // append to *out. Returns false only on a real failure (not cancel).
    virtual bool runWindow(const QString &workload, const LoadedModel &model,
                           Inference &out, QString *error) = 0;
};

// Build the engine for the detected vendor's runtime. Returns null if there is
// no runnable runtime (the caller reports the N/A reason). `nullEngine()` is the
// CI/dev fake that returns deterministic numbers without any vendor runtime.
std::unique_ptr<INpuEngine> makeEngine(EngineVendor vendor, const QString &runtimePath, QString *error);
std::unique_ptr<INpuEngine> nullEngine();

} // namespace sb::npu

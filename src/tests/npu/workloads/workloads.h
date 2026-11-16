// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The three NPU workloads, vendor-agnostic (Phase 4c design §2). Each runs the
// engine's `runWindow` in a 3×window loop, takes the median throughput, and
// applies the §2 correctness sentinel — the first inference must produce a
// valid output (argmax in-range / box count plausible / tokens > 0), otherwise
// a garbage-producing vendor stack is an Error, never scored. The engine owns
// the deterministic input (a fixed synthetic image / prompt, design §8); the
// workload only orchestrates the windows and the sentinel.

#include "tests/npu/engine/engine.h"

#include <QString>

#include <array>
#include <functional>

namespace sb::npu::workloads {

enum class WorkloadStatus { Ok, Error, Cancelled };

// One workload's three windows (§2.1 median-of-3) plus the llm's TTFT.
struct WorkloadResult {
    WorkloadStatus status = WorkloadStatus::Ok;
    double median = 0.0;              // the median throughput (img/s or tok/s)
    std::array<double, 3> windows{};  // the raw per-window throughputs
    double ttftMs = 0.0;              // llm only: time to first token (0 = not measured)
    QString error;                    // set when status != Ok
};

// The shared core: three windows of `workloadId` against `model`, the median,
// and the sentinel. `cancelled` is polled before each window (P-STOP ≤ 250 ms);
// `report(window)` is called before window `window` (0–2) runs, so the run can
// advance its progress and stage there (and a Stop that lands on that stage is
// seen before the window begins).
WorkloadResult runWindows(const QString &workloadId, INpuEngine &engine, const LoadedModel &model,
                          const std::function<bool()> &cancelled,
                          const std::function<void(int window)> &report = {});

} // namespace sb::npu::workloads

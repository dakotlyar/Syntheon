// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/workloads/workloads.h"

#include <algorithm>

namespace sb::npu::workloads {

namespace {

double medianOf3(std::array<double, 3> v)
{
    std::sort(v.begin(), v.end());
    return v[1];
}

} // namespace

WorkloadResult runWindows(const QString &workloadId, INpuEngine &engine, const LoadedModel &model,
                          const std::function<bool()> &cancelled, const std::function<void(int)> &report)
{
    WorkloadResult out;
    QString error;

    for (int i = 0; i < 3; ++i) {
        if (report)
            report(i);
        // Poll Stop before each window so it lands within one window (P-STOP).
        if (cancelled && cancelled()) {
            out.status = WorkloadStatus::Cancelled;
            return out;
        }
        Inference inf;
        if (!engine.runWindow(workloadId, model, inf, &error, cancelled)) {
            out.status = WorkloadStatus::Error;
            out.error = error.isEmpty() ? QStringLiteral("the %1 runtime failed to run a window").arg(workloadId) : error;
            return out;
        }
        // §2 correctness sentinel, on the first inference (window 0): a vendor
        // stack that runs but emits garbage (no valid output) is an Error with a
        // reason, never scored. `valid` is the engine's count of in-range /
        // plausible / non-empty outputs; the null engine always passes.
        if (i == 0 && (inf.valid <= 0 || inf.units <= 0)) {
            out.status = WorkloadStatus::Error;
            out.error = QStringLiteral("the %1 runtime produced no valid output (correctness sentinel failed)").arg(workloadId);
            return out;
        }
        const double rate = inf.seconds > 0.0 ? double(inf.units) / inf.seconds : 0.0;
        out.windows[i] = rate;
        if (i == 0)
            out.ttftMs = inf.ttftSeconds > 0.0 ? inf.ttftSeconds * 1000.0 : 0.0;
    }

    out.median = medianOf3(out.windows);
    out.status = WorkloadStatus::Ok;
    return out;
}

} // namespace sb::npu::workloads

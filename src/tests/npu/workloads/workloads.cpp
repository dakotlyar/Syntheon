// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/workloads/workloads.h"

#include <algorithm>
#include <cmath>

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
        // A Stop that lands INSIDE runWindow (between inferences) breaks the loop
        // early and — per the engine contract — returns true with a truncated
        // span. Poll it again immediately, BEFORE the sentinel: a Stopped final
        // window must be Cancelled, never an Ok that commits a truncated ~200 ms
        // window as a composite score, and a Stop at window 0 must never surface
        // as a bogus Error/Partial instead (P-STOP ≤ 250 ms; EC-P10 silent-wrong).
        if (cancelled && cancelled()) {
            out.status = WorkloadStatus::Cancelled;
            return out;
        }
        // §2 correctness sentinel, on EVERY window (i = 0..2): a vendor stack
        // that runs but emits garbage — no valid output, no units, or a
        // zero/non-finite elapsed span (the driver reset after window 0, say) —
        // is an Error with a reason, never scored. Guarantees `median > 0`
        // whenever Ok is returned. `valid` is the engine's count of in-range /
        // plausible / non-empty outputs; the null engine always passes.
        if (inf.valid <= 0 || inf.units <= 0 || !(inf.seconds > 0.0) || !std::isfinite(inf.seconds)) {
            out.status = WorkloadStatus::Error;
            out.error = QStringLiteral("the %1 runtime produced no valid output (correctness sentinel failed)").arg(workloadId);
            return out;
        }
        const double rate = double(inf.units) / inf.seconds;
        out.windows[i] = rate;
        if (i == 0)
            out.ttftMs = inf.ttftSeconds > 0.0 ? inf.ttftSeconds * 1000.0 : 0.0;
    }

    out.median = medianOf3(out.windows);
    out.status = WorkloadStatus::Ok;
    return out;
}

} // namespace sb::npu::workloads

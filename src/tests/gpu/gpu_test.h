// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU test (Phase 4a design): four 3D scenes on every graphics API, run
// scene by scene across the APIs (D12) in a worker process per API
// (synbench-gpu), with the main window held still while they draw. Task 24:
// every API the probe found (Vulkan, DirectX 12 and 11, OpenGL) runs, with the
// cross-API signature check (D20); task 28 added native 2D and task 32
// fill-rate (Quads and Sprites on every API), which completes formula v1.

#include "core/itest.h"
#include "tests/gpu/model/gpu_identity.h"
#include "tests/gpu/model/protocol.h"

#include <QMutex>
#include <QVector>

namespace sb::tests {

class GpuTest : public ITest
{
public:
    GpuTest();

    const TestInfo &info() const override { return m_info; }
    Availability probe(const ProbeContext &ctx) const override;
    OptionSchema options(const ProbeContext &ctx, const Availability &availability) const override;
    std::chrono::seconds estimate(const OptionValues &) const override;
    std::chrono::seconds timeLimit(const OptionValues &) const override;
    TestResult run(RunContext &ctx, const OptionValues &options) override;
    ScoreBreakdown score(const TestResult &result) const override;
    DetailLayout detail(const TestResult &result) const override;  // gpu_detail.cpp

    // §8: ceil10(16 + 124 a + 46 n), a = 3D APIs, n = native 2D paths.
    static int estimateSeconds(int apis3d, int nativePaths);
    // §8's 3D windows at real time (warm-up, measure): geometry 5 s + 20 s (it
    // includes the first workload's extra 2 s), draw calls, shading and
    // particles 3 s + 20 s.
    static QVector<gpu::protocol::PlanItem> plan3d();
    // §8: the fill workloads, after the four scenes on every API: Quads and Sprites, warm-up 2 s + 10 s each.
    static QVector<gpu::protocol::PlanItem> planFill();

private:
    // The cached probe (a lock: probe() runs on a pool thread, options() and estimate() on the UI thread).
    QVector<gpu::GpuEntry> gpus() const;

    TestInfo m_info;
    mutable QMutex m_mutex;
    mutable QVector<gpu::GpuEntry> m_gpus;
};

} // namespace sb::tests

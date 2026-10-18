// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The CPU test (Phase 2 design): seven groups on all threads (integer,
// floating point + primes, SIMD, compression, encryption, sorting, physics)
// and the single-thread mark, always all of them (D14). The run sequence and
// time budget are §3; the measurement engine §4; the kernels §5; telemetry
// and the throttling flag §8; the result model §9.

#include "core/itest.h"

namespace sb::tests {

class CpuTest : public ITest
{
public:
    CpuTest();

    const TestInfo &info() const override { return m_info; }
    Availability probe(const ProbeContext &ctx) const override;
    OptionSchema options(const ProbeContext &ctx, const Availability &availability) const override;
    std::chrono::seconds estimate(const OptionValues &) const override;
    std::chrono::seconds timeLimit(const OptionValues &) const override;
    TestResult run(RunContext &ctx, const OptionValues &options) override;
    ScoreBreakdown score(const TestResult &result) const override;

    // The fixed budget of a run (§3), in seconds at real time.
    static constexpr int kBudgetSeconds = 270;

private:
    TestInfo m_info;
};

} // namespace sb::tests

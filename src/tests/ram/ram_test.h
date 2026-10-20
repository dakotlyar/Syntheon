// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The RAM test (Phase 3 design): a pointer-chase latency curve with the
// cache levels measured in the middle of their plateaus, read / write / copy
// bandwidth on one core and on every logical CPU, and the DRAM latency while
// the other cores stream reads (loaded latency), always all three (D15). The
// run sequence and time budget are §3; placement §4; the kernels §5; pages
// §6; the SMBIOS readout §7; the result §2 and §11; the score §10
// (formula_v1); the Detail page §12 as the approved mockup draws it
// (ram_detail.cpp).

#include "core/itest.h"

namespace sb::tests {

class RamTest : public ITest
{
public:
    RamTest();

    const TestInfo &info() const override { return m_info; }
    Availability probe(const ProbeContext &ctx) const override;
    OptionSchema options(const ProbeContext &ctx, const Availability &availability) const override;
    std::chrono::seconds estimate(const OptionValues &) const override;
    std::chrono::seconds timeLimit(const OptionValues &) const override;
    TestResult run(RunContext &ctx, const OptionValues &options) override;
    ScoreBreakdown score(const TestResult &result) const override;
    // The approved mockup's layout (ram_detail.cpp).
    DetailLayout detail(const TestResult &result) const override;

    // §3, D16: the fixed budget of a run at real time, per architecture.
    static constexpr int kBudgetSecondsX64 = 160;
    static constexpr int kBudgetSecondsArm64 = 125;

private:
    TestInfo m_info;
};

} // namespace sb::tests

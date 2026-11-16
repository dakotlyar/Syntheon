// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU test (Phase 4c design): dedicated-NPU inference across three
// workloads — image classification, object detection and a small LLM — on the
// primary (first runnable) vendor's NPU, all three always run (D15: none is
// user-selectable). The run sequence is §7; the vendor/runtime detection §3;
// the model pack §6; the score §10 (formula_v1); the Detail page §12
// (npu_detail.cpp). Replaces the Phase 1 stub's class name NpuTest (design
// §1.1).

#include "core/itest.h"

namespace sb::tests {

class NpuTest : public ITest
{
public:
    NpuTest();

    const TestInfo &info() const override { return m_info; }
    Availability probe(const ProbeContext &ctx) const override;
    OptionSchema options(const ProbeContext &ctx, const Availability &availability) const override;
    QList<Precondition> preconditions(const OptionValues &o, const ProbeContext &ctx) const override;
    Validation validate(const OptionValues &o) const override;
    std::chrono::seconds estimate(const OptionValues &o) const override;
    std::chrono::seconds timeLimit(const OptionValues &o) const override;
    TestResult run(RunContext &ctx, const OptionValues &o) override;
    ScoreBreakdown score(const TestResult &result) const override;
    DetailLayout detail(const TestResult &result) const override;

private:
    TestInfo m_info;
};

} // namespace sb::tests

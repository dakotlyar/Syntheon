// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/itest.h"

namespace sb::tests {

// Placeholder behaviour until the Phase 1 stub tests land (task 11): the real
// TestInfo, always available, no options, finishes at once with no metrics and
// no score.
class SkeletonTest : public ITest
{
public:
    explicit SkeletonTest(TestInfo info) : m_info(std::move(info)) {}

    const TestInfo &info() const override { return m_info; }
    Availability probe(const ProbeContext &) const override { return Availability::available(); }
    OptionSchema options(const ProbeContext &, const Availability &) const override { return {}; }
    std::chrono::seconds estimate(const OptionValues &) const override { return std::chrono::seconds(1); }
    std::chrono::seconds timeLimit(const OptionValues &) const override;
    TestResult run(RunContext &ctx, const OptionValues &) override;
    ScoreBreakdown score(const TestResult &) const override { return {}; }

private:
    TestInfo m_info;
};

} // namespace sb::tests

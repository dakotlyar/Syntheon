// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/itest.h"

#include <functional>

namespace sb::testing {

// A test double for ITest. Behaviour is supplied per test case.
class FakeTest : public ITest
{
public:
    explicit FakeTest(TestInfo info) : m_info(std::move(info)) {}

    static TestInfo makeInfo(const QString &id, Exclusivity ex = Exclusivity::Shared)
    {
        TestInfo i;
        i.id = id;
        i.name = id.toUpper();
        i.exclusivity = ex;
        return i;
    }

    const TestInfo &info() const override { return m_info; }
    Availability probe(const ProbeContext &) const override { return availability; }
    OptionSchema options(const ProbeContext &, const Availability &) const override { return schema; }
    QList<Precondition> preconditions(const OptionValues &, const ProbeContext &) const override
    {
        return preconds;
    }
    Validation validate(const OptionValues &v) const override
    {
        return validator ? validator(v) : Validation::success();
    }
    std::chrono::seconds estimate(const OptionValues &) const override { return estimateValue; }
    std::chrono::seconds timeLimit(const OptionValues &) const override { return limit; }
    TestResult run(RunContext &ctx, const OptionValues &options) override
    {
        if (holdDuringRun)
            ctx.holdInterface(true);  // and never lets go: the orchestrator releases it
        return runner ? runner(ctx, options) : TestResult{};
    }
    TestResult recover(RecoveryContext &ctx) override { return recoverer ? recoverer(ctx) : ITest::recover(ctx); }
    ScoreBreakdown score(const TestResult &r) const override { return scorer ? scorer(r) : ScoreBreakdown{}; }

    TestInfo m_info;
    Availability availability;
    OptionSchema schema;
    QList<Precondition> preconds;
    std::function<Validation(const OptionValues &)> validator;
    std::chrono::seconds estimateValue{1};
    std::chrono::seconds limit{60};
    bool holdDuringRun = false;
    std::function<TestResult(RunContext &, const OptionValues &)> runner;
    std::function<ScoreBreakdown(const TestResult &)> scorer;
    std::function<TestResult(RecoveryContext &)> recoverer;
};

class FakeProbeContext : public ProbeContext
{
public:
    const SystemSnapshot &system() const override { return snapshot; }
    PrivilegedOps &privileged() const override { return ops; }
    SystemSnapshot snapshot;
    mutable UnavailablePrivilegedOps ops;
};

} // namespace sb::testing

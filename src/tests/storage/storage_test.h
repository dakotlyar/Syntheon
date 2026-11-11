// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Storage test (Phase 4b design): writes one aligned, direct/uncached file
// on the chosen drive, measures it with a per-OS async engine for the six
// patterns (seq 1 MiB at Q8T1/Q1T1, random 4 KiB at Q32T1/Q1T1, latency from
// the Q1T1 random reads, and mixed 70/30), and reports the median of 3 x 5 s
// per read/write phase. The run sequence is §8; the picker/guards §3; the
// result §10; the score §11 (formula_v1); the Detail page §13 (storage_detail).
// Replaces the Phase 1 stub's class name StorageTest (N8); the history owner
// class sb::Storage in src/storage/ is unrelated and untouched.

#include "core/itest.h"

namespace sb::tests {

class StorageTest : public ITest
{
public:
    StorageTest();

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

    static constexpr QLatin1StringView kDriveKey{"drive"};
    static constexpr QLatin1StringView kFileSizeKey{"file_size"};

private:
    TestInfo m_info;
};

} // namespace sb::tests

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/detail.h"
#include "core/options.h"
#include "core/result.h"
#include "core/test_info.h"

#include <QCborMap>
#include <QList>
#include <QString>

#include <chrono>

namespace sb {

// What a test sees during discovery (probe/options/preconditions), on a pool
// thread.
class ProbeContext
{
public:
    virtual ~ProbeContext() = default;
    virtual const SystemSnapshot &system() const = 0;
    virtual PrivilegedOps &privileged() const = 0;
};

class RunContext;

// Optional per-sub-test progress/status: ctx.subTest("int_fp").begin() … .end(Status::Ok).
class SubTestHandle
{
public:
    SubTestHandle(RunContext &ctx, QString id) : m_ctx(ctx), m_id(std::move(id)) {}
    void begin();
    void end(Status status);

private:
    RunContext &m_ctx;
    QString m_id;
};

// The test's only window to the outside world while it runs (§2.7). The
// contract is process-agnostic (D2): a separate runner process can implement it
// later without plugin changes. progress(), log(), cancelled() and the
// sub-test calls are safe from the test's own helper threads; checkpoint() and
// scratchDir() write to the database and must be called from the thread that
// called run().
class RunContext
{
public:
    virtual ~RunContext() = default;

    // 0–1 for this test, plus a stage label ("Write, plain stores · rep 2 of 5").
    virtual void progress(double fraction, const QString &stage) = 0;
    // The part of the test now running ("All-thread bandwidth"), shown in the
    // Progress screen's live-log pill while the stage line under the test
    // gives the step (Phase 3, Dave 2026-10-20). It applies from the next
    // progress() on; empty clears it. Optional: a test that never sets one
    // gets its stage in the pill, as before. Safe from helper threads.
    virtual void setPart(const QString &part) { (void)part; }
    // Holds the interface still while the test runs something the main
    // window's own drawing would disturb (the GPU test, Phase 4a P10): no
    // animation, no ticker, no live progress or log repaint until released.
    // Progress, log lines and sub-test changes still arrive and are applied
    // on release. The orchestrator releases it when run() returns. Safe from
    // helper threads. Optional: tests that never call it are unchanged.
    virtual void holdInterface(bool hold) { (void)hold; }
    virtual void log(LogLevel level, const QString &message) = 0;
    // Poll at least every 250 ms and return promptly once true.
    virtual bool cancelled() const = 0;

    SubTestHandle subTest(const QString &id) { return SubTestHandle(*this, id); }
    virtual void subTestBegin(const QString &id) = 0;
    virtual void subTestEnd(const QString &id, Status status) = 0;

    // Durable write to the database (Battery samples).
    virtual void checkpoint(const QCborMap &payload) = 0;
    // A temp folder registered in the database, so it's cleaned up even after
    // a crash. Created on first call; empty if it can't be created.
    virtual QString scratchDir() = 0;
    // The Storage test's per-run target folder on a chosen drive (§3.2, D5):
    // creates the hidden `.syntheon-storage-<run uuid>` directory under
    // `parentDir` and registers it for crash cleanup. The orchestrator uses the
    // run's uuid and adds a scratch_paths row; the default (test/mock contexts)
    // creates it with a fresh uuid and registers nothing.
    virtual QString registerTargetScratchDir(const QString &parentDir);
    // The snapshot taken at run start.
    virtual const SystemSnapshot &system() const = 0;
    // Typed root-only operations (§6); fail with PrivError, never hang.
    virtual PrivilegedOps &privileged() = 0;
};

// Given to ITest::recover() at startup for a run left in 'running' (task 6).
class RecoveryContext
{
public:
    virtual ~RecoveryContext() = default;
    virtual const OptionValues &options() const = 0;
    virtual QList<QCborMap> checkpoints() const = 0;
    virtual const SystemSnapshot &system() const = 0;
};

class ITest
{
public:
    virtual ~ITest() = default;
    virtual const TestInfo &info() const = 0;

    // Discovery: called on a pool thread at startup and on "refresh"; must
    // finish in about 2 s.
    virtual Availability probe(const ProbeContext &) const = 0;
    virtual OptionSchema options(const ProbeContext &, const Availability &) const = 0;

    // Live pre-start checks; polled while the tab is visible.
    virtual QList<Precondition> preconditions(const OptionValues &, const ProbeContext &) const { return {}; }

    virtual Validation validate(const OptionValues &) const { return Validation::success(); }  // pure
    virtual std::chrono::seconds estimate(const OptionValues &) const = 0;   // for "≈ 14 min"
    virtual std::chrono::seconds timeLimit(const OptionValues &) const = 0;  // hard cap; 0 = none

    // Execution: worker thread, blocking, cooperative cancel. A test that
    // ignores a cancel for the grace period is abandoned (R7): the run is
    // saved as Interrupted and the thread keeps running until the test
    // returns. System sleep stays blocked until then on Windows (F7).
    virtual TestResult run(RunContext &, const OptionValues &) = 0;
    virtual TestResult recover(RecoveryContext &) { return TestResult::interrupted(); }

    // Pure functions over stored data (unit-testable, deterministic).
    virtual ScoreBreakdown score(const TestResult &) const = 0;
    virtual DetailLayout detail(const TestResult &result) const { return genericDetailLayout(result); }
};

// Option key of the automatic "Sub-tests" MultiSelect. Stable forever.
inline constexpr QLatin1StringView kSubTestsOptionKey("subtests");

// The schema the option tab shows: the test's own options plus, when it has
// user-selectable sub-tests, an automatic "Sub-tests" MultiSelect (so every
// test looks the same). Sub-tests the probe marked unavailable are disabled
// with their reason.
OptionSchema effectiveOptionSchema(const ITest &test, const ProbeContext &ctx, const Availability &availability);

} // namespace sb

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The CPU test's side of the encryption worker (Phase 2 design §5.5, D6, D7,
// P17): starts synbench-cpucrypto with QProcess, reads its JSON lines, and
// kills it on Stop (polled every 50 ms) or at 2 x the expected time + 10 s.
// Killing a process is safe, unlike a thread; the worker also exits by
// itself when its stdin (held open here) closes, so it never outlives
// SynBench.
//
// The worker's environment is built from scratch, never inherited (P17):
//   Linux    empty; a software run adds the mask
//   Windows  SystemRoot, and PATH set to <SystemRoot>\System32 (Qt's QProcess
//            copies PATH from the parent into an environment that lacks it);
//            a software run adds the mask
// So OPENSSL_CONF, OPENSSL_MODULES, OPENSSL_ENGINES and anything else in
// SynBench's environment never reach it.
// It runs in its own folder (not wherever SynBench was started), and on
// Windows its imports load from System32 and that folder only
// (/DEPENDENTLOADFLAG:0xA00). A run's output must have the right shape
// (shapeProblem) and no line may exceed kMaxOutputLine.
//
// The software path (D7): the dedicated crypto instructions off, SIMD on.
//   x64    OPENSSL_ia32cap=~0x200000200000000:~0x60020000000:~0x0:~0x0:~0x0
//          clears AES-NI and PCLMULQDQ (LV0 bits 57, 33) and SHA, VAES and
//          VPCLMULQDQ (LV1 bits 29, 41, 42), and leaves the other logical
//          vectors as they are (the two-value form would zero them)
//   ARM64  OPENSSL_armcap=1: NEON only (Windows: our armcap patch, D5)

#include "tests/cpu/engine/worker_pool.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace sb::cpu {

enum class CryptoAlg { AesGcm, Sha256 };

struct CryptoRunSpec {
    CryptoAlg alg = CryptoAlg::AesGcm;
    bool software = false;
    int threads = 1;
    WindowSpec windows;
    std::uint64_t seed = 0;
    // Tests: instead of 2 x the expected time + 10 s.
    std::optional<std::chrono::milliseconds> timeout;
};

struct CryptoOutcome {
    enum class Status { Ok, Cancelled, Failed };
    Status status = Status::Failed;
    QString error;      // Failed: the worker's last stderr line, or what went wrong
    QString openssl;    // "3.5.9"
    QString features;   // the CPU's crypto features, "aes,pclmul,sha,vaes,vpclmulqdq"
    bool masked = false;  // the worker saw the software-path mask
    std::vector<double> repMbps;
    double medianMbps = 0;
    double spread = 0;
    std::uint64_t checksum = 0;
    qint64 pid = 0;     // the worker's process id (the log; tests: nothing left behind)
    bool ok() const { return status == Status::Ok; }
};

class CryptoClient
{
public:
    // The worker: next to the app (Windows, a dev build), or ../libexec from
    // it (an install, the AppImage's usr/libexec). Empty if it isn't there.
    static QString workerPath();
    // The worker's environment for a hardware or a software run.
    static QProcessEnvironment environment(bool software);
    // The mask variable and its value for this architecture (D7).
    static QString maskVariable();
    static QString maskValue();
    // 2 x (warm-up + repetitions) + 10 s.
    static std::chrono::milliseconds timeoutFor(const CryptoRunSpec &spec);

    // One measurement. `window` (optional) hears each window start (0 = the
    // warm-up). Blocking; polls `cancelled` every 50 ms.
    static CryptoOutcome run(const CryptoRunSpec &spec, const std::function<bool()> &cancelled,
                             const std::function<void(int window)> &window = {});
    // --self-test: the known-answer tests and every seed against the golden
    // table (for probe(): a missing or failing worker makes crypto unavailable).
    static CryptoOutcome selfTest(const std::function<bool()> &cancelled);
    // What the checksum of a passing run is: the XOR of the golden table.
    static std::uint64_t expectedChecksum(CryptoAlg alg);

    // The checks on a measurement's output (AP-5 L6), used by run(): empty if
    // it has the right shape (a ready line, `reps` rep lines, the mask seen
    // exactly on a software run), else the reason.
    static QString shapeProblem(const CryptoOutcome &o, bool sawReady, int reps, bool software);
    // An output line (no newline yet) longer than this is junk: the worker
    // is stopped rather than buffered without end.
    static constexpr qsizetype kMaxOutputLine = 64 * 1024;
    static bool lineTooLong(const QByteArray &pending);
    // The checks on a measurement's result, after its shape (Zeb, before
    // task 14): the checksum must be expectedChecksum(), and every rate
    // positive and under ceilingMbps(). Empty if it passes, else the reason.
    static QString resultProblem(const CryptoOutcome &o, CryptoAlg alg, int threads);
    // The client's own plausibility ceiling, in MB/s: the engine's P14 rule
    // with the worker's per-thread bytes per cycle (AES-GCM 16, SHA-256 8)
    // at 7 GHz x 1.5 x threads. A worker past it isn't measuring.
    static double ceilingMbps(CryptoAlg alg, int threads);
#if defined(SYNBENCH_DEV_HOOKS)
    // Tests: run `program` (with `extraArgs` before the usual ones) instead
    // of the real worker; an empty program restores the real one.
    static void setWorkerForTests(const QString &program, const QStringList &extraArgs = {});
#endif
};

} // namespace sb::cpu

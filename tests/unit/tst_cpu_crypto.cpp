// SPDX-License-Identifier: GPL-3.0-or-later
// The encryption worker and its client (Phase 2 design §5.5, §14.1 "Crypto
// worker"): the known-answer tests and golden table (--self-test); the
// hardware and the software path agree with each other and the golden
// checksum, and the software one is really slower where the CPU has the
// instructions; the environment is built from scratch and the worker drops
// OpenSSL's variables itself (a planted OPENSSL_CONF pointing at a bad file
// is ignored); a failure carries the worker's stderr; Stop and the timeout
// kill it and leave nothing behind; stdin EOF ends it.
#include "isa/cpu_features.h"
#include "tests/cpu/crypto_client.h"

#include <QProcess>
#include <QTest>

#include <algorithm>
#include <thread>

#if defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

using namespace sb::cpu;
using namespace std::chrono_literals;

namespace {

bool processAlive(qint64 pid)
{
#if defined(Q_OS_WIN)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h)
        return false;
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno != ESRCH;
#endif
}

CryptoRunSpec quick(CryptoAlg alg, bool software)
{
    CryptoRunSpec s;
    s.alg = alg;
    s.software = software;
    s.threads = 2;
    s.windows = {20ms, 60ms, 2};
    return s;
}

// Runs the worker directly (not through the client), with `env`, and returns
// what --print-env (a developer-build option) prints.
QStringList workerEnvironment(const QProcessEnvironment &env)
{
    QProcess p;
    p.setProcessEnvironment(env);
    p.start(CryptoClient::workerPath(), {QStringLiteral("--print-env")});
    p.waitForFinished(10000);
    QStringList lines = QString::fromLocal8Bit(p.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString &l : lines)
        l = l.trimmed();
    return lines;
}

} // namespace

class TestCpuCrypto : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(!CryptoClient::workerPath().isEmpty(), "synbench-cpucrypto is not next to the test");
        // Planted for every test below: SynBench's own environment points
        // OpenSSL at a configuration file that doesn't exist (and would fail
        // the load if anything read it).
        qputenv("OPENSSL_CONF", "/nonexistent/synbench-planted.cnf");
        qputenv("OPENSSL_MODULES", "/nonexistent");
        qputenv("OPENSSL_ENGINES", "/nonexistent");
    }

    void selfTest()
    {
        const CryptoOutcome o = CryptoClient::selfTest([] { return false; });
        QVERIFY2(o.ok(), qPrintable(o.error));
        QCOMPARE(o.openssl, QStringLiteral("3.5.9"));
    }

    // P17: nothing of SynBench's environment reaches the worker.
    void environmentFromScratch()
    {
        const QProcessEnvironment hw = CryptoClient::environment(false);
        const QProcessEnvironment sw = CryptoClient::environment(true);
#if defined(Q_OS_WIN)
        QCOMPARE(hw.keys().size(), 2);
        QVERIFY(hw.contains(QStringLiteral("SystemRoot")));
        QVERIFY(hw.value(QStringLiteral("PATH")).endsWith(QLatin1StringView("\\System32")));
#else
        QVERIFY(hw.isEmpty());
#endif
        QCOMPARE(sw.keys().size(), hw.keys().size() + 1);
        QCOMPARE(sw.value(CryptoClient::maskVariable()), CryptoClient::maskValue());
        // What the worker really gets (Qt's QProcess adds nothing on Linux;
        // on Windows it would add PATH, which is set already).
        QStringList want = {CryptoClient::maskVariable() + QLatin1Char('=') + CryptoClient::maskValue()};
#if defined(Q_OS_WIN)
        want << QStringLiteral("SystemRoot=") + sw.value(QStringLiteral("SystemRoot"))
             << QStringLiteral("PATH=") + sw.value(QStringLiteral("PATH"));
#endif
        QStringList got = workerEnvironment(sw);
        std::sort(want.begin(), want.end(), [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
        std::sort(got.begin(), got.end(), [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
        QCOMPARE(got, want);
    }

    // The worker itself drops OpenSSL's variables when it inherits them (a
    // start that bypasses the client).
    void workerDropsOpensslVariables()
    {
        const QStringList got = workerEnvironment(QProcessEnvironment::systemEnvironment());
        QVERIFY(!got.isEmpty());
        for (const QString &l : got)
            QVERIFY2(!l.startsWith(QLatin1StringView("OPENSSL_")), qPrintable(l));
    }

    // §5.5: the hardware and the software path agree (two implementations),
    // with the golden checksum; the software one is the masked one, and where
    // the CPU has the instructions it is really slower ("software path may not
    // have been forced" otherwise).
    void hardwareAndSoftwareAgree_data()
    {
        QTest::addColumn<int>("alg");
        QTest::addRow("AES-256-GCM") << int(CryptoAlg::AesGcm);
        QTest::addRow("SHA-256") << int(CryptoAlg::Sha256);
    }
    void hardwareAndSoftwareAgree()
    {
        QFETCH(int, alg);
        const CryptoAlg a = CryptoAlg(alg);
        const CryptoOutcome hw = CryptoClient::run(quick(a, false), [] { return false; });
        QVERIFY2(hw.ok(), qPrintable(hw.error));
        const CryptoOutcome sw = CryptoClient::run(quick(a, true), [] { return false; });
        QVERIFY2(sw.ok(), qPrintable(sw.error));
        QCOMPARE(hw.checksum, CryptoClient::expectedChecksum(a));
        QCOMPARE(sw.checksum, hw.checksum);
        QVERIFY(!hw.masked);
        QVERIFY(sw.masked);
        QCOMPARE(int(hw.repMbps.size()), 2);
        qInfo("%s: hardware %.0f MB/s, software %.0f MB/s on 2 threads (features: %s)",
              a == CryptoAlg::AesGcm ? "AES-256-GCM" : "SHA-256", hw.medianMbps, sw.medianMbps, qPrintable(hw.features));
        const sb::isa::CpuFeatures f = sb::isa::detect();
        const bool instructions = a == CryptoAlg::AesGcm ? f.has(sb::isa::Feature::Aes) || f.has(sb::isa::Feature::ArmAes)
                                                         : f.has(sb::isa::Feature::Sha) || f.has(sb::isa::Feature::ArmSha2);
        if (instructions)
            QVERIFY2(sw.medianMbps < 0.9 * hw.medianMbps, "the software path may not have been forced");
    }

    // A worker error: Error with the worker's own last stderr line.
    void failureCarriesTheReason()
    {
        CryptoRunSpec s = quick(CryptoAlg::Sha256, false);
        s.threads = 0;  // the worker refuses it
        const CryptoOutcome o = CryptoClient::run(s, [] { return false; });
        QCOMPARE(o.status, CryptoOutcome::Status::Failed);
        QVERIFY2(o.error.startsWith(QLatin1StringView("synbench-cpucrypto: bad --threads")), qPrintable(o.error));
    }

    // AP-5 L6: a run's output must have the right shape, and a runaway line
    // is refused.
    void outputShape()
    {
        CryptoOutcome o;
        o.repMbps = {1, 2, 3};
        QVERIFY(CryptoClient::shapeProblem(o, true, 3, false).isEmpty());
        QVERIFY(CryptoClient::shapeProblem(o, false, 3, false).contains(QLatin1StringView("ready line: no")));
        QVERIFY(CryptoClient::shapeProblem(o, true, 5, false).contains(QLatin1StringView("3 of 5 repetitions")));
        QVERIFY(CryptoClient::shapeProblem(o, true, 3, true).contains(QLatin1StringView("didn't see the software-path mask")));
        o.masked = true;
        QVERIFY(CryptoClient::shapeProblem(o, true, 3, false).contains(QLatin1StringView("on the hardware path")));
        QVERIFY(CryptoClient::shapeProblem(o, true, 3, true).isEmpty());
        QVERIFY(!CryptoClient::lineTooLong(QByteArray(CryptoClient::kMaxOutputLine, 'x')));
        QVERIFY(CryptoClient::lineTooLong(QByteArray(CryptoClient::kMaxOutputLine + 1, 'x')));
        QVERIFY(!CryptoClient::lineTooLong(QByteArray(CryptoClient::kMaxOutputLine + 1, 'x') + '\n'));
    }

    // Stop kills the worker within 250 ms (a poll, then the kill), and no
    // process is left behind.
    void cancelKillsTheWorker()
    {
        CryptoRunSpec s = quick(CryptoAlg::AesGcm, false);
        s.windows = {60000ms, 60000ms, 1};
        Clock::time_point cancelAt{};
        bool started = false;
        const CryptoOutcome o = CryptoClient::run(
            s,
            [&] {
                if (started && cancelAt == Clock::time_point{})
                    cancelAt = Clock::now() + 300ms;
                return started && Clock::now() >= cancelAt;
            },
            [&](int) { started = true; });
        const auto took = Clock::now() - cancelAt;
        QCOMPARE(o.status, CryptoOutcome::Status::Cancelled);
        QVERIFY2(took < 250ms, qPrintable(QStringLiteral("Stop took %1 ms").arg(std::chrono::duration<double, std::milli>(took).count())));
        QVERIFY(o.pid > 0);
        QVERIFY2(!processAlive(o.pid), "the worker is still running");
    }

    void timeoutKillsTheWorker()
    {
        CryptoRunSpec s = quick(CryptoAlg::Sha256, false);
        s.windows = {60000ms, 60000ms, 1};
        s.timeout = 1500ms;
        const CryptoOutcome o = CryptoClient::run(s, [] { return false; });
        QCOMPARE(o.status, CryptoOutcome::Status::Failed);
        QVERIFY2(o.error.contains(QLatin1StringView("didn't finish within")), qPrintable(o.error));
        QVERIFY(!processAlive(o.pid));
    }

    // The worker exits by itself when its stdin closes (SynBench is gone).
    void stdinEofEndsTheWorker()
    {
        QProcess p;
        p.setProcessEnvironment(CryptoClient::environment(false));
        p.start(CryptoClient::workerPath(), {QStringLiteral("--alg"), QStringLiteral("sha-256"), QStringLiteral("--threads"),
                                             QStringLiteral("1"), QStringLiteral("--warmup-ms"), QStringLiteral("60000"),
                                             QStringLiteral("--rep-ms"), QStringLiteral("1000"), QStringLiteral("--reps"),
                                             QStringLiteral("1")});
        QVERIFY(p.waitForStarted(10000));
        QVERIFY(p.waitForReadyRead(10000));  // it is running ("ready")
        p.closeWriteChannel();
        QVERIFY(p.waitForFinished(5000));
        QCOMPARE(p.exitCode(), 3);
    }
};

QTEST_GUILESS_MAIN(TestCpuCrypto)
#include "tst_cpu_crypto.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/crypto_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <array>

namespace sb::cpu {

namespace golden {
#include "tests/cpu/golden/crypto.inc"
}

namespace {

constexpr int kPollMs = 50;  // §4.1

const char *algArg(CryptoAlg a)
{
    return a == CryptoAlg::AesGcm ? "aes-256-gcm" : "sha-256";
}

void killWorker(QProcess &p)
{
    p.kill();
    p.waitForFinished(2000);
}

QString lastLine(const QByteArray &text)
{
    const QList<QByteArray> lines = text.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray l = it->trimmed();
        if (!l.isEmpty())
            return QString::fromUtf8(l);
    }
    return {};
}

#if defined(SYNTHEON_DEV_HOOKS)
QString g_testProgram;
QStringList g_testArgs;
#endif

// What a measurement's output must show (nothing for the self-test).
struct Expect {
    CryptoAlg alg;
    bool software;
    int reps;
    int threads;
};

// Runs the worker with `args` until it exits, Stop or the timeout, then
// checks a measurement's output (`expect`): its shape (a ready line, `reps`
// rep lines, the mask seen exactly when asked for), then its checksum and
// rates (CryptoClient::resultProblem).
CryptoOutcome runWorker(QStringList args, const std::optional<Expect> &expect, std::chrono::milliseconds timeout,
                        const std::function<bool()> &cancelled, const std::function<void(int)> &window)
{
    CryptoOutcome out;
    const bool software = expect && expect->software;
    const QString program = CryptoClient::workerPath();
#if defined(SYNTHEON_DEV_HOOKS)
    if (!g_testProgram.isEmpty())
        args = g_testArgs + args;
#endif
    if (program.isEmpty()) {
        out.error = QStringLiteral("The encryption worker (syntheon-cpucrypto) is missing next to Syntheon");
        return out;
    }
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    // Its own folder, not wherever Syntheon was started from (AP-5 L3).
    p.setWorkingDirectory(QFileInfo(program).absolutePath());
    p.setProcessEnvironment(CryptoClient::environment(software));
    // stdin stays open (a pipe) while we run: the worker exits when it closes.
    p.start(QIODevice::ReadWrite);
    if (!p.waitForStarted(10000)) {
        out.error = QStringLiteral("The encryption worker didn't start: %1").arg(p.errorString());
        return out;
    }
    out.pid = p.processId();
    bool haveResult = false, haveReady = false, garbage = false;
    QByteArray pending;
    const auto handle = [&](const QByteArray &line) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QString ev = o.value(QLatin1StringView("ev")).toString();
        if (ev == QLatin1StringView("ready")) {
            haveReady = true;
            out.openssl = o.value(QLatin1StringView("openssl")).toString();
            out.features = o.value(QLatin1StringView("features")).toString();
            out.masked = o.value(QLatin1StringView("masked")).toBool();
        } else if (ev == QLatin1StringView("window")) {
            if (window)
                window(o.value(QLatin1StringView("i")).toInt());
        } else if (ev == QLatin1StringView("rep")) {
            out.repMbps.push_back(o.value(QLatin1StringView("mbps")).toDouble());
        } else if (ev == QLatin1StringView("result")) {
            out.medianMbps = o.value(QLatin1StringView("median_mbps")).toDouble();
            out.spread = o.value(QLatin1StringView("spread")).toDouble();
            bool ok = false;
            out.checksum = o.value(QLatin1StringView("checksum")).toString().mid(2).toULongLong(&ok, 16);
            haveResult = ok;
        } else if (ev == QLatin1StringView("self-test")) {
            out.openssl = o.value(QLatin1StringView("openssl")).toString();
            haveResult = o.value(QLatin1StringView("ok")).toBool();
        }
    };
    const auto drain = [&] {
        pending += p.readAllStandardOutput();
        qsizetype nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            handle(pending.left(nl));
            pending.remove(0, nl + 1);
        }
        // What is left is an unfinished line: no line of ours is near this
        // long (AP-5 L6), so don't buffer junk.
        if (CryptoClient::lineTooLong(pending))
            garbage = true;
    };
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        if (cancelled && cancelled()) {
            killWorker(p);
            out.status = CryptoOutcome::Status::Cancelled;
            return out;
        }
        if (garbage) {
            killWorker(p);
            out.error = QStringLiteral("The encryption worker wrote an output line longer than %1 bytes").arg(CryptoClient::kMaxOutputLine);
            return out;
        }
        if (Clock::now() >= deadline) {
            killWorker(p);
            out.error = QStringLiteral("The encryption worker didn't finish within %1 s and was stopped")
                            .arg(std::chrono::duration<double>(timeout).count(), 0, 'f', 0);
            return out;
        }
        p.waitForReadyRead(kPollMs);
        drain();
        if (p.state() == QProcess::NotRunning)
            break;
    }
    p.waitForFinished(1000);
    drain();
    // A last line without a newline still counts.
    if (!pending.isEmpty() && !garbage) {
        handle(pending);
        pending.clear();
    }
    if (p.exitStatus() == QProcess::CrashExit) {
#if defined(Q_OS_WIN)
        out.error = QStringLiteral("The encryption worker crashed (NTSTATUS 0x%1)")
                        .arg(quint32(p.exitCode()), 8, 16, QLatin1Char('0'));
#else
        out.error = QStringLiteral("The encryption worker crashed (signal %1)").arg(p.exitCode());
#endif
        if (const QString last = lastLine(p.readAllStandardError()); !last.isEmpty())
            out.error += QStringLiteral(": ") + last;
        return out;
    }
    if (p.exitCode() != 0) {
        const QString reason = lastLine(p.readAllStandardError());
        out.error = reason.isEmpty() ? QStringLiteral("The encryption worker exited with code %1").arg(p.exitCode())
                                     : reason;
        out.wrongResult = reason.contains(QLatin1StringView("wrong result"));
        return out;
    }
    if (!haveResult) {
        out.error = QStringLiteral("The encryption worker gave no result");
        return out;
    }
    if (expect) {
        QString problem = CryptoClient::shapeProblem(out, haveReady, expect->reps, software);
        if (problem.isEmpty())
            problem = CryptoClient::resultProblem(out, expect->alg, expect->threads);
        if (!problem.isEmpty()) {
            out.error = problem;
            out.wrongResult = out.checksum != CryptoClient::expectedChecksum(expect->alg);
            return out;
        }
    }
    out.status = CryptoOutcome::Status::Ok;
    return out;
}

} // namespace

QString CryptoClient::workerPath()
{
#if defined(SYNTHEON_DEV_HOOKS)
    if (!g_testProgram.isEmpty())
        return g_testProgram;
#endif
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("syntheon-cpucrypto.exe");
#else
    const QString name = QStringLiteral("syntheon-cpucrypto");
#endif
    const QDir dir(QCoreApplication::applicationDirPath());
    for (const QString &candidate : {dir.filePath(name), dir.filePath(QStringLiteral("../libexec/") + name)}) {
        const QFileInfo fi(candidate);
        if (fi.isFile() && fi.isExecutable())
            return fi.canonicalFilePath();
    }
    return {};
}

QString CryptoClient::maskVariable()
{
#if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("OPENSSL_armcap");
#else
    return QStringLiteral("OPENSSL_ia32cap");
#endif
}

QString CryptoClient::maskValue()
{
#if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("1");  // ARMV7_NEON only
#else
    return QStringLiteral("~0x200000200000000:~0x60020000000:~0x0:~0x0:~0x0");
#endif
}

QProcessEnvironment CryptoClient::environment(bool software)
{
    QProcessEnvironment env;  // empty: nothing inherited
#if defined(Q_OS_WIN)
    const QString root =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    env.insert(QStringLiteral("SystemRoot"), root);
    env.insert(QStringLiteral("PATH"), root + QStringLiteral("\\System32"));
#endif
    if (software)
        env.insert(maskVariable(), maskValue());
    return env;
}

std::chrono::milliseconds CryptoClient::timeoutFor(const CryptoRunSpec &spec)
{
    if (spec.timeout)
        return *spec.timeout;
    const auto expected = spec.windows.warmup + spec.windows.rep * spec.windows.reps;
    return 2 * expected + std::chrono::milliseconds(10000);
}

CryptoOutcome CryptoClient::run(const CryptoRunSpec &spec, const std::function<bool()> &cancelled,
                                const std::function<void(int)> &window)
{
    const QStringList args = {QStringLiteral("--alg"),       QString::fromLatin1(algArg(spec.alg)),
                              QStringLiteral("--threads"),   QString::number(spec.threads),
                              QStringLiteral("--warmup-ms"), QString::number(spec.windows.warmup.count()),
                              QStringLiteral("--rep-ms"),    QString::number(spec.windows.rep.count()),
                              QStringLiteral("--reps"),      QString::number(spec.windows.reps),
                              QStringLiteral("--seed"),      QString::number(spec.seed % kSeeds)};
    return runWorker(args, Expect{spec.alg, spec.software, spec.windows.reps, spec.threads}, timeoutFor(spec), cancelled,
                     window);
}

CryptoOutcome CryptoClient::selfTest(const std::function<bool()> &cancelled)
{
    return runWorker({QStringLiteral("--self-test")}, std::nullopt, std::chrono::milliseconds(30000), cancelled, {});
}

bool CryptoClient::lineTooLong(const QByteArray &pending)
{
    return pending.size() > kMaxOutputLine && pending.indexOf('\n') < 0;
}

QString CryptoClient::shapeProblem(const CryptoOutcome &o, bool sawReady, int reps, bool software)
{
    if (!sawReady || int(o.repMbps.size()) != reps)
        return QStringLiteral("The encryption worker's output was malformed (ready line: %1, %2 of %3 repetitions)")
            .arg(sawReady ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(o.repMbps.size())
            .arg(reps);
    if (o.masked != software)
        return software ? QStringLiteral("The encryption worker didn't see the software-path mask")
                        : QStringLiteral("The encryption worker saw an OpenSSL capability mask on the hardware path");
    return {};
}

double CryptoClient::ceilingMbps(CryptoAlg alg, int threads)
{
    return plausibilityCeiling(alg == CryptoAlg::AesGcm ? 16 : 8, threads) / 1e6;
}

QString CryptoClient::resultProblem(const CryptoOutcome &o, CryptoAlg alg, int threads)
{
    const std::uint64_t want = expectedChecksum(alg);
    if (o.checksum != want)
        return QStringLiteral("The encryption worker's checksum was 0x%1, not 0x%2: a wrong result")
            .arg(o.checksum, 16, 16, QLatin1Char('0'))
            .arg(want, 16, 16, QLatin1Char('0'));
    const double ceiling = ceilingMbps(alg, threads);
    std::vector<double> rates = o.repMbps;
    rates.push_back(o.medianMbps);
    for (const double r : rates) {
        if (!(r > 0 && r <= ceiling))  // also catches NaN
            return QStringLiteral("The encryption worker reported an implausible rate (%1 MB/s; at most %2 MB/s on "
                                  "%3 threads)")
                .arg(r, 0, 'g', 6)
                .arg(ceiling, 0, 'f', 0)
                .arg(threads);
    }
    return {};
}

#if defined(SYNTHEON_DEV_HOOKS)
void CryptoClient::setWorkerForTests(const QString &program, const QStringList &extraArgs)
{
    g_testProgram = program;
    g_testArgs = extraArgs;
}
#endif

std::uint64_t CryptoClient::expectedChecksum(CryptoAlg alg)
{
    std::uint64_t x = 0;
    for (const std::uint64_t v : alg == CryptoAlg::AesGcm ? golden::kAesGcm : golden::kSha256)
        x ^= v;
    return x;
}

} // namespace sb::cpu

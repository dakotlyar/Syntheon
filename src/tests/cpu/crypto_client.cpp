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

// Runs the worker with `args` until it exits, Stop or the timeout.
CryptoOutcome runWorker(const QStringList &args, bool software, std::chrono::milliseconds timeout,
                        const std::function<bool()> &cancelled, const std::function<void(int)> &window)
{
    CryptoOutcome out;
    const QString program = CryptoClient::workerPath();
    if (program.isEmpty()) {
        out.error = QStringLiteral("The encryption worker (synbench-cpucrypto) is missing next to SynBench");
        return out;
    }
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setProcessEnvironment(CryptoClient::environment(software));
    // stdin stays open (a pipe) while we run: the worker exits when it closes.
    p.start(QIODevice::ReadWrite);
    if (!p.waitForStarted(10000)) {
        out.error = QStringLiteral("The encryption worker didn't start: %1").arg(p.errorString());
        return out;
    }
    out.pid = p.processId();
    bool haveResult = false;
    QByteArray pending;
    const auto handle = [&](const QByteArray &line) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QString ev = o.value(QLatin1StringView("ev")).toString();
        if (ev == QLatin1StringView("ready")) {
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
    };
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        if (cancelled && cancelled()) {
            killWorker(p);
            out.status = CryptoOutcome::Status::Cancelled;
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
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        const QString reason = lastLine(p.readAllStandardError());
        out.error = reason.isEmpty() ? QStringLiteral("The encryption worker exited with code %1").arg(p.exitCode())
                                     : reason;
        return out;
    }
    if (!haveResult) {
        out.error = QStringLiteral("The encryption worker gave no result");
        return out;
    }
    out.status = CryptoOutcome::Status::Ok;
    return out;
}

} // namespace

QString CryptoClient::workerPath()
{
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("synbench-cpucrypto.exe");
#else
    const QString name = QStringLiteral("synbench-cpucrypto");
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
    return runWorker(args, spec.software, timeoutFor(spec), cancelled, window);
}

CryptoOutcome CryptoClient::selfTest(const std::function<bool()> &cancelled)
{
    return runWorker({QStringLiteral("--self-test")}, false, std::chrono::milliseconds(30000), cancelled, {});
}

std::uint64_t CryptoClient::expectedChecksum(CryptoAlg alg)
{
    std::uint64_t x = 0;
    for (const std::uint64_t v : alg == CryptoAlg::AesGcm ? golden::kAesGcm : golden::kSha256)
        x ^= v;
    return x;
}

} // namespace sb::cpu

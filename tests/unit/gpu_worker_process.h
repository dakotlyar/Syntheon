// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the GPU backend tests (Phase 4a task 10): the built synbench-gpu
// as a QProcess with its JSON-lines events, and the software Vulkan driver
// (lavapipe) the tests render on.

#include "tests/gpu/model/protocol.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QSysInfo>

#include <functional>
#include <variant>

// A missing software driver or X server skips a test locally, but fails it where
// the CI sets SYNBENCH_REQUIRE_SOFTWARE_GPU (Linux: lavapipe and xvfb are installed
// there, so a skip would hide a broken test).
#define SB_SKIP_OR_FAIL(msg)                                    \
    do {                                                        \
        if (qEnvironmentVariableIsSet("SYNBENCH_REQUIRE_SOFTWARE_GPU")) \
            QFAIL(msg);                                         \
        else                                                    \
            QSKIP(msg);                                         \
    } while (false)

namespace sb::gpu::testing {

namespace pr = sb::gpu::protocol;

// lavapipe's id as the worker's --gpu takes it (VK_VENDOR_ID_MESA, device 0,
// no PCI slot: makeLinuxGpuId gives "10005:0000#0").
inline const QString kLavapipeId = QStringLiteral("10005:0000#0");
inline constexpr std::uint32_t kLavapipeVendor = 0x10005;

// lavapipe's ICD manifest: Arch names it lvp_icd.json, Debian/Ubuntu
// lvp_icd.<arch>.json (x86_64, aarch64); never assume either. "" if none.
inline QString lavapipeIcd()
{
#if defined(Q_OS_LINUX)
    const QString arch = QSysInfo::currentCpuArchitecture() == QLatin1StringView("arm64") ? QStringLiteral("aarch64")
                                                                                          : QStringLiteral("x86_64");
    for (const char *dir : {"/usr/share/vulkan/icd.d", "/usr/local/share/vulkan/icd.d", "/etc/vulkan/icd.d"}) {
        for (const QString &name : {QStringLiteral("lvp_icd.json"), QStringLiteral("lvp_icd.%1.json").arg(arch)}) {
            const QString path = QDir(QString::fromLatin1(dir)).filePath(name);
            if (QFileInfo::exists(path))
                return path;
        }
    }
#endif
    return {};
}

// The worker's software environment: only lavapipe, no implicit layers (§4.4).
inline void addSoftwareVulkan(QProcessEnvironment &env)
{
    env.insert(QStringLiteral("VK_DRIVER_FILES"), lavapipeIcd());
    env.insert(QStringLiteral("VK_ICD_FILENAMES"), lavapipeIcd());  // old loaders (1.3.204) only know this name
    env.insert(QStringLiteral("VK_LOADER_LAYERS_DISABLE"), QStringLiteral("~implicit~"));
}

template <class T>
bool is(const pr::Event &e)
{
    return std::holds_alternative<T>(e);
}

// One worker process and the events it has sent so far.
class WorkerProcess
{
public:
    WorkerProcess(const QString &program, const QStringList &args, const QProcessEnvironment &env)
    {
        m_p.setProcessEnvironment(env);
        m_p.start(program, args, QIODevice::ReadWrite);
        m_started = m_p.waitForStarted(10000);
    }
    ~WorkerProcess()
    {
        if (m_p.state() != QProcess::NotRunning) {
            m_p.kill();
            m_p.waitForFinished(5000);
        }
    }
    bool started() const { return m_started; }
    const QList<pr::Event> &events() const { return m_events; }
    const QString &problem() const { return m_problem; }

    // Reads events until one matches (true), the process ends or the time is up.
    bool waitFor(const std::function<bool(const pr::Event &)> &match, int timeoutMs = 30000)
    {
        QElapsedTimer t;
        t.start();
        for (;;) {
            while (m_p.canReadLine()) {
                const QByteArray line = m_p.readLine().trimmed();
                if (line.isEmpty())
                    continue;
                QString err;
                const auto e = pr::parseLine(line, &err);
                if (!e) {
                    m_problem = QStringLiteral("malformed line '%1': %2").arg(QString::fromUtf8(line), err);
                    return false;
                }
                m_events.append(*e);
                if (match(*e))
                    return true;
            }
            if (t.elapsed() > timeoutMs) {
                m_problem = QStringLiteral("timed out; stderr: ") + stderrText();
                return false;
            }
            if (m_p.state() == QProcess::NotRunning && !m_p.canReadLine() && m_p.bytesAvailable() == 0) {
                m_problem = QStringLiteral("the worker ended (exit %1): %2").arg(m_p.exitCode()).arg(stderrText());
                return false;
            }
            m_p.waitForReadyRead(50);
        }
    }
    bool waitForIdle()
    {
        return waitFor([](const pr::Event &e) { return is<pr::IdleEvent>(e); });
    }
    // Waits for the exit (draining stdout into events); the exit code, or -1.
    int finish(int timeoutMs = 30000)
    {
        if (!m_p.waitForFinished(timeoutMs))
            return -1;
        waitFor([](const pr::Event &) { return false; }, 0);
        return m_p.exitStatus() == QProcess::NormalExit ? m_p.exitCode() : -1;
    }
    void send(const QByteArray &line)
    {
        m_p.write(line + '\n');
        m_p.waitForBytesWritten(1000);
    }
    QString stderrText()
    {
        m_stderr += QString::fromUtf8(m_p.readAllStandardError());
        return m_stderr;
    }

private:
    QProcess m_p;
    bool m_started = false;
    QList<pr::Event> m_events;
    QString m_problem, m_stderr;
};

// The events' error reasons, joined ("" when there were none).
inline QString errors(const QList<pr::Event> &events)
{
    QStringList out;
    for (const pr::Event &e : events) {
        if (is<pr::ErrorEvent>(e))
            out << std::get<pr::ErrorEvent>(e).reason;
    }
    return out.join(QStringLiteral("; "));
}

inline QList<pr::ResultEvent> results(const QList<pr::Event> &events)
{
    QList<pr::ResultEvent> out;
    for (const pr::Event &e : events) {
        if (is<pr::ResultEvent>(e))
            out << std::get<pr::ResultEvent>(e);
    }
    return out;
}

} // namespace sb::gpu::testing

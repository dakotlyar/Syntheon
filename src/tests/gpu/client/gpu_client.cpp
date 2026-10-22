// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/client/gpu_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <deque>

namespace sb::gpu {

namespace pr = protocol;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kPollMs = 20;  // P-STOP: Stop within 250 ms (shorter than the 50 ms of the spec: headroom on a 1-core runner)
constexpr qsizetype kMaxLine = pr::kMaxLine;

#if defined(SYNBENCH_DEV_HOOKS)
QString g_testProgram;
QStringList g_testArgs;
#endif

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

QString seconds(std::chrono::milliseconds ms)
{
    return QString::number(std::chrono::duration<double>(ms).count(), 'f', 0);
}

// One worker process and its output, line by line.
class Link
{
public:
    ~Link() { killNow(); }

    bool start(const QStringList &args, const QProcessEnvironment &env, QString *error)
    {
        const QString program = GpuClient::workerPath();
        if (program.isEmpty()) {
            *error = QStringLiteral("the GPU worker is missing from this installation");
            return false;
        }
        QStringList all = args;
#if defined(SYNBENCH_DEV_HOOKS)
        if (!g_testProgram.isEmpty())
            all = g_testArgs + args;
#endif
        m_p.setProgram(program);
        m_p.setArguments(all);
        m_p.setWorkingDirectory(QFileInfo(program).absolutePath());  // its own folder
        m_p.setProcessEnvironment(env);
        m_p.start(QIODevice::ReadWrite);  // stdin stays open: the worker exits when it closes
        if (!m_p.waitForStarted(10000)) {
            *error = QStringLiteral("the GPU worker didn't start: %1").arg(m_p.errorString());
            return false;
        }
        return true;
    }

    // Reads what's there (waiting up to kPollMs) into whole lines. False + *problem on an overlong line.
    bool pump(QString *problem)
    {
        m_p.waitForReadyRead(kPollMs);
        m_pending += m_p.readAllStandardOutput();
        qsizetype nl;
        while ((nl = m_pending.indexOf('\n')) >= 0) {
            const QByteArray line = m_pending.left(nl).trimmed();
            m_pending.remove(0, nl + 1);
            if (!line.isEmpty())
                m_lines.push_back(line);
        }
        if (m_pending.size() > kMaxLine) {
            *problem = QStringLiteral("a line longer than %1 bytes").arg(kMaxLine);
            return false;
        }
        return true;
    }
    bool nextLine(QByteArray *line)
    {
        if (m_lines.empty())
            return false;
        *line = m_lines.front();
        m_lines.pop_front();
        return true;
    }
    bool hasLines() const { return !m_lines.empty(); }
    bool running() const { return m_p.state() != QProcess::NotRunning; }
    // After the process ended: a last line without its newline still counts.
    void flushTail()
    {
        m_pending += m_p.readAllStandardOutput();
        if (!m_pending.trimmed().isEmpty())
            m_lines.push_back(m_pending.trimmed());
        m_pending.clear();
    }
    void write(const QByteArray &text)
    {
        if (running()) {
            m_p.write(text);
            m_p.waitForBytesWritten(100);
        }
    }
    // Stop: "stop", up to 100 ms for the exit, then kill (<= 250 ms in all).
    void stopThenKill()
    {
        write("stop\n");
        if (!m_p.waitForFinished(100))  // 100 ms, not 150: the same headroom
            killNow();
    }
    void killNow()
    {
        if (m_p.state() != QProcess::NotRunning) {
            m_p.kill();
            m_p.waitForFinished(2000);
        }
    }
    bool waitForExit(int ms) { return m_p.state() == QProcess::NotRunning || m_p.waitForFinished(ms); }
    int exitCode() const { return m_p.exitCode(); }  // after a crash: the signal (Linux) or NTSTATUS (Windows)
    bool crashed() const { return m_p.exitStatus() == QProcess::CrashExit; }
    qint64 pid() const { return m_pid; }
    void notePid() { m_pid = m_p.processId(); }
    QString stderrLine()
    {
        m_stderr += m_p.readAllStandardError();
        return lastLine(m_stderr);
    }

private:
    QProcess m_p;
    QByteArray m_pending, m_stderr;
    std::deque<QByteArray> m_lines;
    qint64 m_pid = 0;
};

enum class End { Done, Cancelled, TimedOut, Exited, Malformed };

// The one read loop: delivers events to `handle` (true = done) until it says
// done, Stop, the deadline, the process's end, or a malformed line.
End drive(Link &link, const std::function<bool()> &cancelled, Clock::time_point deadline,
          const std::function<bool(const pr::Event &)> &handle, QString *problem)
{
    for (;;) {
        QByteArray line;
        while (link.nextLine(&line)) {
            QString err;
            const auto e = pr::parseLine(line, &err);
            if (!e) {
                *problem = err;
                return End::Malformed;
            }
            if (handle(*e))
                return End::Done;
        }
        if (!link.running()) {
            link.flushTail();
            if (link.hasLines())
                continue;
            return End::Exited;
        }
        if (cancelled && cancelled())
            return End::Cancelled;
        if (Clock::now() >= deadline)
            return End::TimedOut;
        QString tooLong;
        if (!link.pump(&tooLong)) {
            *problem = tooLong;
            return End::Malformed;
        }
    }
}

// What an exit means for a worker that ended on its own.
void applyExit(Link &link, const QString &lastError, bool lastErrorNa, ClientOutcome *out)
{
    out->exitCode = link.exitCode();
    const QString stderrLine = link.stderrLine();
    const int code = out->exitCode;
    if (!link.crashed() && code == 0) {
        out->status = ClientOutcome::Status::Ok;
    } else if (!link.crashed() && code == 4) {
        out->status = ClientOutcome::Status::Cancelled;
        out->reason = lastError;
    } else if (!link.crashed() && code == 5) {
        out->status = ClientOutcome::Status::NotAvailable;
        out->reason = !lastError.isEmpty() ? lastError : stderrLine;
    } else if (!link.crashed() && code == 1 && !lastError.isEmpty() && !lastErrorNa) {
        out->status = ClientOutcome::Status::Failed;
        out->reason = lastError;  // the worker's own reason
    } else if (!link.crashed() && (code == 2 || code == 3) && !lastError.isEmpty() && !lastErrorNa) {
        // A protocol or argument error the worker explained ("protocol: ...") before it exited (L5).
        out->status = ClientOutcome::Status::Failed;
        out->reason = lastError;
    } else {
        out->status = ClientOutcome::Status::Failed;
        out->reason = QStringLiteral("the GPU worker exited with code %1%2").arg(code).arg(stderrLine.isEmpty() ? QString() : QStringLiteral(": ") + stderrLine);
    }
}

ClientOutcome endOutcome(End end, const QString &problem, Link &link, std::chrono::milliseconds timeout, ClientOutcome out)
{
    switch (end) {
    case End::Cancelled:
        link.stopThenKill();
        out.status = ClientOutcome::Status::Cancelled;
        break;
    case End::TimedOut:
        link.killNow();
        out.status = ClientOutcome::Status::Failed;
        out.reason = QStringLiteral("the GPU worker stopped responding (killed after %1 s)").arg(seconds(timeout));
        break;
    case End::Malformed:
        link.killNow();
        out.status = ClientOutcome::Status::Failed;
        out.reason = QStringLiteral("the GPU worker sent a malformed line: %1").arg(problem);
        break;
    default:
        break;
    }
    return out;
}

} // namespace

// --------------------------------------------------------------- GpuClient

QString GpuClient::workerPath()
{
#if defined(SYNBENCH_DEV_HOOKS)
    if (!g_testProgram.isEmpty())
        return g_testProgram;
#endif
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("synbench-gpu.exe");
#else
    const QString name = QStringLiteral("synbench-gpu");
#endif
    const QDir dir(QCoreApplication::applicationDirPath());
    for (const QString &candidate : {dir.filePath(name), dir.filePath(QStringLiteral("../libexec/") + name)}) {
        const QFileInfo fi(candidate);
        if (fi.isFile() && fi.isExecutable())
            return fi.canonicalFilePath();
    }
    return {};
}

QProcessEnvironment GpuClient::baseEnvironment()
{
    const QProcessEnvironment parent = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment env;
#if defined(Q_OS_WIN)
    // The system variables a Windows process needs to start at all (a worker without PATH or
    // SystemRoot dies with STATUS_DLL_NOT_FOUND before main()), the profile folders the drivers'
    // shader caches use, and the machine's description. Qt's own variables (QT_PLUGIN_PATH and the
    // like) are not among them: those are cleared.
    for (const char *name : {"SystemRoot", "windir", "SystemDrive", "TEMP", "TMP", "PATHEXT", "COMSPEC", "LOCALAPPDATA", "APPDATA",
                             "USERPROFILE", "PROGRAMDATA", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "CommonProgramFiles",
                             "CommonProgramFiles(x86)", "CommonProgramW6432", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE"}) {
        if (parent.contains(QLatin1StringView(name)))
            env.insert(QString::fromLatin1(name), parent.value(QLatin1StringView(name)));
    }
    // PATH: the worker's own folder and System32 first, then SynBench's PATH (a dev or test build
    // finds the Qt DLLs there; a packaged one has them next to the exe and loads from System32 and
    // its folder only, /DEPENDENTLOADFLAG:0xA00).
    const QString root = parent.value(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    const QString workerFolder = QFileInfo(workerPath()).absolutePath();
    QString path;
    if (!workerFolder.isEmpty())
        path += QDir::toNativeSeparators(workerFolder) + QLatin1Char(';');
    path += root + QStringLiteral("\\System32");
    if (const QString inherited = parent.value(QStringLiteral("PATH")); !inherited.isEmpty())
        path += QLatin1Char(';') + inherited;
    env.insert(QStringLiteral("PATH"), path);
#else
    for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
                             "DBUS_SESSION_BUS_ADDRESS", "HOME", "LANG"}) {
        if (parent.contains(QLatin1StringView(name)))
            env.insert(QString::fromLatin1(name), parent.value(QLatin1StringView(name)));
    }
    // §4.4: overlays and recorders (implicit layers) stay out of the measurement.
    env.insert(QStringLiteral("VK_LOADER_LAYERS_DISABLE"), QStringLiteral("~implicit~"));
    // DEF-1 (Zeb, checkpoint 3), every API: Qt Wayland turns a window "inexposed" when a
    // frame callback is later than 100 ms, i.e. on any frame slower than 10 fps (the
    // 610M's OpenGL Shading at ~8 fps); 60 s is far beyond any frame the test accepts.
    env.insert(QStringLiteral("QT_WAYLAND_FRAME_CALLBACK_TIMEOUT"), QStringLiteral("60000"));
#endif
#if defined(SYNBENCH_DEV_HOOKS)
    for (const QString &pair : parent.value(QStringLiteral("SYNBENCH_GPU_WORKER_ENV")).split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
        const qsizetype eq = pair.indexOf(QLatin1Char('='));
        if (eq > 0)
            env.insert(pair.left(eq), pair.mid(eq + 1));
    }
#endif
    return env;
}

QStringList GpuClient::extraArgs()
{
#if defined(SYNBENCH_DEV_HOOKS)
    return qEnvironmentVariable("SYNBENCH_GPU_WORKER_ARGS").split(QLatin1Char(' '), Qt::SkipEmptyParts);
#else
    return {};
#endif
}

std::chrono::milliseconds GpuClient::timeoutFor(const QVector<pr::PlanItem> &plan)
{
    std::chrono::milliseconds total{0};
    for (const pr::PlanItem &i : plan)
        total += std::chrono::milliseconds(i.warmupMs + i.measureMs);
    return 2 * total + std::chrono::seconds(30);
}

std::chrono::milliseconds GpuClient::timeoutFor(const pr::PlanItem &item)
{
    return timeoutFor(QVector<pr::PlanItem>{item});
}

ClientOutcome GpuClient::run(const ClientRunSpec &spec, const std::function<bool()> &cancelled,
                             const std::function<void(const pr::Event &)> &onEvent)
{
    ClientOutcome out;
    QStringList args = QStringList{QStringLiteral("--run"), QStringLiteral("--api"), spec.api, QStringLiteral("--gpu"), spec.gpuId,
                                   QStringLiteral("--plan"), pr::planText(spec.plan)};
    if (!spec.luid.isEmpty())
        args << QStringLiteral("--luid") << spec.luid;
    args += extraArgs();
    QProcessEnvironment env = baseEnvironment();
    env.insert(spec.extraEnv);
    Link link;
    if (!link.start(args, env, &out.reason))
        return out;
    link.notePid();
    out.pid = link.pid();
    const auto timeout = spec.timeout ? *spec.timeout : timeoutFor(spec.plan);
    QString lastError, problem;
    bool lastNa = false;
    const End end = drive(link, cancelled, Clock::now() + timeout, [&](const pr::Event &e) {
        if (onEvent)
            onEvent(e);
        if (const auto *r = std::get_if<pr::ReadyEvent>(&e)) {
            out.ready = *r;
        } else if (const auto *res = std::get_if<pr::ResultEvent>(&e)) {
            out.results.append(*res);
        } else if (const auto *err = std::get_if<pr::ErrorEvent>(&e)) {
            lastError = err->reason;
            lastNa = err->na;
            if (!err->workload.isEmpty())
                out.workloadErrors.insert(err->workload, err->reason);
        } else if (const auto *warn = std::get_if<pr::WarningEvent>(&e)) {
            out.warnings.append(warn->text);
        }
        return false;
    }, &problem);
    if (end == End::Exited)
        applyExit(link, lastError, lastNa, &out);
    else
        out = endOutcome(end, problem, link, timeout, out);
    return out;
}

QVector<GpuEntry> GpuClient::probe(std::chrono::milliseconds timeout, QString *error, bool *timedOut)
{
    if (timedOut)
        *timedOut = false;
    QVector<GpuEntry> gpus;
    Link link;
    if (!link.start(QStringList{QStringLiteral("--probe")} + extraArgs(), baseEnvironment(), error))
        return gpus;
    QString lastError, problem;
    bool done = false;
    const End end = drive(link, {}, Clock::now() + timeout, [&](const pr::Event &e) {
        if (const auto *g = std::get_if<pr::GpuEvent>(&e))
            gpus.append(g->gpu);
        else if (const auto *err = std::get_if<pr::ErrorEvent>(&e))
            lastError = err->reason;
        else if (std::holds_alternative<pr::DoneEvent>(e))
            done = true;
        return false;
    }, &problem);
    if (end == End::TimedOut) {
        if (timedOut)
            *timedOut = true;
        link.killNow();
        *error = QStringLiteral("the GPU probe didn't finish within %1 s").arg(seconds(timeout));
        gpus.clear();
    } else if (end == End::Malformed) {
        link.killNow();
        *error = QStringLiteral("the GPU worker sent a malformed line: %1").arg(problem);
        gpus.clear();
    } else if (!done) {
        *error = !lastError.isEmpty() ? lastError : QStringLiteral("the GPU probe failed");
        gpus.clear();
    }
    return gpus;
}

std::optional<pr::GlEvent> GpuClient::probeGl(const GpuEntry &gpu, const QProcessEnvironment &glEnv, std::chrono::milliseconds timeout)
{
    Link link;
    QProcessEnvironment env = baseEnvironment();
    env.insert(glEnv);
    QString error;
    if (!link.start(QStringList{QStringLiteral("--probe-gl"), QStringLiteral("--gpu"), gpu.id} + extraArgs(), env, &error))
        return std::nullopt;
    QString problem;
    std::optional<pr::GlEvent> gl;
    const End end = drive(link, {}, Clock::now() + timeout, [&](const pr::Event &e) {
        if (const auto *g = std::get_if<pr::GlEvent>(&e))
            gl = *g;
        return gl.has_value();
    }, &problem);
    if (end == End::TimedOut || end == End::Malformed || !gl)
        link.killNow();
    return gl;
}

#if defined(SYNBENCH_DEV_HOOKS)
void GpuClient::setWorkerForTests(const QString &program, const QStringList &extraArgs)
{
    g_testProgram = program;
    g_testArgs = extraArgs;
}
#endif

// -------------------------------------------------------------- GpuSession

struct GpuSession::Impl {
    Link link;
    bool started = false;
};

GpuSession::GpuSession() : m_d(std::make_unique<Impl>()) {}

GpuSession::~GpuSession()
{
    kill();
}

bool GpuSession::start(const QString &api, const QString &gpuId, const QProcessEnvironment &extraEnv, QString *error, const QString &luid)
{
    QStringList args = QStringList{QStringLiteral("--run"), QStringLiteral("--serve"), QStringLiteral("--api"), api, QStringLiteral("--gpu"), gpuId};
    if (!luid.isEmpty())
        args << QStringLiteral("--luid") << luid;
    args += GpuClient::extraArgs();
    QProcessEnvironment env = GpuClient::baseEnvironment();
    env.insert(extraEnv);
    if (!m_d->link.start(args, env, error))
        return false;
    m_d->link.notePid();
    m_d->started = true;
    return true;
}

ClientOutcome GpuSession::waitReady(std::chrono::milliseconds timeout, const std::function<bool()> &cancelled)
{
    ClientOutcome out;
    out.pid = m_d->link.pid();
    QString lastError, problem;
    bool lastNa = false;
    const End end = drive(m_d->link, cancelled, Clock::now() + timeout, [&](const pr::Event &e) {
        if (const auto *r = std::get_if<pr::ReadyEvent>(&e)) {
            out.ready = *r;
            return true;
        }
        if (const auto *err = std::get_if<pr::ErrorEvent>(&e)) {
            lastError = err->reason;
            lastNa = err->na;
        }
        return false;
    }, &problem);
    if (end == End::Done) {
        out.status = ClientOutcome::Status::Ok;
        return out;
    }
    if (end == End::Exited) {
        applyExit(m_d->link, lastError, lastNa, &out);
        if (out.status == ClientOutcome::Status::Ok)  // exiting with 0 before `ready` is no readiness
            out.status = ClientOutcome::Status::Failed, out.reason = QStringLiteral("the GPU worker exited before it was ready");
        return out;
    }
    return endOutcome(end, problem, m_d->link, timeout, out);
}

ClientOutcome GpuSession::runWorkload(const pr::PlanItem &item, const std::function<bool()> &cancelled,
                                      const std::function<void(const pr::Event &)> &onEvent)
{
    ClientOutcome out;
    out.pid = m_d->link.pid();
    if (!m_d->link.running()) {
        applyExit(m_d->link, {}, false, &out);
        if (out.status == ClientOutcome::Status::Ok)
            out.status = ClientOutcome::Status::Failed, out.reason = QStringLiteral("the GPU worker has exited");
        return out;
    }
    m_d->link.write(QStringLiteral("run %1:%2:%3\n").arg(item.workload).arg(item.warmupMs).arg(item.measureMs).toUtf8());
    const auto timeout = GpuClient::timeoutFor(item);
    QString lastError, problem;
    bool lastNa = false;
    const End end = drive(m_d->link, cancelled, Clock::now() + timeout, [&](const pr::Event &e) {
        if (onEvent)
            onEvent(e);
        if (const auto *res = std::get_if<pr::ResultEvent>(&e)) {
            out.results.append(*res);
        } else if (const auto *err = std::get_if<pr::ErrorEvent>(&e)) {
            lastError = err->reason;
            lastNa = err->na;
            out.workloadErrors.insert(err->workload.isEmpty() ? item.workload : err->workload, err->reason);
        }
        return std::holds_alternative<pr::IdleEvent>(e);
    }, &problem);
    if (end == End::Done) {
        if (!out.results.isEmpty()) {
            out.status = ClientOutcome::Status::Ok;
        } else if (!lastError.isEmpty()) {
            out.status = lastNa ? ClientOutcome::Status::NotAvailable : ClientOutcome::Status::Failed;
            out.reason = lastError;
        } else {
            out.status = ClientOutcome::Status::Failed;
            out.reason = QStringLiteral("the GPU worker finished %1 without a result").arg(item.workload);
        }
        return out;
    }
    if (end == End::Exited) {
        applyExit(m_d->link, lastError, lastNa, &out);
        if (out.status == ClientOutcome::Status::Ok)
            out.status = ClientOutcome::Status::Failed, out.reason = QStringLiteral("the GPU worker exited with code 0 during %1").arg(item.workload);
        return out;
    }
    return endOutcome(end, problem, m_d->link, timeout, out);
}

void GpuSession::quit()
{
    if (!m_d->link.running())
        return;
    m_d->link.write("quit\n");
    if (!m_d->link.waitForExit(2000))
        m_d->link.killNow();
}

void GpuSession::kill()
{
    m_d->link.killNow();
}

bool GpuSession::alive() const
{
    return m_d->link.running();
}

qint64 GpuSession::pid() const
{
    return m_d->link.pid();
}

} // namespace sb::gpu

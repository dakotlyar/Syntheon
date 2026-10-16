// SPDX-License-Identifier: GPL-3.0-or-later
#include "privileged/linux_client/linux_helper_client.h"

#include "synbench_version.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

Q_LOGGING_CATEGORY(lcHelper, "synbench.helper")

namespace sb {

using namespace sb::proto;

struct LinuxHelperClient::Pending {
    QWaitCondition done;
    bool finished = false;
    std::optional<Frame> reply;
    PrivError error;
};

QStringList LinuxHelperClient::defaultPkexecDirs()
{
    return {QStringLiteral("/usr/bin"), QStringLiteral("/bin"), QStringLiteral("/usr/sbin"), QStringLiteral("/sbin")};
}

LinuxHelperClient::Environment LinuxHelperClient::Environment::current()
{
    Environment e;
    e.appImage = qEnvironmentVariable("APPIMAGE");
    e.appDir = qEnvironmentVariable("APPDIR");
    e.applicationFilePath = QCoreApplication::applicationFilePath();
    return e;
}

namespace {

// mountinfo escapes space, tab, newline and backslash as \040 style octal.
QString unescapeMountField(const QString &s)
{
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('\\') && i + 3 < s.size() + 0 && s.mid(i + 1, 3).size() == 3) {
            bool ok = false;
            const int v = s.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out += QChar(v);
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Is `appDir` really the mount point of an AppImage's FUSE mount, for this image?
// Trusts the kernel's mount table, not $APPDIR/$APPIMAGE. The mount's source
// of the type-2 runtime (checked against a real AppImage, 2026-10-16) is the
// image's file NAME as it was launched, not a full path, and the fs type is
// "fuse.<file name>": both must match the image's name. An absolute source
// (another runtime) must be the image itself.
bool appImageMountIsReal(const QString &mountInfoPath, const QString &appDir, const QString &imagePath)
{
    QFile f(mountInfoPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        const int dash = line.indexOf(QStringLiteral(" - "));
        if (dash < 0)
            continue;
        const QStringList pre = line.left(dash).split(QLatin1Char(' '));
        const QStringList post = line.mid(dash + 3).split(QLatin1Char(' '));
        if (pre.size() < 5 || post.size() < 2)
            continue;
        if (QDir::cleanPath(unescapeMountField(pre[4])) != appDir)
            continue;
        // The kernel octal-escapes space, tab, newline, '#' and '\\' in EVERY field,
        // the file system type included ("fuse.My\040App.AppImage").
        const QString fsType = unescapeMountField(post[0]);
        if (!fsType.startsWith(QLatin1StringView("fuse")))
            continue;
        const QString source = unescapeMountField(post[1]);
        if (QFileInfo(source).isAbsolute())
            return QFileInfo(source).canonicalFilePath() == imagePath;
        const QString name = QFileInfo(imagePath).fileName();
        // `source` is relative to where the AppImage was started; only its file name is known.
        return QFileInfo(source).fileName() == name && fsType == QStringLiteral("fuse.") + name;
    }
    return false;
}

} // namespace

std::optional<LinuxHelperClient::Launch> LinuxHelperClient::defaultLaunch(const Environment &env, QString *error)
{
    // pkexec from a fixed system directory only, never through $PATH.
    QString pkexec;
    for (const QString &dir : env.pkexecDirs) {
        const QFileInfo fi(QDir(dir).filePath(QStringLiteral("pkexec")));
        if (fi.isFile() && fi.isExecutable()) {
            pkexec = fi.absoluteFilePath();
            break;
        }
    }
    if (pkexec.isEmpty()) {
        if (error)
            *error = QStringLiteral("pkexec (polkit) isn't installed.");
        return std::nullopt;
    }

    const QString binary = QFileInfo(env.applicationFilePath).canonicalFilePath();

    // Dev build / no AppImage: the helper next to the app binary, canonical.
    const auto devLaunch = [&]() -> std::optional<Launch> {
        const QString helper =
            QFileInfo(QFileInfo(binary).dir().filePath(QStringLiteral("synbench-helper"))).canonicalFilePath();
        if (helper.isEmpty() || !QFileInfo(helper).isExecutable()) {
            if (error)
                *error = QStringLiteral("The privileged helper (synbench-helper) is missing next to SynBench.");
            return std::nullopt;
        }
        return Launch{{pkexec, {helper}}, std::nullopt};
    };

    // AppImage: only if this binary really runs from inside the mounted image.
    // Root then re-executes the AppImage file itself (root normally can't enter
    // the user's FUSE mount).
    if (!env.appImage.isEmpty() && !env.appDir.isEmpty()) {
        const QString appDir = QFileInfo(env.appDir).canonicalFilePath();
        const QFileInfo image(env.appImage);
        if (!appDir.isEmpty() && !binary.isEmpty() && binary.startsWith(appDir + QLatin1Char('/'))
            && image.isAbsolute() && image.isFile() && image.isExecutable()) {
            const QString imagePath = image.canonicalFilePath();
            if (!appImageMountIsReal(env.mountInfo, appDir, imagePath)) {
                qCWarning(lcHelper) << "$APPDIR isn't a FUSE mount of $APPIMAGE; ignoring both";
                return devLaunch();
            }
            // (b) The fallback is a second pkexec, so a second polkit prompt:
            // accepted, as it only happens when the first re-exec failed.
            return Launch{{pkexec, {imagePath, QStringLiteral("--synbench-helper")}},
                          Command{pkexec, {imagePath, QStringLiteral("--appimage-extract-and-run"),
                                           QStringLiteral("--synbench-helper")}}};
        }
    }

    return devLaunch();
}

QString LinuxHelperClient::thisBuildId()
{
    return QStringLiteral(SYNBENCH_DISPLAY_VERSION " " SYNBENCH_BUILD_ID);
}

LinuxHelperClient::LinuxHelperClient(Options options, QObject *parent) : QObject(parent), m_options(std::move(options))
{
    m_authTimer.setSingleShot(true);
    connect(&m_authTimer, &QTimer::timeout, this, &LinuxHelperClient::onAuthTimeout);
}

LinuxHelperClient::~LinuxHelperClient()
{
    if (m_process) {
        m_process->disconnect(this);
        stop();
        // A root helper can't be killed from here; EOF makes it exit.
        m_process->waitForFinished(2000);
    }
    failAllPending(QStringLiteral("SynBench is closing"));
}

void LinuxHelperClient::setState(State s)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_state == s)
            return;
        m_state = s;
    }
    emit stateChanged(s);
}

qint64 LinuxHelperClient::helperProcessId() const
{
    return m_process ? m_process->processId() : 0;
}

LinuxHelperClient::State LinuxHelperClient::state() const
{
    QMutexLocker lock(&m_mutex);
    return m_state;
}

LinuxHelperClient::Failure LinuxHelperClient::failure() const
{
    QMutexLocker lock(&m_mutex);
    return m_failure;
}

QString LinuxHelperClient::failureMessage() const
{
    QMutexLocker lock(&m_mutex);
    return m_failureMessage;
}

bool LinuxHelperClient::isAvailable() const
{
    return state() == State::Ready;
}

void LinuxHelperClient::start()
{
    QString error;
    const auto launchSpec = defaultLaunch(Environment::current(), &error);
    if (!launchSpec) {
        {
            QMutexLocker lock(&m_mutex);
            m_failure = Failure::None;
        }
        fail(Failure::LaunchFailed, error);
        return;
    }
    start(*launchSpec);
}

void LinuxHelperClient::start(const Launch &launchSpec)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_fallback = launchSpec.fallback;
    {
        QMutexLocker lock(&m_mutex);
        m_failure = Failure::None;
        m_failureMessage.clear();
    }
    launch(launchSpec.primary);
}

void LinuxHelperClient::launch(const Command &command)
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->closeWriteChannel();
        m_process->deleteLater();
    }
    m_decoder = Decoder(kMaxResponseFrame);
    m_stderr.clear();
    m_helperStarted = false;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &LinuxHelperClient::onReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, &LinuxHelperClient::collectStderr);
    connect(m_process, &QProcess::finished, this, &LinuxHelperClient::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, &LinuxHelperClient::onErrorOccurred);
    setState(State::Authorising);
    m_authTimer.start(m_options.authTimeout);
    qCInfo(lcHelper) << "starting" << command.program << command.arguments;
    // argv, no shell (P11).
    m_process->start(command.program, command.arguments, QIODevice::ReadWrite);
}

void LinuxHelperClient::collectStderr()
{
    const QByteArray data = m_process->readAllStandardError();
    for (const QByteArray &line : data.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        qCInfo(lcHelper, "%s", line.constData());
        if (line.startsWith("synbench-helper: started"))
            m_helperStarted = true;
    }
    m_stderr = (m_stderr + data).right(4096);
}

// (e) pkexec exits 126 when the prompt was dismissed and 127 when
// authorisation failed, but the program it runs can exit 126/127 too (the
// AppImage when it can't exec, for instance). If the helper announced itself
// the code is the helper's; otherwise pkexec's own words decide, and the code
// only decides when pkexec said nothing.
LinuxHelperClient::Failure LinuxHelperClient::classifyEarlyExit(int exitCode, QString *message) const
{
    const QString text = QString::fromLocal8Bit(m_stderr).trimmed();
    const QString lower = text.toLower();
    if (!m_helperStarted && (exitCode == 126 || exitCode == 127)) {
        if (lower.contains(QLatin1StringView("dismissed")) || (text.isEmpty() && exitCode == 126)) {
            *message = QStringLiteral("The administrator prompt was cancelled.");
            return Failure::Cancelled;
        }
        if (lower.contains(QLatin1StringView("not authorized")) || lower.contains(QLatin1StringView("not authorised"))
            || lower.contains(QLatin1StringView("authentication")) || text.isEmpty()) {
            *message = QStringLiteral("Authorisation failed, or no authentication agent is running.");
            return Failure::NotAuthorised;
        }
    }
    return Failure::Exited;
}

void LinuxHelperClient::onAuthTimeout()
{
    if (state() != State::Authorising || !m_process)
        return;
    qCWarning(lcHelper) << "the administrator prompt timed out; stopping pkexec";
    QProcess *p = m_process;
    p->disconnect(this);
    p->terminate();
    // pkexec closes the polkit prompt when it goes away; escalate if it lingers.
    QTimer::singleShot(3000, p, [p] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
    });
    m_fallback.reset();
    fail(Failure::TimedOut, QStringLiteral("The administrator prompt timed out."));
}

void LinuxHelperClient::stop()
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->closeWriteChannel();
}

void LinuxHelperClient::shutdown()
{
    if (m_process)
        m_process->disconnect(this);
    {
        QMutexLocker lock(&m_mutex);
        m_state = State::Failed;
        m_failure = Failure::Exited;
        m_failureMessage = QStringLiteral("SynBench is closing");
    }
    failAllPending(QStringLiteral("SynBench is closing"));
    stop();
}

void LinuxHelperClient::fail(Failure failure, const QString &message)
{
    m_authTimer.stop();
    State previous;
    {
        // AP5-R2-5: Failed is set under the same lock request() checks, so no
        // request can register after failAllPending() and wait out its
        // timeout.
        QMutexLocker lock(&m_mutex);
        previous = m_state;
        m_state = State::Failed;
        m_failure = failure;
        m_failureMessage = message;
    }
    qCWarning(lcHelper) << "helper failed:" << message;
    failAllPending(message);
    stop();
    if (previous != State::Failed)
        emit stateChanged(State::Failed);
    emit failed(failure, message);
}

void LinuxHelperClient::failAllPending(const QString &message)
{
    QHash<quint32, std::shared_ptr<Pending>> pending;
    {
        QMutexLocker lock(&m_mutex);
        pending.swap(m_pending);
        for (const auto &p : std::as_const(pending)) {
            p->finished = true;
            p->error = {PrivError::Code::Unavailable, message};
            p->done.wakeAll();
        }
    }
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        emit requestFinished(it.key());
}

void LinuxHelperClient::onErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart)
        fail(Failure::LaunchFailed, QStringLiteral("Couldn't start %1: %2").arg(m_process->program(), m_process->errorString()));
}

void LinuxHelperClient::onFinished(int exitCode, QProcess::ExitStatus status)
{
    const State s = state();
    if (s == State::Failed)
        return;
    collectStderr();
    if (s == State::Authorising) {
        QString classified;
        if (status == QProcess::NormalExit) {
            const Failure f = classifyEarlyExit(exitCode, &classified);
            if (f != Failure::Exited) {
                fail(f, classified);
                return;
            }
        }
        // The AppImage couldn't be re-run as root (FUSE): one automatic retry
        // with --appimage-extract-and-run.
        if (m_fallback) {
            const Command fallback = *m_fallback;
            m_fallback.reset();
            qCInfo(lcHelper) << "helper exited with" << exitCode << "before HELLO; retrying with extract-and-run";
            launch(fallback);
            return;
        }
    }
    const QString how = status == QProcess::CrashExit ? QStringLiteral("unexpectedly")
                                                      : QStringLiteral("(exit code %1)").arg(exitCode);
    fail(Failure::Exited, s == State::Ready ? QStringLiteral("The privileged helper stopped %1.").arg(how)
                                            : QStringLiteral("The privileged helper exited before it started %1.").arg(how));
}

void LinuxHelperClient::onReadyRead()
{
    const QByteArray data = m_process->readAllStandardOutput();
    m_decoder.feed(reinterpret_cast<const std::uint8_t *>(data.constData()), std::size_t(data.size()));
    Frame frame;
    for (;;) {
        const auto status = m_decoder.next(frame);
        if (status == Decoder::Status::NeedMore)
            return;
        if (status == Decoder::Status::Oversized) {
            fail(Failure::BadHello, QStringLiteral("The privileged helper sent an oversized message."));
            return;
        }
        if (state() == State::Authorising) {
            handleHello(frame);
            if (state() != State::Ready)
                return;
            continue;
        }
        std::shared_ptr<Pending> p;
        {
            QMutexLocker lock(&m_mutex);
            p = m_pending.take(frame.requestId);
            if (p) {
                p->finished = true;
                p->reply = frame;
                p->done.wakeAll();
            }
        }
        if (p)
            emit requestFinished(frame.requestId);
        // else: a late answer to a request that timed out; ignore it.
    }
}

void LinuxHelperClient::handleHello(const Frame &frame)
{
    if (frame.op != std::uint16_t(Op::Hello) || !frame.isResponse() || frame.isError()) {
        fail(Failure::BadHello, QStringLiteral("The privileged helper didn't introduce itself."));
        return;
    }
    const auto hello = decodeHello(frame.payload);
    if (!hello || hello->protocolVersion != kProtocolVersion || frame.version != kProtocolVersion) {
        fail(Failure::BadHello, QStringLiteral("The privileged helper speaks a different protocol."));
        return;
    }
    const QString buildId = QString::fromStdString(hello->buildId);
    if (!m_options.expectedBuildId.isEmpty() && buildId != m_options.expectedBuildId) {
        fail(Failure::BuildMismatch,
             QStringLiteral("The privileged helper is from a different SynBench build (%1).").arg(buildId));
        return;
    }
    if (m_options.requireRoot && hello->euid != 0) {
        fail(Failure::NotRoot, QStringLiteral("The privileged helper isn't running as root."));
        return;
    }
    m_fallback.reset();
    m_authTimer.stop();
    qCInfo(lcHelper) << "helper ready:" << buildId << "euid" << hello->euid;
    setState(State::Ready);
    emit ready();
}

std::optional<Frame> LinuxHelperClient::request(Op op, PrivError *error)
{
    auto pending = std::make_shared<Pending>();
    quint32 id;
    {
        QMutexLocker lock(&m_mutex);
        if (m_state != State::Ready) {
            if (error)
                *error = {PrivError::Code::Unavailable, QStringLiteral("The privileged helper isn't running.")};
            return std::nullopt;
        }
        id = m_nextId++;
        if (m_nextId == 0)
            m_nextId = 1;  // 0 is the helper's HELLO
        m_pending.insert(id, pending);
    }

    Frame f;
    f.requestId = id;
    f.op = std::uint16_t(op);
    const QByteArray bytes = [&] {
        const Bytes b = encode(f);
        return QByteArray(reinterpret_cast<const char *>(b.data()), qsizetype(b.size()));
    }();
    const auto write = [this, bytes] {
        if (m_process && state() == State::Ready)
            m_process->write(bytes);
    };

    const bool onOwnerThread = QThread::currentThread() == thread();
    const QDeadlineTimer deadline(m_options.requestTimeout);
    if (onOwnerThread) {
        // The answer arrives through this thread's event loop: spin a local one.
        write();
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(this, &LinuxHelperClient::requestFinished, &loop, [&loop, id](quint32 done) {
            if (done == id)
                loop.quit();
        });
        bool finished;
        {
            QMutexLocker lock(&m_mutex);
            finished = pending->finished;
        }
        if (!finished) {
            timer.start(m_options.requestTimeout);
            loop.exec();
        }
    } else {
        QMetaObject::invokeMethod(this, write, Qt::QueuedConnection);
        QMutexLocker lock(&m_mutex);
        while (!pending->finished) {
            if (!pending->done.wait(&m_mutex, deadline))
                break;
        }
    }

    QMutexLocker lock(&m_mutex);
    if (!pending->finished) {
        m_pending.remove(id);
        if (error)
            *error = {PrivError::Code::IoError, QStringLiteral("The privileged helper didn't answer in time.")};
        return std::nullopt;
    }
    if (!pending->reply) {
        if (error)
            *error = pending->error;
        return std::nullopt;
    }
    const Frame &reply = *pending->reply;
    if (reply.isError()) {
        const auto e = decodeError(reply.payload);
        if (error) {
            const QString message = e ? QString::fromStdString(e->message) : QStringLiteral("malformed error reply");
            PrivError::Code code = PrivError::Code::Protocol;
            if (e && e->code == ErrorCode::NotPermitted)
                code = PrivError::Code::NotPermitted;
            else if (e && e->code == ErrorCode::IoError)
                code = PrivError::Code::IoError;
            *error = {code, message};
        }
        return std::nullopt;
    }
    if (reply.op != std::uint16_t(op)) {
        if (error)
            *error = {PrivError::Code::Protocol, QStringLiteral("The privileged helper answered a different request.")};
        return std::nullopt;
    }
    return reply;
}

bool LinuxHelperClient::ping(PrivError *error)
{
    return request(Op::Ping, error).has_value();
}

std::optional<SmbiosTables> LinuxHelperClient::readSmbios(PrivError *error)
{
    const auto reply = request(Op::ReadSmbios, error);
    if (!reply)
        return std::nullopt;
    const auto blobs = decodeSmbios(reply->payload);
    if (!blobs) {
        if (error)
            *error = {PrivError::Code::Protocol, QStringLiteral("The privileged helper sent malformed SMBIOS data.")};
        return std::nullopt;
    }
    SmbiosTables t;
    t.entryPoint = QByteArray(reinterpret_cast<const char *>(blobs->entryPoint.data()), qsizetype(blobs->entryPoint.size()));
    t.table = QByteArray(reinterpret_cast<const char *>(blobs->table.data()), qsizetype(blobs->table.size()));
    return t;
}

} // namespace sb

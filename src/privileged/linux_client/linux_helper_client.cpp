// SPDX-License-Identifier: GPL-3.0-or-later
#include "privileged/linux_client/linux_helper_client.h"

#include "synbench_version.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
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

LinuxHelperClient::Launch LinuxHelperClient::defaultLaunch(const QString &appImage, const QString &appDir)
{
    const QString pkexec = QStringLiteral("pkexec");
    if (!appImage.isEmpty()) {
        return {{pkexec, {appImage, QStringLiteral("--synbench-helper")}},
                Command{pkexec, {appImage, QStringLiteral("--appimage-extract-and-run"), QStringLiteral("--synbench-helper")}}};
    }
    // pkexec needs an absolute path.
    return {{pkexec, {QDir(appDir).absoluteFilePath(QStringLiteral("synbench-helper"))}}, std::nullopt};
}

QString LinuxHelperClient::thisBuildId()
{
    return QStringLiteral(SYNBENCH_DISPLAY_VERSION " " SYNBENCH_BUILD_ID);
}

LinuxHelperClient::LinuxHelperClient(Options options, QObject *parent) : QObject(parent), m_options(std::move(options))
{
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
    start(defaultLaunch(qEnvironmentVariable("APPIMAGE"), QCoreApplication::applicationDirPath()));
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
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &LinuxHelperClient::onReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, [p = m_process] {
        for (const QByteArray &line : p->readAllStandardError().split('\n')) {
            if (!line.trimmed().isEmpty())
                qCInfo(lcHelper, "%s", line.constData());
        }
    });
    connect(m_process, &QProcess::finished, this, &LinuxHelperClient::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, &LinuxHelperClient::onErrorOccurred);
    setState(State::Authorising);
    qCInfo(lcHelper) << "starting" << command.program << command.arguments;
    // argv, no shell (P11).
    m_process->start(command.program, command.arguments, QIODevice::ReadWrite);
}

void LinuxHelperClient::stop()
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->closeWriteChannel();
}

void LinuxHelperClient::fail(Failure failure, const QString &message)
{
    {
        QMutexLocker lock(&m_mutex);
        m_failure = failure;
        m_failureMessage = message;
    }
    qCWarning(lcHelper) << "helper failed:" << message;
    failAllPending(message);
    stop();
    setState(State::Failed);
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
    if (s == State::Authorising) {
        if (status == QProcess::NormalExit && exitCode == 126) {
            fail(Failure::Cancelled, QStringLiteral("The administrator prompt was cancelled."));
            return;
        }
        if (status == QProcess::NormalExit && exitCode == 127) {
            fail(Failure::NotAuthorised,
                 QStringLiteral("Authorisation failed, or no authentication agent is running."));
            return;
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

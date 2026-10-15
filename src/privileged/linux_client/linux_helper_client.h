// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "privileged/privileged_ops.h"
#include "privileged/protocol/protocol.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QStringList>

#include <chrono>
#include <memory>
#include <optional>

namespace sb {

// Linux PrivilegedOps (§6, P11): starts synbench-helper as root with pkexec
// (one polkit prompt per launch), checks its HELLO, then sends requests over
// the child's stdin/stdout. Lives on the GUI thread; readSmbios() and ping()
// may be called from any thread and block until the answer, a timeout, or
// the helper's death (then: Unavailable, never a hang).
class LinuxHelperClient : public QObject, public PrivilegedOps
{
    Q_OBJECT

public:
    enum class State { Idle, Authorising, Ready, Failed };
    Q_ENUM(State)

    enum class Failure {
        None,
        Cancelled,       // pkexec 126: the user dismissed the prompt
        NotAuthorised,   // pkexec 127: no agent, wrong password, not allowed
        LaunchFailed,    // pkexec itself couldn't be started
        Exited,          // the helper exited (before HELLO, or mid-session)
        BadHello,        // first frame wasn't a valid HELLO
        BuildMismatch,   // a stale or foreign helper
        NotRoot,         // the helper isn't running as root
    };
    Q_ENUM(Failure)

    struct Command {
        QString program;
        QStringList arguments;
        bool operator==(const Command &) const = default;
    };

    struct Options {
        QString expectedBuildId;  // empty = don't check (tests only)
        bool requireRoot = true;
        std::chrono::milliseconds requestTimeout{15000};
    };

    // The pkexec command lines (no shell, P11). With $APPIMAGE set, root
    // re-executes the AppImage file (root normally can't enter the user's
    // FUSE mount); the fallback adds --appimage-extract-and-run. Otherwise
    // (dev build) the helper next to the app binary.
    struct Launch {
        Command primary;
        std::optional<Command> fallback;
    };
    // What the launch decision looks at. AP5-R2-1: $APPIMAGE alone is never
    // trusted; the AppImage branch needs the running binary to really be
    // inside the mounted image ($APPDIR).
    struct Environment {
        QString appImage;             // $APPIMAGE
        QString appDir;               // $APPDIR (the runtime's mount point)
        QString applicationFilePath;  // QCoreApplication::applicationFilePath()
        QStringList pkexecDirs = defaultPkexecDirs();
        static Environment current();
    };
    static QStringList defaultPkexecDirs();  // fixed system dirs, never $PATH
    // nullopt (with `error`) if pkexec or the helper can't be found.
    static std::optional<Launch> defaultLaunch(const Environment &env, QString *error = nullptr);
    // "<display version> <build id>", what the helper reports in HELLO.
    static QString thisBuildId();

    explicit LinuxHelperClient(Options options, QObject *parent = nullptr);
    ~LinuxHelperClient() override;

    // GUI thread. start() uses defaultLaunch() for this process.
    void start();
    void start(const Launch &launch);
    // Closes the helper's stdin; it exits on EOF.
    void stop();

    // The pkexec (or helper) process id, for logs; 0 if none.
    qint64 helperProcessId() const;

    State state() const;
    Failure failure() const;
    QString failureMessage() const;

    bool isAvailable() const override;
    std::optional<SmbiosTables> readSmbios(PrivError *error) override;
    bool ping(PrivError *error);

signals:
    void stateChanged(sb::LinuxHelperClient::State state);
    void ready();
    void failed(sb::LinuxHelperClient::Failure failure, const QString &message);
    void requestFinished(quint32 requestId);  // internal: wakes GUI-thread waiters

private:
    struct Pending;

    void launch(const Command &command);
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);
    void handleHello(const proto::Frame &frame);
    void fail(Failure failure, const QString &message);
    void failAllPending(const QString &message);
    void setState(State s);
    std::optional<proto::Frame> request(proto::Op op, PrivError *error);

    Options m_options;
    QProcess *m_process = nullptr;
    std::optional<Command> m_fallback;
    proto::Decoder m_decoder{proto::kMaxResponseFrame};

    mutable QMutex m_mutex;  // guards the members below
    State m_state = State::Idle;
    Failure m_failure = Failure::None;
    QString m_failureMessage;
    quint32 m_nextId = 1;
    QHash<quint32, std::shared_ptr<Pending>> m_pending;
};

} // namespace sb

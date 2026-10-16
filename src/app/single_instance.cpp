// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/single_instance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcInstance, "synbench.instance")

namespace sb {

namespace {
constexpr char kRequest[] = "SYNBENCH-RAISE 1";
constexpr char kAnswer[] = "OK";
} // namespace

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {}

QString SingleInstance::nameFor(const QString &dataDir)
{
    const QString canonical = QDir::cleanPath(QFileInfo(dataDir).absoluteFilePath());
    const QByteArray h = QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("synbench-") + QString::fromLatin1(h);
}

bool SingleInstance::listen(const QString &dataDir)
{
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
    }
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    const QString name = nameFor(dataDir);
    // We hold the history's lock, so any socket left under this name is stale.
    QLocalServer::removeServer(name);
    if (!m_server->listen(name)) {
        qCWarning(lcInstance) << "can't listen for a second launch:" << m_server->errorString();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                const QList<QByteArray> lines = s->readAll().split('\n');
                if (lines.isEmpty() || lines.first() != kRequest)
                    return;
                const QString token = lines.size() > 1 ? QString::fromUtf8(lines[1]) : QString();
                s->write(kAnswer);
                s->flush();
                qCInfo(lcInstance) << "another launch asked for this window";
                emit focusRequested(token);
            });
        }
    });
    qCInfo(lcInstance) << "listening for a second launch on" << name;
    return true;
}

bool SingleInstance::requestFocus(const QString &dataDir, int timeoutMs)
{
#ifdef Q_OS_WIN
    // We were just started by the user, so we may hand the foreground over.
    AllowSetForegroundWindow(ASFRW_ANY);
#endif
    QLocalSocket socket;
    socket.connectToServer(nameFor(dataDir));
    if (!socket.waitForConnected(timeoutMs))
        return false;
    QByteArray msg = QByteArray(kRequest) + '\n';
    msg += qgetenv("XDG_ACTIVATION_TOKEN");
    if (msg.endsWith('\n') == false)
        msg += '\n';
    socket.write(msg);
    if (!socket.waitForBytesWritten(timeoutMs) || !socket.waitForReadyRead(timeoutMs))
        return false;
    return socket.readAll().startsWith(kAnswer);
}

} // namespace sb

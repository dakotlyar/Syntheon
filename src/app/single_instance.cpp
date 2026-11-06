// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/single_instance.h"

#include "storage/instance_paths.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>

#ifdef Q_OS_WIN
#include <windows.h>
#ifndef ASFRW_ANY
#define ASFRW_ANY ((DWORD)-1)  // not in every set of headers
#endif
#endif

Q_LOGGING_CATEGORY(lcInstance, "syntheon.instance")

namespace sb {

namespace {
constexpr char kRequest[] = "SYNTHEON-RAISE 1";
constexpr char kAnswer[] = "OK";
} // namespace

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {}

QString SingleInstance::nameFor(const QString &dataDir)
{
    return instance::socketName(dataDir);
}

bool SingleInstance::listen(const QString &dataDir)
{
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_error.clear();
    const QString name = nameFor(dataDir);
    if (name.isEmpty()) {
        m_error = tr("No private folder is available for Syntheon's runtime files.");
        qCWarning(lcInstance) << m_error;
        return false;
    }
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    // We hold the history's lock, so any socket left under this name is stale
    // (and it is in our own 0700 directory: nobody else can have put it there).
    QLocalServer::removeServer(name);
    if (!m_server->listen(name)) {
        m_error = tr("Can't listen for a second launch (%1). Opening Syntheon again won't bring this window forward.")
                      .arg(m_server->errorString());
        qCWarning(lcInstance) << m_error;
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                const QList<QByteArray> lines = s->readAll().split('\n');
                if (lines.isEmpty() || lines.first() != kRequest)
                    return;
                s->write(kAnswer);
                s->flush();
                qCInfo(lcInstance) << "another launch asked for this window";
                emit focusRequested(QString());
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
    // Wayland may only flash the window instead of raising it; flash is the
    // accepted outcome there, so no activation token is passed along.
    socket.write(QByteArray(kRequest) + '\n');
    if (!socket.waitForBytesWritten(timeoutMs) || !socket.waitForReadyRead(timeoutMs))
        return false;
    return socket.readAll().startsWith(kAnswer);
}

} // namespace sb

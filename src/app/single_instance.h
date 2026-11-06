// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace sb {

// One Syntheon per history folder. The lock itself is Storage's (a lock file
// in the folder); this is the other half: the running copy listens on a local
// socket named after the folder, and a second launch asks it to come to the
// front, then exits. Two different folders get two different names, so two
// copies on different histories coexist.
class SingleInstance : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = nullptr);

    // The socket / pipe name for a history folder.
    static QString nameFor(const QString &dataDir);

    // Starts (or moves) listening for `dataDir`. Returns false if the socket
    // couldn't be created (see error()); the app still works, a second launch
    // just can't be told to raise the window.
    bool listen(const QString &dataDir);
    QString error() const { return m_error; }

    // Asks the copy running on `dataDir` to raise its window. True if it
    // answered. Blocks up to `timeoutMs` and needs no event loop of its own
    // (it uses blocking socket calls), so it must not run on the thread that
    // owns the server.
    static bool requestFocus(const QString &dataDir, int timeoutMs = 3000);

signals:
    // Another launch asked for the window (the argument is unused: no
    // activation token is passed, flashing is fine on Wayland).
    void focusRequested(const QString &activationToken);

private:
    QLocalServer *m_server = nullptr;
    QString m_error;
};

} // namespace sb

// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/power_inhibitor.h"

#include <QTest>

#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#endif

using namespace sb;

namespace {

#ifdef Q_OS_LINUX
// logind's own view: is there a sleep/block lock held by "Syntheon" from us?
bool logindShowsSyntheonLock()
{
    const QDBusMessage call =
        QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
                                       QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("ListInhibitors"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return false;
    // a(ssssuu): what, who, why, mode, uid, pid
    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    bool found = false;
    arg.beginArray();
    while (!arg.atEnd()) {
        QString what, who, why, mode;
        uint uid = 0, pid = 0;
        arg.beginStructure();
        arg >> what >> who >> why >> mode >> uid >> pid;
        arg.endStructure();
        if (who == QLatin1StringView("Syntheon") && what.contains(QLatin1StringView("sleep"))
            && mode == QLatin1StringView("block") && pid == uint(QCoreApplication::applicationPid()))
            found = true;
    }
    arg.endArray();
    return found;
}
#endif

} // namespace

// Exercises the real platform inhibitor. CI runners and some desktops have no
// logind session that may block sleep, so a refused acquire is a skip, not a
// failure.
class TestPowerInhibitor : public QObject
{
    Q_OBJECT

private slots:
    void acquireAndRelease()
    {
        auto inhibitor = createSystemPowerInhibitor();
        QVERIFY(inhibitor);
        QString error;
        if (!inhibitor->acquire(QStringLiteral("Syntheon unit test"), &error))
            QSKIP(qPrintable(QStringLiteral("system sleep lock unavailable here: %1").arg(error)));
        QVERIFY(inhibitor->acquire(QStringLiteral("again"), &error));  // already held
#ifdef Q_OS_LINUX
        QVERIFY(logindShowsSyntheonLock());
#endif
        inhibitor->release();
#ifdef Q_OS_LINUX
        QTRY_VERIFY(!logindShowsSyntheonLock());
#endif
        inhibitor->release();  // idempotent
        QVERIFY(inhibitor->acquire(QStringLiteral("re-acquire"), &error));
        inhibitor->release();
    }
};

QTEST_GUILESS_MAIN(TestPowerInhibitor)
#include "tst_power_inhibitor.moc"

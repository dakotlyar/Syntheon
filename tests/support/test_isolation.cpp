// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linked into every test executable, next to crash_handler.cpp. No test may
// ever reach the user's real history or settings: tst_controllers once
// deleted Storage::defaultDataDir() without Qt's test mode on, which was the
// real ~/.local/share/SynBench.
//
//  - At static initialisation, before main() and before anything resolves a
//    path: QStandardPaths test mode (~/.qttest/... on Linux, ...\qttest on
//    Windows), and QSettings' INI user/system paths pointed inside it
//    (QSettings on Windows doesn't follow test mode on its own).
//  - When the QCoreApplication exists: a guard resolves the data, config and
//    INI-settings locations and aborts the whole binary unless every one is
//    under "qttest". A test that defeats the redirect fails, it doesn't run.

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>
#include <cstdlib>

namespace {

QString testConfigRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(QStringLiteral("synbench-ini"));
}

void enableTestPaths()
{
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testConfigRoot() + QStringLiteral("/user"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, testConfigRoot() + QStringLiteral("/system"));
}

bool isTestPath(const QString &path)
{
    return QDir::fromNativeSeparators(path).contains(QLatin1StringView("qttest"), Qt::CaseInsensitive);
}

void checkTestPaths()
{
    const QSettings ini(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("SynBench"), QStringLiteral("SynBench"));
    const QString paths[] = {
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
        ini.fileName(),
    };
    for (const QString &p : paths) {
        if (!isTestPath(p)) {
            std::fprintf(stderr, "test_isolation: '%s' is not a Qt test location; refusing to run tests that could reach "
                                 "the user's real history or settings\n",
                         qPrintable(p));
            std::fflush(stderr);
            std::_Exit(3);
        }
    }
}

}  // namespace

Q_CONSTRUCTOR_FUNCTION(enableTestPaths)
Q_COREAPP_STARTUP_FUNCTION(checkTestPaths)

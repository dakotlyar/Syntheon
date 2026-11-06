// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linked into every test executable, next to crash_handler.cpp. No test may
// ever reach the user's real history or settings: tst_controllers once
// deleted Storage::defaultDataDir() without Qt's test mode on, which was the
// real ~/.local/share/Syntheon.
//
//  - At static initialisation, before main() and before anything resolves a
//    path: QStandardPaths test mode (~/.qttest/... on Linux, ...\qttest on
//    Windows), INI as QSettings' default format (so a plain QSettings() or a
//    QML Settings never lands in the Windows registry), and the INI
//    user/system paths pointed inside the test location (QSettings on Windows
//    doesn't follow test mode on its own).
//  - That initialisation runs before every other dynamic initialiser of the
//    test binary, by construction rather than by link order: MSVC puts this
//    file's initialisers in the "lib" segment (#pragma init_seg(lib), ahead of
//    all user code), GCC and Clang run init_priority(101) first (lower runs
//    earlier; 0-100 are reserved for the implementation). The file is also
//    listed first in each test's sources.
//  - On Unix under CTest, XDG_RUNTIME_DIR from the file the test_home
//    fixture writes (SYNTHEON_TEST_RUNTIME_FILE, tests/check_test_home.cmake):
//    a private folder chosen at test time, so single-instance sockets never
//    share the user's real runtime folder. Launched apps inherit it.
//  - When the QCoreApplication exists: a guard resolves the data, config and
//    INI-settings locations and aborts the whole binary unless every one is
//    under "qttest". A test that defeats the redirect fails, it doesn't run.

#if defined(_MSC_VER)
#pragma init_seg(lib)
#endif

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

QString testConfigRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(QStringLiteral("syntheon-ini"));
}

void useFixtureRuntimeDir()
{
#ifdef Q_OS_UNIX
    const QByteArray file = qgetenv("SYNTHEON_TEST_RUNTIME_FILE");
    if (file.isEmpty())
        return;  // not under CTest's fixture
    std::FILE *f = std::fopen(file.constData(), "r");
    if (!f) {
        std::fprintf(stderr, "test_isolation: %s is missing; run the tests through ctest (the test_home fixture)\n",
                     file.constData());
        std::fflush(stderr);
        std::_Exit(3);
    }
    char path[512] = {};
    const bool read = std::fgets(path, sizeof path, f) != nullptr;
    std::fclose(f);
    path[std::strcspn(path, "\n")] = '\0';
    if (!read || path[0] != '/') {
        std::fprintf(stderr, "test_isolation: %s doesn't name a runtime folder\n", file.constData());
        std::fflush(stderr);
        std::_Exit(3);
    }
    qputenv("XDG_RUNTIME_DIR", QByteArray(path));
#endif
}

void enableTestPaths()
{
    useFixtureRuntimeDir();
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testConfigRoot() + QStringLiteral("/user"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, testConfigRoot() + QStringLiteral("/system"));
}

bool isTestPath(const QString &path)
{
    return QDir::fromNativeSeparators(path).contains(QLatin1StringView("qttest"), Qt::CaseInsensitive);
}

void checkTestPaths()
{
    const QSettings ini(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Syntheon"), QStringLiteral("Syntheon"));
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

struct EnableTestPaths {
    EnableTestPaths() { enableTestPaths(); }
};

#if defined(_MSC_VER)
const EnableTestPaths enableTestPathsFirst;  // init_seg(lib), above
#else
const EnableTestPaths enableTestPathsFirst __attribute__((init_priority(101)));
#endif

}  // namespace

Q_COREAPP_STARTUP_FUNCTION(checkTestPaths)

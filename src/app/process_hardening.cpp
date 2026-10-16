// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/process_hardening.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sb {

void hardenDllSearch()
{
#ifdef Q_OS_WIN
    // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: the application folder, System32 and
    // folders added with AddDllDirectory: not the current directory, not PATH.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    SetDllDirectoryW(L"");
#endif
}

QByteArrayList qtEnvironmentVariablesToScrub()
{
    return {
        "QT_PLUGIN_PATH",                // extra plugin folders
        "QT_QPA_PLATFORM_PLUGIN_PATH",   // the platform plugin itself
        "QT_QPA_GENERIC_PLUGINS",        // generic input plugins
        "QML_IMPORT_PATH",               // QML modules
        "QML2_IMPORT_PATH",
        "QT_QUICK_CONTROLS_CONF",        // a style configuration file
        "QT_QUICK_CONTROLS_STYLE_PATH",  // extra style folders
        "QT_IM_MODULE",                  // input method plugins
        "QT_QPA_PLATFORMTHEME",          // a platform theme plugin
    };
}

QStringList scrubQtEnvironment()
{
    QStringList removed;
    for (const QByteArray &name : qtEnvironmentVariablesToScrub()) {
        if (qEnvironmentVariableIsSet(name.constData())) {
            removed << QString::fromLatin1(name);
            qunsetenv(name.constData());
        }
    }
    return removed;
}

void pinLibraryPaths(const QStringList &paths)
{
    QCoreApplication::setLibraryPaths(paths);
}

QString executableFolder()
{
#ifdef Q_OS_WIN
    wchar_t buffer[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(sizeof buffer / sizeof buffer[0]));
    return n > 0 ? QFileInfo(QString::fromWCharArray(buffer, int(n))).absolutePath() : QString();
#else
    return QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/self/exe"))).absolutePath();
#endif
}

} // namespace sb

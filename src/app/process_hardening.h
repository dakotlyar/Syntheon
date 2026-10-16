// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArrayList>
#include <QString>
#include <QStringList>

namespace sb {

// SynBench runs elevated, so nothing it loads may come from a folder a normal
// user (or malware) could write to. Called first thing in main(), before Qt
// starts.

// Windows: restricts DLL search to the application and system folders and
// drops the current directory from it (SetDefaultDllDirectories +
// SetDllDirectoryW("")). A no-op elsewhere.
void hardenDllSearch();

// The environment variables through which a caller could make Qt load plugins,
// QML modules or a style configuration from somewhere else.
QByteArrayList qtEnvironmentVariablesToScrub();

// Unsets those variables; returns the names that were set (for a log line).
QStringList scrubQtEnvironment();

// Pins Qt's plugin search to `paths` (the application's own plugin folders).
void pinLibraryPaths(const QStringList &paths);

// The folder the executable lives in, found without Qt's application object
// (it does not exist yet).
QString executableFolder();

} // namespace sb

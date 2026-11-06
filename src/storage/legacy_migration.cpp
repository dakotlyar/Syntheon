// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/legacy_migration.h"

#include "storage/instance_paths.h"
#include "storage/legacy_names.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace sb::legacy {

QString defaultDataDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QString::fromLatin1(kFolderName));
}

QString settingsFile()
{
    // Never written: only asked where the file is.
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QString::fromLatin1(kFolderName),
                     QStringLiteral("settings")).fileName();
}

QString lockFilePath(const QString &folder)
{
#ifdef Q_OS_WIN
    const QString dir = QDir(defaultDataDir()).filePath(QStringLiteral("run"));
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        return {};
    const QString dir = QDir(base).filePath(QString::fromLatin1(kRuntimeDirName));
#endif
    // Not instance::stateDir(): that creates the folder, which on Windows would
    // bring the old data folder back.
    if (!QFileInfo(dir).isDir())
        return {};
    return QDir(dir).filePath(QStringLiteral("%1%2.lock").arg(QLatin1StringView(kFilePrefix), instance::key(folder)));
}

} // namespace sb::legacy

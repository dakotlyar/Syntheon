// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/legacy_migration.h"

#include "storage/instance_paths.h"
#include "storage/legacy_names.h"
#include "storage/settings_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <filesystem>
#include <system_error>

namespace sb::legacy {

namespace {

constexpr QLatin1StringView kTempSuffix(".migrating");

bool sameFolder(const QString &a, const QString &b)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return instance::canonicalFolder(a).compare(instance::canonicalFolder(b), cs) == 0;
}

// Not QFile::rename(): when the rename itself fails that falls back to copying
// onto the final name, and a copy can be cut short. Here the final name appearing
// is what says the job is done, so it must appear whole or not at all.
bool renameWhole(const QString &from, const QString &to, QString *error)
{
    std::error_code ec;
    std::filesystem::rename(QFile(from).filesystemFileName(), QFile(to).filesystemFileName(), ec);
    if (ec)
        *error = QString::fromStdString(ec.message());
    return !ec;
}

} // namespace

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

SettingsResult migrateSettings(const QString &legacyIni, const QString &newIni, const QString &legacyDefaultDir)
{
    SettingsResult r;
    if (QFileInfo::exists(newIni) || !QFileInfo::exists(legacyIni))
        return r;

    const QString tmp = newIni + kTempSuffix;
    const auto failed = [&](const QString &why) {
        QFile::remove(tmp);
        r.status = SettingsStatus::Failed;
        r.error = QStringLiteral("Couldn't copy the settings to %1: %2").arg(newIni, why);
        return r;
    };
    if (!QDir().mkpath(QFileInfo(newIni).absolutePath()))
        return failed(QStringLiteral("the folder can't be created."));
    if (QFile::exists(tmp) && !QFile::remove(tmp))
        return failed(QStringLiteral("a leftover temp file can't be removed."));
    if (!QFile::copy(legacyIni, tmp))
        return failed(QStringLiteral("the copy failed."));
    {
        SettingsStore settings(tmp);
        const QString location = settings.dataLocation();
        if (!location.isEmpty() && sameFolder(location, legacyDefaultDir)) {
            settings.setDataLocation({});
            if (!settings.sync())
                return failed(QStringLiteral("the temp file can't be written."));
        }
    }
    QString why;
    if (!renameWhole(tmp, newIni, &why))
        return failed(why);
    r.status = SettingsStatus::Migrated;
    return r;
}

} // namespace sb::legacy

// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/legacy_migration.h"

#include "core/fs_util.h"
#include "storage/database.h"
#include "storage/instance_paths.h"
#include "storage/legacy_names.h"
#include "storage/settings_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

#include <filesystem>
#include <optional>
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

// A copy keeps the original's mode, and a read-only original must still give a file
// the app can write. Also what lets Windows delete a read-only temp left by a kill.
// Nothing to do on an already writable file: file systems without modes (vfat, exFAT,
// some CIFS) refuse chmod, and that must not stop a copy that needs none.
bool makeWritable(const QString &path)
{
    const QFile::Permissions permissions = QFile::permissions(path);
    return (permissions & QFile::WriteOwner) || QFile::setPermissions(path, permissions | QFile::WriteOwner);
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

QByteArray sha256(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
        return {};
    return hash.result();
}

// What must be the same on both sides of the copy.
struct Facts {
    int runs = -1;
    int userVersion = -1;
    bool operator==(const Facts &) const = default;
};

// SQLite's own reason, e.g. "file is not a database", for showing to the user.
QString reason(const QSqlQuery &q)
{
    const QSqlError e = q.lastError();
    return (e.databaseText().isEmpty() ? e.driverText() : e.databaseText()).trimmed();
}

bool readFacts(QSqlDatabase &db, Facts *facts, QString *error)
{
    QSqlQuery q(db);
    // Also what fails on a file with no runs table: it isn't a history.
    if (!q.exec(QStringLiteral("SELECT count(*) FROM runs")) || !q.next()) {
        *error = reason(q);
        return false;
    }
    facts->runs = q.value(0).toInt();
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
        *error = reason(q);
        return false;
    }
    facts->userVersion = q.value(0).toInt();
    return true;
}

// Folds the WAL into the file and reads the facts. Any failure leaves the file alone.
bool checkpointAndRead(const QString &dbPath, Facts *facts, QString *error)
{
    Database::ScopedConnection conn(dbPath, QStringLiteral("adopt-source"));
    if (!conn.isOpen()) {
        *error = conn.error();
        return false;
    }
    {
        QSqlQuery q(conn.db());
        if (!q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) || !q.next()) {
            *error = reason(q);
            return false;
        }
        if (q.value(0).toInt() != 0) {
            *error = QStringLiteral("The old history is busy.");
            return false;
        }
    }
    return readFacts(conn.db(), facts, error);
}

// The copy against the old file, its bytes before anything opens it. `facts` is
// what the old file said, or unset if it wasn't asked, and comes back as the copy's.
bool verifyCopy(const QString &legacyDb, const QString &tmp, Facts *facts, QString *error)
{
    const QByteArray hash = sha256(tmp);
    if (hash.isEmpty() || hash != sha256(legacyDb)) {
        *error = QStringLiteral("The copy isn't the same as the original.");
        return false;
    }
    Database::ScopedConnection conn(tmp, QStringLiteral("adopt-verify"));
    if (!conn.isOpen()) {
        *error = conn.error();
        return false;
    }
    {
        QSqlQuery q(conn.db());
        if (!q.exec(QStringLiteral("PRAGMA integrity_check")) || !q.next()
            || q.value(0).toString() != QLatin1StringView("ok")) {
            *error = QStringLiteral("The history failed its integrity check.");
            return false;
        }
    }
    Facts copied;
    if (!readFacts(conn.db(), &copied, error))
        return false;
    if (facts->runs >= 0 && !(copied == *facts)) {
        *error = QStringLiteral("The copy doesn't have every run.");
        return false;
    }
    *facts = copied;
    return true;
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
    if (QFile::exists(tmp)) {
        makeWritable(tmp);
        if (!QFile::remove(tmp))
            return failed(QStringLiteral("a leftover temp file can't be removed."));
    }
    if (!QFile::copy(legacyIni, tmp) || !makeWritable(tmp))
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
    if (!fs::syncFile(tmp))
        return failed(QStringLiteral("the temp file can't be written."));
    QString why;
    if (!renameWhole(tmp, newIni, &why))
        return failed(why);
    r.status = SettingsStatus::Migrated;
    return r;
}

AdoptResult adoptHistory(const QString &legacyDb, const QString &newDb, const QString &legacyLockPath,
                         const std::function<bool(Stage)> &hook)
{
    AdoptResult r;
    if (QFileInfo::exists(newDb) || !QFileInfo::exists(legacyDb))
        return r;

    const auto failed = [&r](const QString &why) {
        r.status = AdoptStatus::Failed;
        r.error = QStringLiteral("Couldn't copy your saved runs: %1%2 Your saved runs were not changed.")
                      .arg(why, why.endsWith(QLatin1Char('.')) ? QString() : QStringLiteral("."));
        return r;
    };
    const auto stop = [&hook](Stage stage) { return hook && !hook(stage); };
    const auto interrupted = [&r] {
        r.status = AdoptStatus::Failed;
        r.error = QStringLiteral("Interrupted.");
        return r;
    };

    // Same rule as the app's own lock: stale only if its owner is dead.
    std::optional<QLockFile> lock;
    if (!legacyLockPath.isEmpty()) {
        lock.emplace(legacyLockPath);
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) {
            if (lock->error() != QLockFile::LockFailedError)
                return failed(QStringLiteral("the old lock file %1 can't be read.").arg(legacyLockPath));
            r.status = AdoptStatus::LegacyInUse;
            r.error = QStringLiteral("An old build has this history open.");
            return r;
        }
    }

    const QString tmp = newDb + kTempSuffix;
    if (QFile::exists(tmp))
        makeWritable(tmp);
    if (!Database::removeDbFiles(tmp))
        return failed(QStringLiteral("a leftover temp file can't be removed."));

    // A read-only file isn't opened at all: SQLite can't clean up after itself there,
    // so it would leave -wal and -shm beside the old file and refuse the next attempt.
    // This user's old build can't be writing to it (the owner of someone else's file
    // could), and the hash check below still covers the copy.
    const bool readOnly = !QFileInfo(legacyDb).isWritable();
    if (readOnly && QFileInfo(legacyDb + QStringLiteral("-wal")).size() > 0)
        return failed(QStringLiteral("the old file %1 is read-only and may have runs still in its WAL. "
                                     "Make it writable and start Syntheon again.").arg(legacyDb));

    Facts facts;
    QString why;
    if (!readOnly && !checkpointAndRead(legacyDb, &facts, &why))
        return failed(why);
    if (stop(Stage::BeforeCopy))
        return interrupted();

    if (!QFile::copy(legacyDb, tmp) || !makeWritable(tmp) || !fs::syncFile(tmp)) {
        Database::removeDbFiles(tmp);
        return failed(QStringLiteral("the copy to %1 failed (is the disk full?).").arg(QFileInfo(newDb).absolutePath()));
    }
    if (stop(Stage::AfterCopy))
        return interrupted();

    if (!verifyCopy(legacyDb, tmp, &facts, &why)) {
        Database::removeDbFiles(tmp);
        return failed(why);
    }
    // Opening it for the check can leave these; the file itself is complete.
    for (const char *suffix : {"-wal", "-shm"})
        QFile::remove(tmp + QLatin1StringView(suffix));
    if (stop(Stage::AfterVerify))
        return interrupted();

    if (!renameWhole(tmp, newDb, &why)) {
        Database::removeDbFiles(tmp);
        return failed(why);
    }
    if (stop(Stage::AfterRename))
        return interrupted();

    r.status = AdoptStatus::Adopted;
    r.runCount = facts.runs;
    r.legacyFile = QFileInfo(legacyDb).absoluteFilePath();
    return r;
}

} // namespace sb::legacy

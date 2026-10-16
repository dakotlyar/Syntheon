// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace sb {

// SQLite via Qt's QSQLITE driver (P9): WAL, foreign keys on, one connection per
// thread (the UI reads, the run worker writes). Migrations are ordered SQL
// scripts embedded as resources; PRAGMA user_version holds the version.
namespace Database {

inline constexpr const char *kFileName = "synbench.db";
inline constexpr const char *kMigrationsDir = ":/synbench/migrations";

struct MigrateResult {
    enum class Status {
        Ok,
        NewerVersion,  // made by a newer SynBench: refused, file untouched
        Damaged,       // not readable as a SQLite database (or not ours): refused, file untouched
        Failed,
    };
    Status status = Status::Failed;
    QString error;
    int fromVersion = 0;
    int toVersion = 0;
    QString backupPath;  // set when a backup was made

    bool ok() const { return status == Status::Ok; }
};

// The schema version this build knows: the number of embedded migrations.
int schemaVersion(const QString &migrationsDir = QString::fromLatin1(kMigrationsDir));

// Brings the database at `path` up to date (creating it if needed):
//  - user_version > ours: refuse (NewerVersion), never downgrade;
//  - user_version < ours: if the file already existed, checkpoint the WAL and
//    copy it to "<path>.bak-v<N>", then apply each pending script in its own
//    transaction (a failed script rolls back and stops).
// Opens and closes its own connection. `migrationsDir` is for tests.
MigrateResult migrate(const QString &path, const QString &migrationsDir = QString::fromLatin1(kMigrationsDir));

// Opens a named connection with the standard pragmas (WAL, foreign keys,
// busy timeout). The caller removes it with closeConnection(). Returns an
// invalid/closed QSqlDatabase on failure, with `error` set.
QSqlDatabase openConnection(const QString &path, const QString &connectionName, QString *error = nullptr);
void closeConnection(const QString &connectionName);

// A unique connection name for a temporary or per-thread connection.
QString uniqueConnectionName(const QString &purpose);

// Splits a migration script into statements. Handles -- and /* */
// comments, '…' strings, "…", `…` and […] identifiers, and CREATE TRIGGER
// bodies (BEGIN …; …; END).
QStringList splitSqlScript(const QString &script);

// RAII temporary connection, removed on destruction.
class ScopedConnection
{
public:
    ScopedConnection(const QString &path, const QString &purpose);
    ~ScopedConnection();
    ScopedConnection(const ScopedConnection &) = delete;
    ScopedConnection &operator=(const ScopedConnection &) = delete;

    bool isOpen() const { return m_db.isOpen(); }
    QSqlDatabase &db() { return m_db; }
    const QString &error() const { return m_error; }

private:
    QString m_name;
    QSqlDatabase m_db;
    QString m_error;
};

} // namespace Database
} // namespace sb

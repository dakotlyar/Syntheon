// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/database.h"

#include <QAtomicInteger>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <map>

namespace sb::Database {

namespace {

// Ordered scripts NNNN_name.sql; returns version -> path. Versions must run
// 1..N without gaps.
bool listMigrations(const QString &dir, std::map<int, QString> *out, QString *error)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{4})_.+\\.sql$"));
    out->clear();
    const QStringList files = QDir(dir).entryList({QStringLiteral("*.sql")}, QDir::Files, QDir::Name);
    for (const QString &f : files) {
        const auto m = re.match(f);
        if (!m.hasMatch())
            continue;
        out->emplace(m.captured(1).toInt(), QDir(dir).filePath(f));
    }
    int expected = 1;
    for (const auto &[version, path] : *out) {
        if (version != expected) {
            if (error)
                *error = QStringLiteral("Migration %1 is missing").arg(expected);
            return false;
        }
        ++expected;
    }
    return true;
}

bool exec(QSqlDatabase &db, const QString &sql, QString *error)
{
    QSqlQuery q(db);
    if (q.exec(sql))
        return true;
    if (error)
        *error = q.lastError().text();
    return false;
}

int userVersion(QSqlDatabase &db, bool *ok)
{
    QSqlQuery q(db);
    *ok = q.exec(QStringLiteral("PRAGMA user_version")) && q.next();
    return *ok ? q.value(0).toInt() : 0;
}

} // namespace

QString uniqueConnectionName(const QString &purpose)
{
    static QAtomicInteger<quint64> counter;
    return QStringLiteral("synbench-%1-%2-%3")
        .arg(purpose)
        .arg(reinterpret_cast<quintptr>(QThread::currentThread()), 0, 16)
        .arg(counter.fetchAndAddRelaxed(1));
}

QSqlDatabase openConnection(const QString &path, const QString &connectionName, QString *error)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!db.open()) {
        if (error)
            *error = db.lastError().text();
        return db;
    }
    QString err;
    if (!exec(db, QStringLiteral("PRAGMA foreign_keys = ON"), &err)) {
        if (error)
            *error = err;
        db.close();
    }
    return db;
}

void closeConnection(const QString &connectionName)
{
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        if (db.isValid())
            db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

ScopedConnection::ScopedConnection(const QString &path, const QString &purpose)
    : m_name(uniqueConnectionName(purpose))
{
    m_db = openConnection(path, m_name, &m_error);
}

ScopedConnection::~ScopedConnection()
{
    m_db = QSqlDatabase();
    closeConnection(m_name);
}

QStringList splitSqlScript(const QString &script)
{
    QStringList out;
    QString current;
    // F8: inside a CREATE [TEMP|TEMPORARY] TRIGGER statement, ';' only ends
    // the statement at nesting depth 0. BEGIN and CASE open a level, END
    // closes one (CASE … END can appear in the trigger's WHEN clause and body).
    QStringList lead;  // the statement's first three words, upper-case
    bool trigger = false;
    int depth = 0;

    const auto flush = [&] {
        const QString stmt = current.trimmed();
        if (!stmt.isEmpty())
            out.append(stmt);
        current.clear();
        lead.clear();
        trigger = false;
        depth = 0;
    };
    const auto isWordChar = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };

    const qsizetype n = script.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = script[i];
        const QChar next = i + 1 < n ? script[i + 1] : QChar();
        if (c == QLatin1Char('-') && next == QLatin1Char('-')) {
            while (i < n && script[i] != QLatin1Char('\n'))
                ++i;
            current += QLatin1Char('\n');
            continue;
        }
        if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < n && !(script[i] == QLatin1Char('*') && script[i + 1] == QLatin1Char('/')))
                ++i;
            ++i;
            current += QLatin1Char(' ');
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('[')) {
            // Strings and quoted identifiers. '…', "…" and `…` escape their
            // quote by doubling it; […] has no escape.
            const QChar close = c == QLatin1Char('[') ? QLatin1Char(']') : c;
            current += c;
            ++i;
            while (i < n) {
                current += script[i];
                if (script[i] == close) {
                    if (close != QLatin1Char(']') && i + 1 < n && script[i + 1] == close)
                        current += script[++i];
                    else
                        break;
                }
                ++i;
            }
            continue;
        }
        if (isWordChar(c)) {
            qsizetype j = i;
            while (j < n && isWordChar(script[j]))
                ++j;
            const QString word = script.mid(i, j - i);
            current += word;
            i = j - 1;
            const QString upper = word.toUpper();
            if (lead.size() < 3) {
                lead.append(upper);
                trigger = lead.size() >= 2 && lead[0] == QLatin1StringView("CREATE")
                          && (lead[1] == QLatin1StringView("TRIGGER")
                              || (lead.size() == 3
                                  && (lead[1] == QLatin1StringView("TEMP") || lead[1] == QLatin1StringView("TEMPORARY"))
                                  && lead[2] == QLatin1StringView("TRIGGER")));
            }
            if (trigger) {
                if (upper == QLatin1StringView("BEGIN") || upper == QLatin1StringView("CASE"))
                    ++depth;
                else if (upper == QLatin1StringView("END") && depth > 0)
                    --depth;
            }
            continue;
        }
        if (c == QLatin1Char(';')) {
            if (depth > 0) {
                current += c;
                continue;
            }
            flush();
            continue;
        }
        current += c;
    }
    flush();
    return out;
}

int schemaVersion(const QString &migrationsDir)
{
    std::map<int, QString> scripts;
    if (!listMigrations(migrationsDir, &scripts, nullptr))
        return 0;
    return static_cast<int>(scripts.size());
}

MigrateResult migrate(const QString &path, const QString &migrationsDir)
{
    MigrateResult result;
    std::map<int, QString> scripts;
    if (!listMigrations(migrationsDir, &scripts, &result.error))
        return result;
    const int target = static_cast<int>(scripts.size());
    result.toVersion = target;

    const QFileInfo info(path);
    const bool existed = info.exists() && info.size() > 0;

    ScopedConnection conn(path, QStringLiteral("migrate"));
    if (!conn.isOpen()) {
        result.error = conn.error();
        return result;
    }
    QSqlDatabase &db = conn.db();

    bool ok = false;
    const int current = userVersion(db, &ok);
    if (!ok) {
        result.error = db.lastError().text();
        return result;
    }
    result.fromVersion = current;

    // Check the version before changing anything (including the journal
    // mode), so a newer database is left exactly as it was.
    if (current > target) {
        result.status = MigrateResult::Status::NewerVersion;
        result.error = QStringLiteral("This history was made by a newer SynBench (database version %1; this "
                                      "version supports up to %2).")
                           .arg(current)
                           .arg(target);
        return result;
    }

    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("PRAGMA journal_mode = WAL")) || !q.next()
            || q.value(0).toString().compare(QLatin1StringView("wal"), Qt::CaseInsensitive) != 0) {
            result.error = QStringLiteral("Couldn't switch the database to WAL mode: %1").arg(q.lastError().text());
            return result;
        }
    }

    if (current == target) {
        result.status = MigrateResult::Status::Ok;
        return result;
    }

    if (existed) {
        // F9: the backup is a plain file copy, so the WAL must be fully
        // checkpointed first. wal_checkpoint reports "busy" in its first
        // column (not as an error) when another connection holds it up.
        QSqlQuery cp(db);
        if (!cp.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) || !cp.next()) {
            result.error = cp.lastError().text();
            return result;
        }
        if (cp.value(0).toInt() != 0) {
            result.error = QStringLiteral("The history database is in use elsewhere, so it can't be backed up "
                                          "before upgrading. Close other SynBench windows and try again.");
            return result;
        }
        cp.finish();
        const QString backup = QStringLiteral("%1.bak-v%2").arg(path).arg(current);
        if (QFile::exists(backup) && !QFile::remove(backup)) {
            result.error = QStringLiteral("Couldn't replace the old backup %1").arg(backup);
            return result;
        }
        if (!QFile::copy(path, backup)) {
            result.error = QStringLiteral("Couldn't back up the database to %1").arg(backup);
            return result;
        }
        result.backupPath = backup;
    }

    for (const auto &[version, scriptPath] : scripts) {
        if (version <= current)
            continue;
        QFile f(scriptPath);
        if (!f.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Couldn't read migration %1").arg(scriptPath);
            return result;
        }
        const QStringList statements = splitSqlScript(QString::fromUtf8(f.readAll()));

        if (!db.transaction()) {
            result.error = db.lastError().text();
            return result;
        }
        QString err;
        bool applied = true;
        for (const QString &stmt : statements) {
            if (!exec(db, stmt, &err)) {
                applied = false;
                break;
            }
        }
        if (applied)
            applied = exec(db, QStringLiteral("PRAGMA user_version = %1").arg(version), &err);
        if (!applied || !db.commit()) {
            if (err.isEmpty())
                err = db.lastError().text();
            db.rollback();
            result.error = QStringLiteral("Migration %1 failed: %2").arg(version).arg(err);
            return result;
        }
    }

    result.status = MigrateResult::Status::Ok;
    return result;
}

} // namespace sb::Database

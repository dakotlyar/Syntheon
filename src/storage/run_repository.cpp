// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/run_repository.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace sb {

namespace {

QVariant nullableText(const QString &s)
{
    return s.isNull() ? QVariant(QMetaType::fromType<QString>()) : QVariant(s);
}

QVariant nullableReal(const std::optional<double> &v)
{
    return v ? QVariant(*v) : QVariant(QMetaType::fromType<double>());
}

std::optional<double> optionalReal(const QVariant &v)
{
    return v.isNull() ? std::nullopt : std::optional<double>(v.toDouble());
}

QString toJson(const QVariantMap &map)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

QString toJson(const QJsonObject &obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QJsonObject parseObject(const QString &s)
{
    return QJsonDocument::fromJson(s.toUtf8()).object();
}

// For NOT NULL text columns: a null QString would bind as SQL NULL.
QString notNull(const QString &s)
{
    return s.isNull() ? QStringLiteral("") : s;
}

// Stored text is null for empty, so optional text columns read back as "".
QString text(const QVariant &v)
{
    return v.isNull() ? QString() : v.toString();
}

} // namespace

bool RunRepository::fail(const QString &context, const QString &error)
{
    m_error = context + QStringLiteral(": ") + error;
    return false;
}

QString RunRepository::formatTime(const QDateTime &t)
{
    return t.isValid() ? t.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime RunRepository::parseTime(const QString &s)
{
    return s.isEmpty() ? QDateTime() : QDateTime::fromString(s, Qt::ISODateWithMs);
}

QByteArray RunRepository::packPoints(const QVector<QPointF> &points)
{
    QByteArray blob(points.size() * 2 * qsizetype(sizeof(double)), Qt::Uninitialized);
    char *p = blob.data();
    for (const QPointF &pt : points) {
        for (double v : {pt.x(), pt.y()}) {
            quint64 bits;
            std::memcpy(&bits, &v, sizeof bits);
            qToLittleEndian(bits, p);
            p += sizeof bits;
        }
    }
    return blob;
}

QVector<QPointF> RunRepository::unpackPoints(const QByteArray &blob)
{
    QVector<QPointF> points;
    const qsizetype pairs = blob.size() / qsizetype(2 * sizeof(double));
    points.reserve(pairs);
    const char *p = blob.constData();
    for (qsizetype i = 0; i < pairs; ++i) {
        double xy[2];
        for (double &v : xy) {
            const quint64 bits = qFromLittleEndian<quint64>(p);
            std::memcpy(&v, &bits, sizeof v);
            p += sizeof bits;
        }
        points.append({xy[0], xy[1]});
    }
    return points;
}

std::optional<qint64> RunRepository::insertRun(const RunRecord &r)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO runs (uuid, kind, status, started_at, finished_at, app_version, app_build, machine_label, "
        "overall_score, system_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(r.uuid.toString(QUuid::WithoutBraces));
    q.addBindValue(toKey(r.kind));
    q.addBindValue(toKey(r.status));
    q.addBindValue(formatTime(r.startedAt));
    q.addBindValue(nullableText(formatTime(r.finishedAt)));
    q.addBindValue(notNull(r.appVersion));
    q.addBindValue(notNull(r.appBuild));
    q.addBindValue(notNull(r.system.machineLabel));
    q.addBindValue(nullableReal(r.overall));
    q.addBindValue(toJson(r.system.toJson()));
    if (!q.exec()) {
        fail(QStringLiteral("insert run"), q.lastError().text());
        return std::nullopt;
    }
    return q.lastInsertId().toLongLong();
}

bool RunRepository::insertTestResultRows(qint64 runId, int position, const TestResult &t)
{
    // A test has one row per run: replace its placeholder (or an earlier
    // row); the children go with it (ON DELETE CASCADE).
    QSqlQuery del(m_db);
    del.prepare(QStringLiteral("DELETE FROM test_results WHERE run_id = ? AND test_id = ?"));
    del.addBindValue(runId);
    del.addBindValue(notNull(t.testId));
    if (!del.exec())
        return fail(QStringLiteral("replace test result %1").arg(t.testId), del.lastError().text());

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO test_results (run_id, position, test_id, test_version, formula_version, status, "
        "status_reason, score, options_json, started_at, duration_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(runId);
    q.addBindValue(position);
    q.addBindValue(notNull(t.testId));
    q.addBindValue(t.testVersion);
    q.addBindValue(t.formulaVersion);
    q.addBindValue(toKey(t.status));
    q.addBindValue(nullableText(t.statusReason.isEmpty() ? QString() : t.statusReason));
    q.addBindValue(nullableReal(t.score));
    q.addBindValue(toJson(t.options));
    q.addBindValue(nullableText(formatTime(t.startedAt)));
    q.addBindValue(t.startedAt.isValid() || t.durationMs ? QVariant(t.durationMs)
                                                         : QVariant(QMetaType::fromType<qint64>()));
    if (!q.exec())
        return fail(QStringLiteral("insert test result %1").arg(t.testId), q.lastError().text());
    const qint64 resultId = q.lastInsertId().toLongLong();

    QSqlQuery sub(m_db);
    sub.prepare(QStringLiteral(
        "INSERT INTO sub_results (test_result_id, sub_id, status, status_reason, score) VALUES (?, ?, ?, ?, ?)"));
    for (const SubResult &s : t.subResults) {
        sub.addBindValue(resultId);
        sub.addBindValue(notNull(s.id));
        sub.addBindValue(toKey(s.status));
        sub.addBindValue(nullableText(s.reason.isEmpty() ? QString() : s.reason));
        sub.addBindValue(nullableReal(s.score));
        if (!sub.exec())
            return fail(QStringLiteral("insert sub-result %1").arg(s.id), sub.lastError().text());
    }

    QSqlQuery met(m_db);
    met.prepare(QStringLiteral(
        "INSERT INTO metrics (test_result_id, sub_id, key, label, value_num, value_text, unit, direction, "
        "headline, position) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (int i = 0; i < t.metrics.size(); ++i) {
        const Metric &m = t.metrics[i];
        met.addBindValue(resultId);
        met.addBindValue(nullableText(m.subTestId.isEmpty() ? QString() : m.subTestId));
        met.addBindValue(notNull(m.key));
        met.addBindValue(notNull(m.label));
        if (m.isNumeric() && std::isfinite(std::get<double>(m.value))) {
            met.addBindValue(std::get<double>(m.value));
            met.addBindValue(QVariant(QMetaType::fromType<QString>()));
        } else if (m.isNumeric()) {
            // F3: SQLite stores NaN as NULL, which would break the CHECK and
            // roll back the whole test row. Keep the value as text instead.
            const double v = std::get<double>(m.value);
            met.addBindValue(QVariant(QMetaType::fromType<double>()));
            met.addBindValue(std::isnan(v) ? QStringLiteral("nan") : v > 0 ? QStringLiteral("inf") : QStringLiteral("-inf"));
        } else {
            met.addBindValue(QVariant(QMetaType::fromType<double>()));
            // A text value is never NULL (the CHECK needs exactly one value).
            const QString &v = std::get<QString>(m.value);
            met.addBindValue(v.isNull() ? QStringLiteral("") : v);
        }
        met.addBindValue(m.unit.isNull() ? QStringLiteral("") : m.unit);
        met.addBindValue(toKey(m.direction));
        met.addBindValue(m.headline ? 1 : 0);
        met.addBindValue(i);
        if (!met.exec())
            return fail(QStringLiteral("insert metric %1").arg(m.key), met.lastError().text());
    }

    QSqlQuery ser(m_db);
    ser.prepare(QStringLiteral(
        "INSERT INTO series (test_result_id, key, label, x_label, x_unit, y_unit, points) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    for (const Series &s : t.series) {
        ser.addBindValue(resultId);
        ser.addBindValue(notNull(s.key));
        ser.addBindValue(notNull(s.label));
        ser.addBindValue(nullableText(s.xLabel.isEmpty() ? QString() : s.xLabel));
        ser.addBindValue(nullableText(s.xUnit.isEmpty() ? QString() : s.xUnit));
        ser.addBindValue(nullableText(s.yUnit.isEmpty() ? QString() : s.yUnit));
        ser.addBindValue(packPoints(s.points));
        if (!ser.exec())
            return fail(QStringLiteral("insert series %1").arg(s.key), ser.lastError().text());
    }
    return true;
}

bool RunRepository::insertTestResult(qint64 runId, int position, const TestResult &result)
{
    if (!m_db.transaction())
        return fail(QStringLiteral("begin"), m_db.lastError().text());
    if (!insertTestResultRows(runId, position, result)) {
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        const QString err = m_db.lastError().text();
        m_db.rollback();
        return fail(QStringLiteral("commit test result"), err);
    }
    return true;
}

bool RunRepository::finishRun(qint64 runId, RunStatus status, const QDateTime &finishedAt,
                              std::optional<double> overall)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE runs SET status = ?, finished_at = ?, overall_score = ? WHERE id = ?"));
    q.addBindValue(toKey(status));
    q.addBindValue(nullableText(formatTime(finishedAt)));
    q.addBindValue(nullableReal(overall));
    q.addBindValue(runId);
    if (!q.exec())
        return fail(QStringLiteral("finish run"), q.lastError().text());
    if (q.numRowsAffected() != 1)
        return fail(QStringLiteral("finish run"), QStringLiteral("no run with id %1").arg(runId));
    return true;
}

std::optional<qint64> RunRepository::saveRun(const RunRecord &record)
{
    if (!m_db.transaction()) {
        fail(QStringLiteral("begin"), m_db.lastError().text());
        return std::nullopt;
    }
    const auto id = insertRun(record);
    bool ok = id.has_value();
    for (int i = 0; ok && i < record.tests.size(); ++i)
        ok = insertTestResultRows(*id, i, record.tests[i]);
    if (ok && !m_db.commit()) {
        fail(QStringLiteral("commit run"), m_db.lastError().text());
        ok = false;
    }
    if (!ok) {
        m_db.rollback();
        return std::nullopt;
    }
    return id;
}

std::optional<qint64> RunRepository::runId(const QUuid &uuid)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM runs WHERE uuid = ?"));
    q.addBindValue(uuid.toString(QUuid::WithoutBraces));
    if (!q.exec()) {
        fail(QStringLiteral("find run"), q.lastError().text());
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    return q.value(0).toLongLong();
}

std::optional<RunRecord> RunRepository::load(const QUuid &uuid)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, kind, status, started_at, finished_at, app_version, app_build, "
                             "overall_score, system_json FROM runs WHERE uuid = ?"));
    q.addBindValue(uuid.toString(QUuid::WithoutBraces));
    if (!q.exec()) {
        fail(QStringLiteral("load run"), q.lastError().text());
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;

    RunRecord r;
    const qint64 id = q.value(0).toLongLong();
    r.uuid = uuid;
    r.kind = runKindFromKey(q.value(1).toString()).value_or(RunKind::Standard);
    r.status = runStatusFromKey(q.value(2).toString()).value_or(RunStatus::Interrupted);
    r.startedAt = parseTime(text(q.value(3)));
    r.finishedAt = parseTime(text(q.value(4)));
    r.appVersion = q.value(5).toString();
    r.appBuild = q.value(6).toString();
    r.overall = optionalReal(q.value(7));
    r.system = SystemSnapshot::fromJson(parseObject(q.value(8).toString()));

    QSqlQuery tq(m_db);
    tq.prepare(QStringLiteral("SELECT id, test_id, test_version, formula_version, status, status_reason, score, "
                              "options_json, started_at, duration_ms FROM test_results WHERE run_id = ? "
                              "ORDER BY position"));
    tq.addBindValue(id);
    if (!tq.exec()) {
        fail(QStringLiteral("load test results"), tq.lastError().text());
        return std::nullopt;
    }

    QSqlQuery sub(m_db), met(m_db), ser(m_db);
    sub.prepare(QStringLiteral(
        "SELECT sub_id, status, status_reason, score FROM sub_results WHERE test_result_id = ? ORDER BY id"));
    met.prepare(QStringLiteral("SELECT sub_id, key, label, value_num, value_text, unit, direction, headline "
                               "FROM metrics WHERE test_result_id = ? ORDER BY position"));
    ser.prepare(QStringLiteral("SELECT key, label, x_label, x_unit, y_unit, points FROM series "
                               "WHERE test_result_id = ? ORDER BY id"));

    while (tq.next()) {
        TestResult t;
        const qint64 resultId = tq.value(0).toLongLong();
        t.testId = tq.value(1).toString();
        t.testVersion = tq.value(2).toInt();
        t.formulaVersion = tq.value(3).toInt();
        t.status = statusFromKey(tq.value(4).toString()).value_or(Status::Error);
        t.statusReason = text(tq.value(5));
        t.score = optionalReal(tq.value(6));
        t.options = parseObject(tq.value(7).toString()).toVariantMap();
        t.startedAt = parseTime(text(tq.value(8)));
        t.durationMs = tq.value(9).toLongLong();

        sub.addBindValue(resultId);
        if (!sub.exec()) {
            fail(QStringLiteral("load sub-results"), sub.lastError().text());
            return std::nullopt;
        }
        while (sub.next()) {
            t.subResults.append({sub.value(0).toString(),
                                 statusFromKey(sub.value(1).toString()).value_or(Status::Error),
                                 text(sub.value(2)), optionalReal(sub.value(3))});
        }

        met.addBindValue(resultId);
        if (!met.exec()) {
            fail(QStringLiteral("load metrics"), met.lastError().text());
            return std::nullopt;
        }
        while (met.next()) {
            Metric m;
            m.subTestId = text(met.value(0));
            m.key = met.value(1).toString();
            m.label = met.value(2).toString();
            if (met.value(3).isNull())
                m.value = text(met.value(4));
            else
                m.value = met.value(3).toDouble();
            m.unit = text(met.value(5));
            m.direction = directionFromKey(met.value(6).toString()).value_or(Direction::Neutral);
            m.headline = met.value(7).toInt() != 0;
            t.metrics.append(m);
        }

        ser.addBindValue(resultId);
        if (!ser.exec()) {
            fail(QStringLiteral("load series"), ser.lastError().text());
            return std::nullopt;
        }
        while (ser.next()) {
            t.series.append({ser.value(0).toString(), ser.value(1).toString(), text(ser.value(2)),
                             text(ser.value(3)), text(ser.value(4)), unpackPoints(ser.value(5).toByteArray())});
        }

        r.tests.append(t);
    }
    return r;
}

int RunRepository::runCount()
{
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM runs")) || !q.next()) {
        fail(QStringLiteral("count runs"), q.lastError().text());
        return -1;
    }
    return q.value(0).toInt();
}

bool RunRepository::appendLog(qint64 runId, const QVector<LogLine> &lines)
{
    if (lines.isEmpty())
        return true;
    if (!m_db.transaction())
        return fail(QStringLiteral("begin"), m_db.lastError().text());
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO run_log (run_id, ts, level, test_id, message) VALUES (?, ?, ?, ?, ?)"));
    for (const LogLine &l : lines) {
        q.addBindValue(runId);
        q.addBindValue(formatTime(l.timestamp));
        q.addBindValue(toKey(l.level));
        q.addBindValue(nullableText(l.testId.isEmpty() ? QString() : l.testId));
        q.addBindValue(notNull(l.message));
        if (!q.exec()) {
            const QString err = q.lastError().text();
            m_db.rollback();
            return fail(QStringLiteral("append log"), err);
        }
    }
    if (!m_db.commit()) {
        const QString err = m_db.lastError().text();
        m_db.rollback();
        return fail(QStringLiteral("commit log"), err);
    }
    return true;
}

QVector<LogLine> RunRepository::loadLog(qint64 runId)
{
    QVector<LogLine> lines;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT ts, level, test_id, message FROM run_log WHERE run_id = ? ORDER BY id"));
    q.addBindValue(runId);
    if (!q.exec()) {
        fail(QStringLiteral("load log"), q.lastError().text());
        return lines;
    }
    while (q.next()) {
        lines.append({parseTime(q.value(0).toString()),
                      logLevelFromKey(q.value(1).toString()).value_or(LogLevel::Info), text(q.value(2)),
                      q.value(3).toString()});
    }
    return lines;
}

bool RunRepository::addCheckpoint(qint64 runId, const QString &testId, int seq, const QByteArray &payload)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO checkpoints (run_id, test_id, seq, created_at, payload) VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(runId);
    q.addBindValue(testId);
    q.addBindValue(seq);
    q.addBindValue(formatTime(QDateTime::currentDateTimeUtc()));
    q.addBindValue(payload);
    if (!q.exec())
        return fail(QStringLiteral("add checkpoint"), q.lastError().text());
    return true;
}

bool RunRepository::addScratchPath(qint64 runId, const QString &path)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO scratch_paths (run_id, path, created_at) VALUES (?, ?, ?)"));
    q.addBindValue(runId);
    q.addBindValue(path);
    q.addBindValue(formatTime(QDateTime::currentDateTimeUtc()));
    if (!q.exec())
        return fail(QStringLiteral("add scratch path"), q.lastError().text());
    return true;
}

bool RunRepository::removeScratchPath(qint64 runId, const QString &path)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM scratch_paths WHERE run_id = ? AND path = ?"));
    q.addBindValue(runId);
    q.addBindValue(path);
    if (!q.exec())
        return fail(QStringLiteral("remove scratch path"), q.lastError().text());
    return true;
}

QVector<QUuid> RunRepository::runsWithStatus(RunStatus status)
{
    QVector<QUuid> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT uuid FROM runs WHERE status = ? ORDER BY started_at"));
    q.addBindValue(toKey(status));
    if (!q.exec()) {
        fail(QStringLiteral("list runs"), q.lastError().text());
        return out;
    }
    while (q.next())
        out.append(QUuid::fromString(q.value(0).toString()));
    return out;
}

QVector<QByteArray> RunRepository::loadCheckpoints(qint64 runId, const QString &testId)
{
    QVector<QByteArray> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT payload FROM checkpoints WHERE run_id = ? AND test_id = ? ORDER BY seq"));
    q.addBindValue(runId);
    q.addBindValue(testId);
    if (!q.exec()) {
        fail(QStringLiteral("load checkpoints"), q.lastError().text());
        return out;
    }
    while (q.next())
        out.append(q.value(0).toByteArray());
    return out;
}

QDateTime RunRepository::lastActivity(qint64 runId)
{
    // Stored times share one UTC ISO format, so text order is time order.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT MAX(t) FROM (SELECT created_at AS t FROM checkpoints WHERE run_id = ? "
                             "UNION ALL SELECT ts FROM run_log WHERE run_id = ?)"));
    q.addBindValue(runId);
    q.addBindValue(runId);
    if (!q.exec() || !q.next())
        return {};
    return parseTime(text(q.value(0)));
}

QVector<RunRepository::ScratchPath> RunRepository::scratchPathsOfFinishedRuns()
{
    QVector<ScratchPath> out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT s.id, r.uuid, s.path FROM scratch_paths s JOIN runs r ON r.id = s.run_id "
                               "WHERE r.status != 'running' ORDER BY s.id"))) {
        fail(QStringLiteral("list scratch paths"), q.lastError().text());
        return out;
    }
    while (q.next())
        out.append({q.value(0).toLongLong(), QUuid::fromString(q.value(1).toString()), q.value(2).toString()});
    return out;
}

bool RunRepository::deleteScratchPathRow(qint64 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM scratch_paths WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec())
        return fail(QStringLiteral("delete scratch path"), q.lastError().text());
    return true;
}

bool RunRepository::clearHistory()
{
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("DELETE FROM runs")))
        return fail(QStringLiteral("clear history"), q.lastError().text());
    if (!q.exec(QStringLiteral("VACUUM")))
        return fail(QStringLiteral("vacuum"), q.lastError().text());
    return true;
}

} // namespace sb

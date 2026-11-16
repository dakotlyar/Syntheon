// SPDX-License-Identifier: GPL-3.0-or-later
// The assembled NPU test (Phase 4c design §13–§15): a whole run through a fake
// RunContext with the null engine (SYNTHEON_NPU_FAKE_VENDOR=null) against a
// file:// model-pack manifest in a temp dir, so the whole loop — detect →
// download → load → three workloads → score — runs end-to-end in milliseconds
// with no NPU and no network (P-VENDOR, P-NET, P-HOME). Also: a NoNpu snapshot
// → NotAvailable; cancel mid-run → Cancelled; a tampered manifest → fail closed.
#include "tests/npu/npu_test.h"
#include "tests/npu/formula_v1.h"
#include "tests/npu/ids.h"

#include <QCborMap>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <mutex>

using namespace sb;
using namespace sb::tests;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

// §14/§15.1: the small FakeRunContext (the tst_ram_test / tst_storage_test
// analogue), plus the NPU model-pack base-URL / cache-root overrides so the
// assembled test points the downloader at a file:// fixture instead of Settings.
class FakeRunContext : public RunContext
{
public:
    void progress(double fraction, const QString &stage) override
    {
        std::lock_guard lock(m_mutex);
        if (!m_progress.isEmpty() && fraction + 1e-9 < m_progress.last().first)
            m_backwards = true;
        m_progress.append({fraction, stage});
        if (!m_cancelOn.isEmpty() && stage.startsWith(m_cancelOn) && !m_cancel) {
            m_cancelAt = Clock::now();
            m_cancel = true;
        }
    }
    void log(LogLevel, const QString &message) override
    {
        std::lock_guard lock(m_mutex);
        m_logs.append(message);
    }
    void setPart(const QString &part) override
    {
        std::lock_guard lock(m_mutex);
        m_parts.append(part);
    }
    bool cancelled() const override { return m_cancel.load(); }
    void subTestBegin(const QString &id) override
    {
        std::lock_guard lock(m_mutex);
        m_subTests.append(id + QStringLiteral(":begin"));
    }
    void subTestEnd(const QString &id, Status status) override
    {
        std::lock_guard lock(m_mutex);
        m_subTests.append(id + QStringLiteral(":") + toKey(status));
    }
    void checkpoint(const QCborMap &) override {}
    QString scratchDir() override { return {}; }
    const SystemSnapshot &system() const override { return m_system; }
    PrivilegedOps &privileged() override { return m_priv; }
    QString npuModelsBaseUrl() const override { return m_baseUrl; }
    QString npuCacheRoot() const override { return m_cacheRoot; }

    void setSystem(const SystemSnapshot &s) { m_system = s; }
    void setNpuModelsBaseUrl(const QString &url) { m_baseUrl = url; }
    void setNpuCacheRoot(const QString &root) { m_cacheRoot = root; }
    void cancelOn(const QString &stage) { m_cancelOn = stage; }
    Clock::time_point cancelAt() const { return m_cancelAt.load(); }
    bool wentBackwards() const { return m_backwards; }

private:
    mutable std::mutex m_mutex;
    QList<QPair<double, QString>> m_progress;
    QStringList m_logs;
    QStringList m_subTests;
    QStringList m_parts;
    std::atomic<bool> m_cancel{false};
    std::atomic<Clock::time_point> m_cancelAt{};
    QString m_cancelOn;
    QString m_baseUrl;
    QString m_cacheRoot;
    bool m_backwards = false;
    SystemSnapshot m_system;
    UnavailablePrivilegedOps m_priv;
};

// A tiny deterministic file body.
QByteArray bytes(int seed, int n = 8)
{
    QByteArray b;
    b.reserve(n);
    for (int i = 0; i < n; ++i)
        b.append(char((i * 31 + seed) & 0xFF));
    return b;
}

QByteArray sha256Hex(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

// The three workload models a well-formed pack lists, in `kModels`.
struct ModelFix {
    const char *id;
    const char *workload;   // classify | detect | llm
    const char *precision;  // INT8 | INT4
    int seed;
};
const ModelFix kModels[] = {
    {"resnet50-int8", "classify", "INT8", 1},
    {"yolov8n-int8", "detect", "INT8", 2},
    {"llm-1b-int4", "llm", "INT4", 3},
};

// Writes a fake "server" under `fixture`: `npu-models/manifest.json` plus one
// file per model, all vendor "intel". With `tamper` the served file bytes don't
// match the declared SHA256, so the downloader must fail closed (P-NET).
// Returns false on any write failure.
bool writeServer(QTemporaryDir &fixture, bool tamper = false)
{
    const QString root = QDir(fixture.path()).filePath(QStringLiteral("npu-models"));
    QJsonArray models;
    for (const ModelFix &m : kModels) {
        const QByteArray good = bytes(m.seed);
        const QString rel = QStringLiteral("intel/%1/%2/1/model.bin").arg(QLatin1StringView(m.workload), QLatin1StringView(m.id));
        const QJsonObject file{{QStringLiteral("path"), rel},
                               {QStringLiteral("size"), good.size()},
                               {QStringLiteral("sha256"), QString::fromLatin1(sha256Hex(good))}};
        const QJsonObject model{{QStringLiteral("id"), QLatin1StringView(m.id)},
                                {QStringLiteral("vendor"), QStringLiteral("intel")},
                                {QStringLiteral("workload"), QLatin1StringView(m.workload)},
                                {QStringLiteral("precision"), QLatin1StringView(m.precision)},
                                {QStringLiteral("files"), QJsonArray{file}}};
        models.append(model);
    }
    const QJsonObject manifest{{QStringLiteral("schema"), 1},
                               {QStringLiteral("pack_version"), QStringLiteral("1")},
                               {QStringLiteral("models"), models}};
    QFile mf(root + QStringLiteral("/manifest.json"));
    if (!QDir().mkpath(root) || !mf.open(QIODevice::WriteOnly))
        return false;
    mf.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    mf.close();

    for (const ModelFix &m : kModels) {
        const QByteArray served = tamper ? bytes(m.seed + 100) : bytes(m.seed);
        const QString path = root + QStringLiteral("/1/intel/%1/%2/1/model.bin").arg(QLatin1StringView(m.workload), QLatin1StringView(m.id));
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(served);
        f.close();
    }
    return true;
}

double num(const TestResult &r, const char *key)
{
    const Metric *m = r.metric(QLatin1StringView(key));
    return m && m->isNumeric() ? std::get<double>(m->value) : std::nan("");
}

} // namespace

class TestNpuTest : public QObject
{
    Q_OBJECT

private slots:
    void identity()
    {
        NpuTest t;
        QCOMPARE(t.info().id, QStringLiteral("npu"));
        QCOMPARE(t.info().name, QStringLiteral("NPU"));
        QCOMPARE(t.info().colorToken, QStringLiteral("t-npu"));
        QCOMPARE(t.info().testVersion, 1);
        QCOMPARE(t.info().formulaVersion, 0);   // placeholder anchors until task 19
        QStringList subs;
        for (const SubTestInfo &s : t.info().subTests) {
            subs << s.id;
            QVERIFY(!s.userSelectable);   // D15: all three, none selectable
        }
        QCOMPARE(subs, (QStringList{QStringLiteral("classify"), QStringLiteral("detect"), QStringLiteral("llm")}));
        QCOMPARE(t.estimate({}), std::chrono::seconds(130));
        QCOMPARE(t.timeLimit({}), std::chrono::seconds(320));
    }

    // The null-engine end-to-end run: detect → download → load → 3 workloads →
    // score, all through the fake manifest (§13, §15).
    void nullEngineEndToEnd()
    {
        qputenv("SYNTHEON_NPU_FAKE_VENDOR", "null");
        qputenv("SYNTHEON_NPU_FAKE_SEED", "42");
        const auto restore = qScopeGuard([] {
            qunsetenv("SYNTHEON_NPU_FAKE_VENDOR");
            qunsetenv("SYNTHEON_NPU_FAKE_SEED");
        });

        QTemporaryDir server;
        QTemporaryDir cache;
        QVERIFY(server.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(writeServer(server));

        NpuTest t;
        FakeRunContext ctx;
        ctx.setNpuModelsBaseUrl(QUrl::fromLocalFile(server.path() + QLatin1Char('/')).toString());
        ctx.setNpuCacheRoot(cache.path());
        const TestResult r = t.run(ctx, {});

        QVERIFY2(r.status == Status::Ok, qPrintable(r.statusReason));
        QCOMPARE(r.subResults.size(), 3);
        for (const SubResult &s : r.subResults)
            QCOMPARE(s.status, Status::Ok);
        QVERIFY(r.metric(QStringLiteral("classify.imgs")));
        QVERIFY(r.metric(QStringLiteral("detect.imgs")));
        QVERIFY(r.metric(QStringLiteral("llm.tps")));
        QVERIFY(r.metric(QStringLiteral("llm.ttft")));
        QVERIFY(num(r, "classify.imgs") > 0.0);
        QVERIFY(num(r, "detect.imgs") > 0.0);
        QVERIFY(num(r, "llm.tps") > 0.0);
        // The model + device neutrals are present for the Detail page.
        QVERIFY(r.metric(QStringLiteral("device.vendor")));
        QVERIFY(r.metric(QStringLiteral("engine.name")));
        QVERIFY(r.metric(QStringLiteral("model.classify")));
        QVERIFY(r.metric(QStringLiteral("models.pack_version")));
        // score() gives a total (the anchors are placeholders, so it is large
        // but finite — only presence is asserted).
        const ScoreBreakdown b = t.score(r);
        QVERIFY(b.total.has_value());
        QVERIFY(b.subScores.size() == 3);
        QVERIFY(!ctx.wentBackwards());
    }

    void noNpuIsNotAvailable()
    {
        // No FAKE_VENDOR and an empty snapshot (no npus[]) → NoNpu → NotAvailable
        // with the §3.3 taxonomy string.
        NpuTest t;
        FakeRunContext ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::NotAvailable);
        QCOMPARE(r.statusReason, QStringLiteral("No supported NPU found on this machine"));
    }

    void cancelMidRun()
    {
        qputenv("SYNTHEON_NPU_FAKE_VENDOR", "null");
        const auto restore = qScopeGuard([] { qunsetenv("SYNTHEON_NPU_FAKE_VENDOR"); });

        QTemporaryDir server;
        QTemporaryDir cache;
        QVERIFY(server.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(writeServer(server));

        NpuTest t;
        FakeRunContext ctx;
        ctx.setNpuModelsBaseUrl(QUrl::fromLocalFile(server.path() + QLatin1Char('/')).toString());
        ctx.setNpuCacheRoot(cache.path());
        ctx.cancelOn(QStringLiteral("Detect · window 2 of 3"));

        const TestResult r = t.run(ctx, {});

        QCOMPARE(r.status, Status::Cancelled);
        QVERIFY(ctx.cancelAt() != Clock::time_point{});
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - ctx.cancelAt()).count();
        QVERIFY2(ms < 250, qPrintable(QStringLiteral("Stop took %1 ms").arg(ms, 0, 'f', 1)));
    }

    void tamperedManifestFailsClosed()
    {
        qputenv("SYNTHEON_NPU_FAKE_VENDOR", "null");
        const auto restore = qScopeGuard([] { qunsetenv("SYNTHEON_NPU_FAKE_VENDOR"); });

        QTemporaryDir server;
        QTemporaryDir cache;
        QVERIFY(server.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(writeServer(server, /*tamper=*/true));

        NpuTest t;
        FakeRunContext ctx;
        ctx.setNpuModelsBaseUrl(QUrl::fromLocalFile(server.path() + QLatin1Char('/')).toString());
        ctx.setNpuCacheRoot(cache.path());
        const TestResult r = t.run(ctx, {});

        // The SHA256 mismatch aborts the download → model_missing N/A, never a
        // run against an unverified model (P-NET / §15).
        QCOMPARE(r.status, Status::NotAvailable);
        QVERIFY2(r.statusReason.contains(QLatin1StringView("model pack")), qPrintable(r.statusReason));
    }
};

QTEST_GUILESS_MAIN(TestNpuTest)
#include "tst_npu_test.moc"

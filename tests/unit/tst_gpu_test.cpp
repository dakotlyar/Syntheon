// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test (Phase 4a task 15) through a fake RunContext, with a fake
// worker (fake_gpuworker.cpp): scene-by-scene order, the interface hold,
// monotonic progress, Stop within 250 ms, N/A and Error mapping (D6), the
// estimate and the time limit.
#include "core/units.h"
#include "fake_test.h"
#include "privileged/privileged_ops.h"
#include "tests/common/stub_clock.h"
#include "tests/gpu/client/gpu_client.h"
#include "tests/gpu/formula_v1.h"
#include "tests/gpu/gpu_test.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <mutex>
#include <thread>

using namespace sb;
using namespace sb::tests;
namespace pr = sb::gpu::protocol;

namespace {

const QString kFake = QStringLiteral(SB_FAKE_GPUWORKER);

class Ctx : public RunContext
{
public:
    void progress(double fraction, const QString &) override
    {
        std::lock_guard lock(m_mutex);
        if (!m_fractions.isEmpty() && fraction + 1e-9 < m_fractions.last())
            m_backwards = true;
        m_fractions.append(fraction);
    }
    void setPart(const QString &part) override
    {
        std::lock_guard lock(m_mutex);
        m_parts.append(part);
    }
    void holdInterface(bool hold) override
    {
        std::lock_guard lock(m_mutex);
        m_holds.append(hold);
    }
    void log(LogLevel, const QString &message) override
    {
        std::lock_guard lock(m_mutex);
        m_logs.append(message);
    }
    bool cancelled() const override { return m_cancel.load(); }
    void subTestBegin(const QString &) override {}
    void subTestEnd(const QString &, Status) override {}
    void checkpoint(const QCborMap &) override {}
    QString scratchDir() override { return {}; }
    const SystemSnapshot &system() const override { return m_system; }
    PrivilegedOps &privileged() override { return m_priv; }

    bool backwards() const { return m_backwards; }
    QVector<bool> holds() const { return m_holds; }
    QStringList parts() const { return m_parts; }
    QVector<double> fractions() const { return m_fractions; }
    QStringList logs() const { return m_logs; }
    void cancel() { m_cancel = true; }

private:
    mutable std::mutex m_mutex;
    QVector<double> m_fractions;
    QVector<bool> m_holds;
    QStringList m_parts, m_logs;
    bool m_backwards = false;
    std::atomic<bool> m_cancel{false};
    SystemSnapshot m_system;
    UnavailablePrivilegedOps m_priv;
};

double number(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    return m && m->isNumeric() ? std::get<double>(m->value) : -1.0;
}

QString textOf(const TestResult &r, const QString &key)
{
    const Metric *m = r.metric(key);
    return m && !m->isNumeric() ? std::get<QString>(m->value) : QString();
}


// --- Detail page (task 34) -------------------------------------------------------------------

void addNumber(TestResult &r, const QString &key, double v, const QString &unit)
{
    Metric m;
    m.key = key;
    m.label = key;
    m.value = v;
    m.unit = unit;
    r.metrics.append(m);
}

void addText(TestResult &r, const QString &key, const QString &v)
{
    Metric m;
    m.key = key;
    m.label = key;
    m.value = v;
    r.metrics.append(m);
}

// What the orchestrator does after a run: the score and the sub-scores from ITest::score().
TestResult scored(const GpuTest &t, TestResult r)
{
    const ScoreBreakdown b = t.score(r);
    r.score = b.total;
    for (SubResult &s : r.subResults) {
        if (b.subScores.contains(s.id))
            s.score = b.subScores.value(s.id);
    }
    return r;
}

// A Windows-like run with made-up numbers (the mockup's): Vulkan best in Geometry and Particles, DX12 in Draw calls
// and Shading. Every API OK, Direct2D and Qt raster too.
TestResult windowsRun(const GpuTest &t)
{
    TestResult r;
    r.status = Status::Ok;
    r.durationMs = 604000;
    addText(r, QStringLiteral("gpu.name"), QStringLiteral("AMD Radeon RX 7800 XT"));
    addText(r, QStringLiteral("gpu.kind"), QStringLiteral("discrete"));
    addText(r, QStringLiteral("gpu.vram"), QStringLiteral("16 GB"));
    addText(r, QStringLiteral("render.size"), QStringLiteral("1920×1080"));
    addNumber(r, QStringLiteral("render.scale"), 100, QStringLiteral("%"));
    addText(r, QStringLiteral("window.size"), QStringLiteral("1920×1080"));
    const char *apis[] = {"vulkan", "dx12", "dx11", "opengl"};
    const char *scenes[] = {"geometry", "drawcalls", "shading", "particles"};
    const double fps[4][4] = {{412, 181, 291, 341}, {405, 188, 296, 336}, {371, 92.4, 284, 318}, {344, 71.6, 262, 297}};
    for (int a = 0; a < 4; ++a) {
        addText(r, QStringLiteral("api.%1.status").arg(QLatin1String(apis[a])), QStringLiteral("OK"));
        addText(r, QStringLiteral("api.%1.driver").arg(QLatin1String(apis[a])), QStringLiteral("32.0.21025.10"));
        for (int s = 0; s < 4; ++s) {
            const QString k = QStringLiteral("scenes3d.%1.%2.").arg(QLatin1String(apis[a]), QLatin1String(scenes[s]));
            addNumber(r, k + QStringLiteral("fps"), fps[a][s], QStringLiteral("fps"));
            addNumber(r, k + QStringLiteral("low"), fps[a][s] * 0.85, QStringLiteral("fps"));
            addNumber(r, k + QStringLiteral("gpu"), 2.35 + a * 0.1, QStringLiteral("ms"));
            addNumber(r, k + QStringLiteral("cpu"), s == 1 ? 5.0 + a * 3 : 0.3, QStringLiteral("ms"));
        }
    }
    const QString names[] = {QStringLiteral("Vulkan"), QStringLiteral("DirectX 12"), QStringLiteral("DirectX 11"), QStringLiteral("OpenGL")};
    for (int s = 0; s < 4; ++s) {
        int best = 0;
        for (int a = 1; a < 4; ++a)
            best = fps[a][s] > fps[best][s] ? a : best;
        const QString k = QStringLiteral("scenes3d.%1.").arg(QLatin1String(scenes[s]));
        addNumber(r, k + QStringLiteral("fps"), fps[best][s], QStringLiteral("fps"));
        addNumber(r, k + QStringLiteral("low"), fps[best][s] * 0.85, QStringLiteral("fps"));
        addText(r, k + QStringLiteral("api"), names[best]);
        Series ser{QStringLiteral("ft.%1.%2").arg(QLatin1String(apis[best]), QLatin1String(scenes[s])), QStringLiteral("x"), QStringLiteral("Time"),
                   QStringLiteral("s"), QStringLiteral("ms"), {{0.05, 2.4}, {0.15, 2.5}}};
        r.series.append(ser);
    }
    for (int a = 0; a < 4; ++a) {
        addNumber(r, QStringLiteral("fill2d.%1").arg(QLatin1String(apis[a])), 79.6 + a, QStringLiteral("Gpx/s"));
        addNumber(r, QStringLiteral("fill2d.%1.sprites").arg(QLatin1String(apis[a])), 31.4 - a, QStringLiteral("MOps/s"));
    }
    addNumber(r, QStringLiteral("fill2d.quads"), 82.6, QStringLiteral("Gpx/s"));
    addText(r, QStringLiteral("fill2d.quads.api"), QStringLiteral("OpenGL"));
    addNumber(r, QStringLiteral("fill2d.sprites"), 31.4, QStringLiteral("MOps/s"));
    addText(r, QStringLiteral("fill2d.sprites.api"), QStringLiteral("Vulkan"));
    const char *natives[] = {"shapes", "text", "blits", "gradients"};
    for (int w = 0; w < 4; ++w) {
        addNumber(r, QStringLiteral("native2d.qt.%1").arg(QLatin1String(natives[w])), 0.41 + w, QStringLiteral("MOps/s"));
        addNumber(r, QStringLiteral("native2d.d2d.%1").arg(QLatin1String(natives[w])), 1.62 + w, QStringLiteral("MOps/s"));
        addNumber(r, QStringLiteral("native2d.%1").arg(QLatin1String(natives[w])), 0.41 + w, QStringLiteral("MOps/s"));
    }
    addText(r, QStringLiteral("api.d2d.status"), QStringLiteral("OK"));
    addText(r, QStringLiteral("api.qt.status"), QStringLiteral("OK"));
    r.subResults = {{QStringLiteral("scenes3d"), Status::Ok, {}, std::nullopt},
                    {QStringLiteral("native2d"), Status::Ok, {}, std::nullopt},
                    {QStringLiteral("fill2d"), Status::Ok, {}, std::nullopt}};
    addNumber(r, QStringLiteral("score.3d"), 1244, QStringLiteral("points"));
    addNumber(r, QStringLiteral("score.2d"), 1294, QStringLiteral("points"));
    TestResult done = scored(t, r);
    done.score = 1254;  // the mockup's
    done.subResults[0].score = 1244;
    done.subResults[1].score = 1293;
    done.subResults[2].score = 1295;
    return done;
}

template <typename T>
QVector<const T *> blocksOf(const DetailLayout &d)
{
    QVector<const T *> out;
    for (const DetailBlock &b : d.blocks) {
        if (const auto *p = std::get_if<T>(&b.content))
            out.append(p);
    }
    return out;
}

int indexOfTable(const DetailLayout &d, const QString &title)
{
    for (int i = 0; i < d.blocks.size(); ++i) {
        if (d.blocks[i].title == title && std::holds_alternative<TableBlock>(d.blocks[i].content))
            return i;
    }
    return -1;
}

const TableBlock &tableOf(const DetailLayout &d, const QString &title)
{
    static const TableBlock none;
    const int i = indexOfTable(d, title);
    return i < 0 ? none : std::get<TableBlock>(d.blocks[i].content);
}

QString cell(const TableBlock &t, int row, int col)
{
    return t.rows.value(row).value(col).text;
}

const NoteBlock *noteContaining(const DetailLayout &d, const QString &part)
{
    for (const DetailBlock &b : d.blocks) {
        if (const auto *n = std::get_if<NoteBlock>(&b.content); n && n->text.contains(part))
            return n;
    }
    return nullptr;
}

// "detail renders from stored data": every metric a cell names and every series a chart names is in the result.
void checkReferences(const DetailLayout &d, const TestResult &r)
{
    for (const DetailBlock &b : d.blocks) {
        if (const auto *t = std::get_if<TableBlock>(&b.content)) {
            for (const auto &row : t->rows) {
                for (const DetailCell &c : row)
                    QVERIFY2(c.metricKey.isEmpty() || r.metric(c.metricKey), qPrintable(c.metricKey));
            }
        } else if (const auto *k = std::get_if<KpisBlock>(&b.content)) {
            for (const KpiItem &i : k->items)
                QVERIFY2(i.value.metricKey.isEmpty() || r.metric(i.value.metricKey), qPrintable(i.value.metricKey));
        } else if (const auto *c = std::get_if<LineChartBlock>(&b.content)) {
            for (const QString &key : c->seriesKeys) {
                const bool found = std::any_of(r.series.begin(), r.series.end(), [&](const Series &s) { return s.key == key; });
                QVERIFY2(found, qPrintable(key));
            }
        }
    }
}

} // namespace

class TestGpuTest : public QObject
{
    Q_OBJECT

    // The probe, as the registry does it before a run.
    static void probed(GpuTest &t)
    {
        sb::testing::FakeProbeContext ctx;
        const Availability a = t.probe(ctx);
        QVERIFY2(a.state != Availability::State::Unavailable, qPrintable(a.reason));
    }

private slots:
    void init()
    {
        tests::setStubTimeScale(0.02);  // 5 s -> 100 ms, 20 s -> 400 ms
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("ok")});
        qunsetenv("SYNBENCH_GPU_FAKE_APIS");
        qunsetenv("SYNBENCH_GPU_WORKER_ENV");
        qunsetenv("SYNBENCH_GPU_MIN_FRAMES");
    }
    void cleanup()
    {
        gpu::GpuClient::setWorkerForTests({});
        qunsetenv("SYNBENCH_GPU_FAKE_APIS");
        qunsetenv("SYNBENCH_GPU_WORKER_ENV");
        qunsetenv("SYNBENCH_GPU_FAKE_D2D");
        qunsetenv("SYNBENCH_GPU_MIN_FRAMES");
        qunsetenv("SYNBENCH_GPU_WORKER_ARGS");
        tests::setStubTimeScale(0.02);
    }

    void estimates()
    {
        QCOMPARE(GpuTest::estimateSeconds(4, 2), 610);
        QCOMPARE(GpuTest::estimateSeconds(2, 1), 310);
        QCOMPARE(GpuTest::estimateSeconds(1, 1), 190);
        QCOMPARE(GpuTest::estimateSeconds(3, 2), 480);
        const auto plan = GpuTest::plan3d();
        QCOMPARE(plan.size(), 4);
        QCOMPARE(plan[0].workload, QStringLiteral("geometry"));
        QCOMPARE(plan[0].warmupMs, 5000);
        QCOMPARE(plan[3].workload, QStringLiteral("particles"));
        QCOMPARE(plan[3].warmupMs, 3000);
    }

    void infoAndOptions()
    {
        GpuTest t;
        QCOMPARE(t.info().id, QStringLiteral("gpu"));
        QCOMPARE(t.info().name, QStringLiteral("GPU"));
        QCOMPARE(t.info().summary, QStringLiteral("3D, 2D and fill-rate on 4 graphics APIs"));
        QCOMPARE(t.info().colorToken, QStringLiteral("t-gpu"));
        QCOMPARE(t.info().subTests.size(), 3);
        for (const SubTestInfo &s : t.info().subTests)
            QVERIFY(!s.userSelectable);
        probed(t);
        sb::testing::FakeProbeContext ctx;
        const OptionSchema o = t.options(ctx, Availability{});
        const OptionDef *gpu = o.find(QStringLiteral("gpu"));
        QVERIFY(gpu);
        QCOMPARE(gpu->kind, OptionKind::DynamicSelect);
        QCOMPARE(gpu->items.size(), 1);
        QCOMPARE(gpu->items[0].detail, QStringLiteral("discrete · 16 GB · Vulkan, OpenGL"));
        QCOMPARE(gpu->defaultValue.toString(), QStringLiteral("10de:2c05@0000:01:00.0"));
        QVERIFY(o.find(QStringLiteral("apis")));
        // DEF-3: the APIs row follows the GPU select (its text for every listed GPU).
        QCOMPARE(o.find(QStringLiteral("apis"))->infoFollows, QStringLiteral("gpu"));
        QVERIFY(o.find(QStringLiteral("apis"))->infoByValue.value(QStringLiteral("10de:2c05@0000:01:00.0")).contains(QLatin1StringView("Vulkan 1.4.351")));
        QVERIFY(o.find(QStringLiteral("window"))->infoValue.contains(QLatin1StringView("vsync off")));
        QVERIFY(o.find(QStringLiteral("runs"))->infoValue.startsWith(QLatin1StringView("Every API and part runs (about ")));
    }

    // Task 15a (M6): the Window row reads the snapshot's primary screen.
    void windowRow()
    {
        GpuTest t;
        probed(t);
        const auto row = [&](int w, int h, double scale, bool withDisplay) {
            sb::testing::FakeProbeContext ctx;
            if (withDisplay) {
                ctx.snapshot.data.insert(QStringLiteral("display"),
                                         QJsonObject{{QStringLiteral("name"), QStringLiteral("DP-3")}, {QStringLiteral("avail_width"), w},
                                                     {QStringLiteral("avail_height"), h}, {QStringLiteral("scale"), scale}});
            }
            const OptionSchema o = t.options(ctx, Availability{});
            return *o.find(QStringLiteral("window"));
        };
        const OptionDef fits = row(2560, 1400, 1.0, true);
        QVERIFY2(fits.infoValue.contains(QStringLiteral("here: 1920×1080 px, fits")), qPrintable(fits.infoValue));
        QVERIFY(fits.infoValue.startsWith(QStringLiteral("1920×1080 logical · vsync off")));
        QCOMPARE(fits.infoTone, Tone::Neutral);
        const OptionDef scaledRow = row(1707, 1019, 1.5, true);
        QVERIFY2(scaledRow.infoValue.contains(QStringLiteral("here: 2880×1620 px, window scaled to fit")), qPrintable(scaledRow.infoValue));
        QCOMPARE(scaledRow.infoTone, Tone::Warn);
        QVERIFY(row(500, 300, 1.0, true).infoValue.contains(QLatin1StringView("too small")));
        const OptionDef none = row(0, 0, 1.0, false);
        QCOMPARE(none.infoValue, QStringLiteral("1920×1080 logical at the screen's scale · vsync off"));
        QCOMPARE(none.infoTone, Tone::Neutral);
    }

    void timeLimitAtRealTime()
    {
#if !defined(Q_OS_WIN)
        GpuTest t;
        probed(t);  // two APIs OK on the fake GPU (Vulkan and OpenGL) + 1 native path
        tests::setStubTimeScale(1.0);
        QCOMPARE(t.estimate({}).count(), 310);
        QCOMPARE(t.timeLimit({}).count(), 680);
#endif
    }

    // A run of every API the fake GPU has (Vulkan and OpenGL): Ok with a score (task 32 completed
    // formula v1), the 3D, native and fill numbers, the hold pair, the parts workload by workload,
    // monotonic progress.
    void allApisRun()
    {
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Ok);
        QVERIFY2(r.statusReason.isEmpty(), qPrintable(r.statusReason));
        QCOMPARE(r.subResults.size(), 3);
        QCOMPARE(r.subResults[0].id, QStringLiteral("scenes3d"));
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QCOMPARE(r.subResults[1].status, Status::Ok);  // native 2D: Qt raster finished all four workloads
        QCOMPARE(r.subResults[2].status, Status::Ok);  // fill-rate: Quads and Sprites on both APIs
        QCOMPARE(number(r, QStringLiteral("scenes3d.geometry.fps")), number(r, QStringLiteral("scenes3d.vulkan.geometry.fps")));
        QCOMPARE(number(r, QStringLiteral("scenes3d.geometry.fps")), 100.0);
        QCOMPARE(textOf(r, QStringLiteral("scenes3d.geometry.api")), QStringLiteral("Vulkan"));
        QVERIFY(r.metric(QStringLiteral("scenes3d.vulkan.fps")));  // the stub key kept: all four scenes are there
        QCOMPARE(textOf(r, QStringLiteral("api.vulkan.status")), QStringLiteral("OK"));
        QCOMPARE(textOf(r, QStringLiteral("api.opengl.status")), QStringLiteral("OK"));
        QVERIFY(r.metric(QStringLiteral("scenes3d.opengl.particles.fps")));
#if !defined(Q_OS_WIN)
        QCOMPARE(textOf(r, QStringLiteral("api.dx12.status")), QStringLiteral("N/A: DirectX is Windows only"));
#endif
        // Native 2D (task 28): Qt raster's MOps/s per workload and as the scored native2d.<w>; Linux has no Direct2D.
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("OK"));
#if defined(Q_OS_WIN)
        // Windows with no d2d probe entry (the fake's list has none): honest text, not "Windows only" (L3).
        QCOMPARE(textOf(r, QStringLiteral("api.d2d.status")), QStringLiteral("N/A: Direct2D wasn't checked (the GPU list came from the fallback)"));
#else
        QCOMPARE(textOf(r, QStringLiteral("api.d2d.status")), QStringLiteral("N/A: Direct2D is Windows only"));
#endif
        QVERIFY(!r.metric(QStringLiteral("native2d.d2d.text")));
        for (const char *w : {"shapes", "text", "blits", "gradients"}) {
            QVERIFY(r.metric(QStringLiteral("native2d.%1").arg(QLatin1String(w))));
            QCOMPARE(number(r, QStringLiteral("native2d.%1").arg(QLatin1String(w))), number(r, QStringLiteral("native2d.qt.%1").arg(QLatin1String(w))));
        }
        // The fake's result is 20 frames in 0.2 s: MOps/s = the UNSCALED ops (§7.1) x 100 / 1e6.
        QVERIFY(std::fabs(number(r, QStringLiteral("native2d.text")) - 8000.0 * 100.0 / 1e6) < 1e-9);
        QVERIFY(std::fabs(number(r, QStringLiteral("native2d.shapes")) - 2000.0 * 100.0 / 1e6) < 1e-9);
        QCOMPARE(textOf(r, QStringLiteral("gpu.name")), QStringLiteral("Fake GPU"));
        bool series = false;
        for (const Series &s : r.series)
            series = series || (s.key == QLatin1StringView("ft.vulkan.geometry") && !s.points.isEmpty());
        QVERIFY(series);
        // score.3d is the 3D sub-score (all four scenes at the provisional anchor).
        const ScoreBreakdown b = t.score(r);
        QVERIFY(b.subScores.contains(QStringLiteral("scenes3d")));
        QCOMPARE(number(r, QStringLiteral("score.3d")), b.subScores.value(QStringLiteral("scenes3d")));
        // The stub keys kept (P1): scenes3d.vulkan.fps is the geometric mean of the four scene fps.
        double logSum = 0;
        for (const char *scene : {"geometry", "drawcalls", "shading", "particles"})
            logSum += std::log(number(r, QStringLiteral("scenes3d.vulkan.%1.fps").arg(QLatin1String(scene))));
        QVERIFY(std::fabs(number(r, QStringLiteral("scenes3d.vulkan.fps")) - std::exp(logSum / 4.0)) < 1e-9);
        // Fill-rate (§6.5, §6.6): the fake's result is 20 frames in 0.2 s at the 320x180 it reports as its render size.
        const double gpx = 128.0 * 320.0 * 180.0 * 100.0 / 1e9, sprites = 100000.0 * 100.0 / 1e6;
        for (const char *api : {"vulkan", "opengl"}) {
            QVERIFY(std::fabs(number(r, QStringLiteral("fill2d.%1").arg(QLatin1String(api))) - gpx) < 1e-9);
            QVERIFY(std::fabs(number(r, QStringLiteral("fill2d.%1.sprites").arg(QLatin1String(api))) - sprites) < 1e-9);
            QCOMPARE(number(r, QStringLiteral("fill2d.%1.quads.fps").arg(QLatin1String(api))), 100.0);
            QCOMPARE(number(r, QStringLiteral("fill2d.%1.sprites.fps").arg(QLatin1String(api))), 100.0);
        }
        QCOMPARE(number(r, QStringLiteral("fill2d.quads")), number(r, QStringLiteral("fill2d.vulkan")));  // the best API's (a tie: the earlier)
        QCOMPARE(number(r, QStringLiteral("fill2d.sprites")), number(r, QStringLiteral("fill2d.vulkan.sprites")));
        QCOMPARE(textOf(r, QStringLiteral("fill2d.quads.api")), QStringLiteral("Vulkan"));
        QCOMPARE(textOf(r, QStringLiteral("fill2d.sprites.api")), QStringLiteral("Vulkan"));
        QVERIFY(r.metric(QStringLiteral("fill2d.quads"))->unit == QLatin1StringView("Gpx/s"));
        QVERIFY(r.metric(QStringLiteral("fill2d.sprites"))->unit == QLatin1StringView("MOps/s"));
        bool fillSeries = false;
        for (const Series &s : r.series)
            fillSeries = fillSeries || (s.key == QLatin1StringView("ft.opengl.sprites") && !s.points.isEmpty());
        QVERIFY(fillSeries);
        // The scores, from the formula's own anchors: score.2d = round(1000 x sqrt(G_native x G_fill)), the total the
        // three groups at 0.8 / 0.1 / 0.1.
        const auto groupG = [&](const char *id) {
            for (const GeometricGroup &g : gpu::formula_v1::groups()) {
                if (g.id != QLatin1String(id))
                    continue;
                double l = 0;
                for (const GeometricMetric &m : g.metrics)
                    l += m.weight * std::log(number(r, m.metricKey) / m.anchor);
                return std::exp(l);
            }
            return 0.0;
        };
        const double g3 = groupG("scenes3d"), gn = groupG("native2d"), gf = groupG("fill2d");
        QVERIFY(b.total.has_value());
        QVERIFY2(std::fabs(*b.total - 1000.0 * std::exp(0.8 * std::log(g3) + 0.1 * std::log(gn) + 0.1 * std::log(gf))) <= 0.5 + 1e-9, qPrintable(QString::number(*b.total)));
        QCOMPARE(number(r, QStringLiteral("score.2d")), std::round(1000.0 * std::sqrt(gn * gf)));
        QCOMPARE(b.subScores.value(QStringLiteral("fill2d")), std::round(1000.0 * gf));
        QCOMPARE(b.subScores.value(QStringLiteral("scenes3d")), std::round(1000.0 * g3));
        QCOMPARE(b.subScores.value(QStringLiteral("native2d")), std::round(1000.0 * gn));
        QCOMPARE(ctx.holds(), (QVector<bool>{true, false}));
        QCOMPARE(ctx.parts(), (QStringList{QStringLiteral("Starting"), QStringLiteral("Geometry · Vulkan"), QStringLiteral("Geometry · OpenGL"),
                                           QStringLiteral("Draw calls · Vulkan"), QStringLiteral("Draw calls · OpenGL"),
                                           QStringLiteral("Shading · Vulkan"), QStringLiteral("Shading · OpenGL"),
                                           QStringLiteral("Particles + post · Vulkan"), QStringLiteral("Particles + post · OpenGL"),
                                           QStringLiteral("Quads · Vulkan"), QStringLiteral("Quads · OpenGL"),
                                           QStringLiteral("Sprites · Vulkan"), QStringLiteral("Sprites · OpenGL"),
                                           QStringLiteral("Native 2D · Qt raster")}));  // the fills after the 3D scenes, native 2D last (§8)
        QVERIFY(!ctx.backwards());
        QVERIFY(ctx.fractions().last() < 1.0);
    }

    void stopMidRun()
    {
        GpuTest t;
        probed(t);
        Ctx ctx;
        std::atomic<qint64> cancelAt{0};
        QElapsedTimer clock;
        clock.start();
        std::thread stopper([&] {
            QThread::msleep(700);
            cancelAt = clock.elapsed();
            ctx.cancel();
        });
        const TestResult r = t.run(ctx, {});
        const qint64 returned = clock.elapsed();
        stopper.join();
        QCOMPARE(r.status, Status::Cancelled);
        QVERIFY2(returned - cancelAt <= 250, qPrintable(QString::number(returned - cancelAt)));
        QCOMPARE(ctx.holds().last(), false);
        // L4: what the Stop cut short is "Stopped", never "OK" (3D API and native path alike).
        QCOMPARE(textOf(r, QStringLiteral("api.vulkan.status")), QStringLiteral("Stopped"));
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("Stopped"));
    }

    void notAvailableWhenTheApiIs()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("na")});
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::NotAvailable);
        QVERIFY2(textOf(r, QStringLiteral("api.vulkan.status")).startsWith(QLatin1StringView("N/A: ")), qPrintable(textOf(r, QStringLiteral("api.vulkan.status"))));
        QCOMPARE(textOf(r, QStringLiteral("api.vulkan.status")), QStringLiteral("N/A: No Vulkan driver is installed"));
    }

    // D12: each scene on every API before the next scene.
    void sceneByScene()
    {
        QTemporaryDir dir;
        const QString log = dir.filePath(QStringLiteral("commands.txt"));
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        qputenv("SYNBENCH_GPU_WORKER_ENV", ("SB_FAKE_LOG=" + log).toUtf8());
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QFile f(log);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).remove(QLatin1Char('\r')).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines, (QStringList{QStringLiteral("vulkan geometry"), QStringLiteral("opengl geometry"), QStringLiteral("vulkan drawcalls"),
                                     QStringLiteral("opengl drawcalls"), QStringLiteral("vulkan shading"), QStringLiteral("opengl shading"),
                                     QStringLiteral("vulkan particles"), QStringLiteral("opengl particles"),
                                     QStringLiteral("vulkan quads"), QStringLiteral("opengl quads"), QStringLiteral("vulkan sprites"),
                                     QStringLiteral("opengl sprites")}));
        QCOMPARE(ctx.holds(), (QVector<bool>{true, false}));
        QVERIFY(!ctx.backwards());
    }

    // D6: the second API crashing at shading makes the result Partial with no score; every
    // Vulkan number is still there and the sub-test is an Error.
    void secondApiCrashes()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-at-shading")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY2(r.statusReason.startsWith(QLatin1StringView("No GPU score: OpenGL failed in Shading: the GPU worker exited with code")),
                 qPrintable(r.statusReason));
        QCOMPARE(r.subResults[0].status, Status::Error);
        QVERIFY(textOf(r, QStringLiteral("api.opengl.status")).startsWith(QLatin1StringView("Error: ")));
        for (const char *scene : {"geometry", "drawcalls", "shading", "particles"})
            QVERIFY(r.metric(QStringLiteral("scenes3d.vulkan.%1.fps").arg(QLatin1String(scene))));
        QVERIFY(r.metric(QStringLiteral("scenes3d.opengl.drawcalls.fps")));      // what it measured before the crash
        QVERIFY(!r.metric(QStringLiteral("scenes3d.opengl.shading.fps")));
        QVERIFY(!t.score(r).subScores.contains(QStringLiteral("scenes3d")));    // a group that isn't Ok has no sub-score
        QVERIFY(!r.metric(QStringLiteral("score.3d")));
        // The fill workloads ran on Vulkan alone (OpenGL was dropped), and that is enough for fill2d (the error was in a 3D scene).
        QCOMPARE(r.subResults[2].status, Status::Ok);
        QVERIFY(r.metric(QStringLiteral("fill2d.vulkan.sprites")));
        QVERIFY(!r.metric(QStringLiteral("fill2d.opengl")));
        QCOMPARE(textOf(r, QStringLiteral("fill2d.quads.api")), QStringLiteral("Vulkan"));
        QVERIFY(r.metric(QStringLiteral("score.2d")));  // native 2D and fill-rate are both Ok: the 2D score stands
    }

    // D20 for the fill workloads: OpenGL draws Quads differently (and faster): `fill2d.opengl.quads.match`, an API Error on the
    // fill2d sub-test (the scenes are fine), never the best number, and no score.
    void fillSignatureMismatch()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("mismatch-fill")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY2(textOf(r, QStringLiteral("api.opengl.status")).startsWith(QLatin1StringView("Error: drew Textured quads differently from Vulkan")),
                 qPrintable(textOf(r, QStringLiteral("api.opengl.status"))));
        QCOMPARE(textOf(r, QStringLiteral("fill2d.opengl.quads.match")), textOf(r, QStringLiteral("api.opengl.status")).mid(7));
        QVERIFY(number(r, QStringLiteral("fill2d.opengl")) > number(r, QStringLiteral("fill2d.vulkan")));  // stored, and faster
        QCOMPARE(number(r, QStringLiteral("fill2d.quads")), number(r, QStringLiteral("fill2d.vulkan")));    // but not the headline
        QCOMPARE(textOf(r, QStringLiteral("fill2d.quads.api")), QStringLiteral("Vulkan"));
        QVERIFY(!r.metric(QStringLiteral("fill2d.opengl.sprites")));  // the API stopped there
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QCOMPARE(r.subResults[1].status, Status::Ok);
        QCOMPARE(r.subResults[2].status, Status::Error);
        QVERIFY2(r.statusReason.startsWith(QLatin1StringView("No GPU score: OpenGL failed in Textured quads: drew Textured quads differently from Vulkan")),
                 qPrintable(r.statusReason));
        const ScoreBreakdown b = t.score(r);
        QVERIFY(!b.total);
        QVERIFY(b.subScores.contains(QStringLiteral("scenes3d")) && !b.subScores.contains(QStringLiteral("fill2d")));
        QVERIFY(!r.metric(QStringLiteral("score.2d")));
    }

    // An API crashing in a fill workload: the fill2d sub-test is the Error, the four scenes stay Ok.
    void fillCrashIsPartial()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-at-quads")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY2(r.statusReason.startsWith(QLatin1StringView("No GPU score: OpenGL failed in Textured quads: the GPU worker exited with code")),
                 qPrintable(r.statusReason));
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QCOMPARE(r.subResults[2].status, Status::Error);
        QVERIFY(r.metric(QStringLiteral("scenes3d.opengl.particles.fps")));
        QVERIFY(!r.metric(QStringLiteral("fill2d.opengl")));
        QVERIFY(!t.score(r).total);
        QVERIFY(!r.metric(QStringLiteral("score.2d")));
        QVERIFY(r.metric(QStringLiteral("score.3d")));
    }
    // M2: the faster API that drew the scene differently is the outlier (an Error) and never
    // the headline "best API" number.
    void mismatchingApiIsNeverBest()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("mismatch")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY(textOf(r, QStringLiteral("api.opengl.status")).startsWith(QLatin1StringView("Error: drew Geometry differently from Vulkan")));
        QCOMPARE(number(r, QStringLiteral("scenes3d.opengl.geometry.fps")), 200.0);  // still stored
        QVERIFY(r.metric(QStringLiteral("scenes3d.opengl.geometry.match")));
        QCOMPARE(number(r, QStringLiteral("scenes3d.geometry.fps")), 100.0);          // Vulkan's, not the faster one
        QCOMPARE(textOf(r, QStringLiteral("scenes3d.geometry.api")), QStringLiteral("Vulkan"));
        QCOMPARE(r.subResults[0].status, Status::Error);
    }

    // L2: when every API died in a 3D scene, fill2d says why it has no numbers, not the generic "not every fill workload ran".
    void fillReasonWhenEveryApiDiedIn3d()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-all-at-shading")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.subResults[0].status, Status::Error);
        QCOMPARE(r.subResults[2].status, Status::Error);
        QVERIFY2(r.subResults[2].reason.startsWith(QLatin1StringView("no API ran the fill workloads (Vulkan failed in Shading: the GPU worker exited with code")),
                 qPrintable(r.subResults[2].reason));
        QVERIFY2(r.subResults[0].reason.startsWith(QLatin1StringView("Vulkan failed in Shading: ")), qPrintable(r.subResults[0].reason));
        QCOMPARE(r.status, Status::Partial);  // native 2D still ran
    }

    // L3: under the dev hook's --content-scale, the fill rates count the layers and sprites the worker really drew
    // (max(1, ceil(n / scale)): 2 layers and 1,563 sprites at 64), not the full 128 and 100,000.
    void fillRatesFollowTheContentScale()
    {
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        qputenv("SYNBENCH_GPU_WORKER_ARGS", "--allow-software --content-scale 64");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        qunsetenv("SYNBENCH_GPU_WORKER_ARGS");
        QCOMPARE(r.status, Status::Ok);
        // The fake's 20 frames in 0.2 s at 320x180.
        QVERIFY(std::fabs(number(r, QStringLiteral("fill2d.vulkan")) - 2.0 * 320.0 * 180.0 * 100.0 / 1e9) < 1e-12);
        QVERIFY(std::fabs(number(r, QStringLiteral("fill2d.vulkan.sprites")) - 1563.0 * 100.0 / 1e6) < 1e-12);
    }

    // §9 (H1): fewer than 20 measured frames is an API Error "too few frames to measure (n)" in that workload; its numbers
    // aren't stored, and the API's remaining workloads are skipped.
    void tooFewFramesIsAnApiError()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("few-frames")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QCOMPARE(r.statusReason, QStringLiteral("No GPU score: OpenGL failed in Shading: too few frames to measure (19)"));
        QCOMPARE(textOf(r, QStringLiteral("api.opengl.status")), QStringLiteral("Error: too few frames to measure (19)"));
        QCOMPARE(r.subResults[0].status, Status::Error);
        QVERIFY(r.metric(QStringLiteral("scenes3d.opengl.drawcalls.fps")));
        QVERIFY(!r.metric(QStringLiteral("scenes3d.opengl.shading.fps")));
        QVERIFY(!r.metric(QStringLiteral("scenes3d.opengl.shading.frames")));
        QCOMPARE(textOf(r, QStringLiteral("scenes3d.shading.api")), QStringLiteral("Vulkan"));
        QVERIFY(!r.metric(QStringLiteral("fill2d.opengl")));  // skipped after the Error
        QCOMPARE(r.subResults[2].status, Status::Ok);
        QVERIFY(!t.score(r).total);
        // The CI smoke's dev hook lowers the floor (never in a packaged build): the same 19 frames are then accepted.
        qputenv("SYNBENCH_GPU_MIN_FRAMES", "10");
        Ctx ctx2;
        const TestResult lowered = t.run(ctx2, {});
        QCOMPARE(lowered.status, Status::Ok);
        QCOMPARE(number(lowered, QStringLiteral("scenes3d.opengl.shading.frames")), 19.0);
    }

    // §9 (H1): a frame faster than 0.005 ms, or more than 100,000 fps, is an API Error "implausible frame rate".
    void implausibleFrameRateIsAnApiError_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("workload");
        QTest::addColumn<QString>("key");
        QTest::addColumn<int>("errorSub");
        QTest::newRow("a 0.004 ms frame in a scene") << QStringLiteral("implausible") << QStringLiteral("Draw calls")
                                                     << QStringLiteral("scenes3d.opengl.drawcalls.fps") << 0;
        QTest::newRow("150,000 fps in a fill") << QStringLiteral("fast-fps") << QStringLiteral("Sprites") << QStringLiteral("fill2d.opengl.sprites") << 2;
    }
    void implausibleFrameRateIsAnApiError()
    {
        QFETCH(QString, mode);
        QFETCH(QString, workload);
        QFETCH(QString, key);
        QFETCH(int, errorSub);
        gpu::GpuClient::setWorkerForTests(kFake, {mode});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QCOMPARE(r.statusReason, QStringLiteral("No GPU score: OpenGL failed in %1: implausible frame rate").arg(workload));
        QCOMPARE(textOf(r, QStringLiteral("api.opengl.status")), QStringLiteral("Error: implausible frame rate"));
        QVERIFY(!r.metric(key));
        QCOMPARE(r.subResults[errorSub].status, Status::Error);
        QCOMPARE(r.subResults[2 - errorSub].status, Status::Ok);
    }

    // §9 (H1): a median GPU time over 1.5 x the p50 frame time is dropped (no `gpu` key) with a warning note on that API;
    // it isn't an error, so the score stands. A plausible GPU time is stored as before.
    void implausibleGpuTimeIsDroppedWithAWarning()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("bad-gpu-time")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Ok);
        QVERIFY(t.score(r).total);
        for (const char *scene : {"geometry", "drawcalls", "shading", "particles"}) {
            QVERIFY(!r.metric(QStringLiteral("scenes3d.vulkan.%1.gpu").arg(QLatin1String(scene))));
            QCOMPARE(number(r, QStringLiteral("scenes3d.opengl.%1.gpu").arg(QLatin1String(scene))), 9.0);
        }
        QCOMPARE(textOf(r, QStringLiteral("api.vulkan.note")), QStringLiteral("GPU timestamps look wrong; GPU time not shown"));
        QVERIFY(!r.metric(QStringLiteral("api.opengl.note")));
        QCOMPARE(textOf(r, QStringLiteral("api.vulkan.status")), QStringLiteral("OK"));
    }

    // Task 28, D8: native2d.<w> is Qt raster's number even when Direct2D's is larger; Direct2D is stored and shown, not scored.
    void qtRasterIsTheScoredNativePath()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("d2d-fast")});
        qputenv("SYNBENCH_GPU_FAKE_D2D", "1");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QVERIFY(number(r, QStringLiteral("native2d.d2d.text")) > number(r, QStringLiteral("native2d.qt.text")));
        QCOMPARE(number(r, QStringLiteral("native2d.text")), number(r, QStringLiteral("native2d.qt.text")));
        QVERIFY(!r.metric(QStringLiteral("native2d.text.path")));  // retired: there is no best-of-two
        QCOMPARE(textOf(r, QStringLiteral("api.d2d.status")), QStringLiteral("OK"));
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("OK"));
        QCOMPARE(r.subResults[1].status, Status::Ok);
        // The parts: Direct2D before Qt raster, after the 3D scenes.
        const QStringList parts = ctx.parts();
        QVERIFY(parts.indexOf(QStringLiteral("Native 2D · Direct2D")) > parts.indexOf(QStringLiteral("Particles + post · OpenGL")));
        QCOMPARE(parts.indexOf(QStringLiteral("Native 2D · Qt raster")), parts.indexOf(QStringLiteral("Native 2D · Direct2D")) + 1);
        qunsetenv("SYNBENCH_GPU_FAKE_D2D");
    }

    // D23 a: a Direct2D crash is a warning note and its own status; the test's status and the native2d sub-result are untouched.
    void direct2dCrashIsOnlyAWarning()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-d2d")});
        qputenv("SYNBENCH_GPU_FAKE_D2D", "1");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QVERIFY2(textOf(r, QStringLiteral("api.d2d.status")).startsWith(QLatin1StringView("Error: ")), qPrintable(textOf(r, QStringLiteral("api.d2d.status"))));
        const QString note = textOf(r, QStringLiteral("api.d2d.note"));
        QVERIFY2(note.startsWith(QLatin1StringView("Direct2D failed during start-up (")) && note.endsWith(QLatin1StringView("It isn't scored, so the GPU score stands.")), qPrintable(note));
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QCOMPARE(r.subResults[1].status, Status::Ok);  // Qt raster finished all four
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("OK"));
        QVERIFY(r.metric(QStringLiteral("native2d.text")));
        QVERIFY2(!r.statusReason.contains(QLatin1StringView("Direct2D")), qPrintable(r.statusReason));  // never an API error for D6
        QCOMPARE(r.status, Status::Ok);  // Direct2D isn't scored: the result keeps its score
        QVERIFY(r.statusReason.isEmpty());
        QVERIFY(t.score(r).total.has_value());
        qunsetenv("SYNBENCH_GPU_FAKE_D2D");
    }

    // A worker's `warning` event (Direct2D's font fallback) reaches the result as the D2D note, and stays a warning.
    void direct2dWarningEventIsANote()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("warn-d2d")});
        qputenv("SYNBENCH_GPU_FAKE_D2D", "1");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(textOf(r, QStringLiteral("api.d2d.status")), QStringLiteral("OK"));
        QVERIFY2(textOf(r, QStringLiteral("api.d2d.note")).contains(QLatin1StringView("Segoe UI")), qPrintable(textOf(r, QStringLiteral("api.d2d.note"))));
        QCOMPARE(r.subResults[1].status, Status::Ok);
        qunsetenv("SYNBENCH_GPU_FAKE_D2D");
    }

    // L2: all four results delivered, then a bad exit: the numbers stand, with a note (Qt raster stays Ok; Direct2D only a note).
    void exitAfterFinishingKeepsTheResults()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("exit-after-finish")});
        qputenv("SYNBENCH_GPU_FAKE_D2D", "1");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.subResults[1].status, Status::Ok);
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("OK"));
        QVERIFY2(textOf(r, QStringLiteral("api.qt.note")).contains(QLatin1StringView("the Qt raster worker exited with code 1 after finishing")),
                 qPrintable(textOf(r, QStringLiteral("api.qt.note"))));
        QVERIFY(textOf(r, QStringLiteral("api.d2d.note")).contains(QLatin1StringView("the Direct2D worker exited with code 1 after finishing")));
        QVERIFY(r.metric(QStringLiteral("native2d.text")));
        QVERIFY(r.metric(QStringLiteral("native2d.d2d.text")));
        qunsetenv("SYNBENCH_GPU_FAKE_D2D");
    }

    // A worker that dies before any result: "during start-up", not a workload name.
    void startupFailureSaysSo()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-qt")});
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QVERIFY2(r.statusReason.startsWith(QLatin1StringView("No GPU score: Qt raster failed during start-up: ")), qPrintable(r.statusReason));
    }

    // §9 on native 2D (Dave): an implausible frame rate is that path's Error in that workload. Qt raster's is the native2d
    // sub-test's Error (Partial, no score), like any Qt raster error.
    void implausibleNativeRateIsQtRastersError()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("implausible-qt")});
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QCOMPARE(r.statusReason, QStringLiteral("No GPU score: Qt raster failed in Text: implausible frame rate"));
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QCOMPARE(r.subResults[1].status, Status::Error);
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("Error: implausible frame rate"));
        QVERIFY(r.metric(QStringLiteral("native2d.qt.shapes")));  // the workloads before it stand
        QVERIFY(!r.metric(QStringLiteral("native2d.qt.text")));
        QVERIFY(!r.metric(QStringLiteral("native2d.text")));
        QVERIFY(!t.score(r).total);
    }

    // The same on Direct2D is a warning and its own status only (D23 a): the result stays Ok with its score.
    void implausibleDirect2dRateIsOnlyAWarning()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("implausible-d2d")});
        qputenv("SYNBENCH_GPU_FAKE_D2D", "1");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(textOf(r, QStringLiteral("api.d2d.status")), QStringLiteral("Error: implausible frame rate"));
        const QString note = textOf(r, QStringLiteral("api.d2d.note"));
        QVERIFY2(note.startsWith(QLatin1StringView("Direct2D failed during Text (implausible frame rate)")), qPrintable(note));
        QVERIFY(!r.metric(QStringLiteral("native2d.d2d.text")));
        QCOMPARE(r.subResults[1].status, Status::Ok);
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("OK"));
        QCOMPARE(r.status, Status::Ok);
        QVERIFY(r.statusReason.isEmpty());
        QVERIFY(t.score(r).total.has_value());
        qunsetenv("SYNBENCH_GPU_FAKE_D2D");
    }

    // The 20-frame floor does not apply to native 2D: 5 frames per workload is accepted.
    void fewNativeFramesAreAccepted()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("few-native")});
        qputenv("SYNBENCH_GPU_FAKE_D2D", "1");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Ok);
        QCOMPARE(r.subResults[1].status, Status::Ok);
        QCOMPARE(textOf(r, QStringLiteral("api.qt.status")), QStringLiteral("OK"));
        QCOMPARE(textOf(r, QStringLiteral("api.d2d.status")), QStringLiteral("OK"));
        for (const char *w : {"shapes", "text", "blits", "gradients"}) {
            QVERIFY(r.metric(QStringLiteral("native2d.qt.%1").arg(QLatin1String(w))));
            QVERIFY(r.metric(QStringLiteral("native2d.d2d.%1").arg(QLatin1String(w))));
        }
        QVERIFY(r.metric(QStringLiteral("native2d.text")));
        qunsetenv("SYNBENCH_GPU_FAKE_D2D");
    }

    // A Qt raster crash is the native2d sub-test's Error, so the test is Partial with no score.
    void qtRasterCrashIsPartial()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-qt")});
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::Partial);
        QVERIFY2(r.statusReason.startsWith(QLatin1StringView("No GPU score: Qt raster failed during start-up: ")), qPrintable(r.statusReason));
        QCOMPARE(r.subResults[0].status, Status::Ok);
        QCOMPARE(r.subResults[1].status, Status::Error);
        QVERIFY2(textOf(r, QStringLiteral("api.qt.status")).startsWith(QLatin1StringView("Error: ")), qPrintable(textOf(r, QStringLiteral("api.qt.status"))));
        QVERIFY(!r.metric(QStringLiteral("native2d.text")));
        QVERIFY(!t.score(r).subScores.contains(QStringLiteral("native2d")));  // a group that isn't Ok has no sub-score
        QVERIFY(!t.score(r).total);
    }

    // L6: Stop with two APIs running still returns within 250 ms.
    void stopWithTwoApis()
    {
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        std::atomic<qint64> cancelAt{0};
        QElapsedTimer clock;
        clock.start();
        std::thread stopper([&] {
            QThread::msleep(700);
            cancelAt = clock.elapsed();
            ctx.cancel();
        });
        const TestResult r = t.run(ctx, {});
        const qint64 returned = clock.elapsed();
        stopper.join();
        QCOMPARE(r.status, Status::Cancelled);
        QVERIFY2(returned - cancelAt <= 250, qPrintable(QString::number(returned - cancelAt)));
    }

    // L8: when every API says N/A the test's reason is theirs.
    void notAvailableReasonIsTheApis()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("na")});
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = t.run(ctx, {});
        QCOMPARE(r.status, Status::NotAvailable);
        QVERIFY2(r.statusReason.contains(QLatin1StringView("No Vulkan driver is installed")), qPrintable(r.statusReason));
    }

    // --- Detail page (task 34) ---------------------------------------------------------------

    // The Windows Ok layout: four FPS charts, the exact summaries, one bold cell per scene row, the notes in place.
    void detailWindowsOk()
    {
        GpuTest t;
        const TestResult r = windowsRun(t);
        const DetailLayout d = t.detail(r);
        QCOMPARE(d.headlineMetrics, (QStringList{QStringLiteral("score.3d"), QStringLiteral("score.2d")}));
        QCOMPARE(d.subtitle, QStringLiteral("Score 1,254 · AMD Radeon RX 7800 XT · 1920×1080 at 100% · 4 APIs"));
        checkReferences(d, r);
        // The charts: FPS, from the best API's frame-time series.
        const auto charts = blocksOf<LineChartBlock>(d);
        QCOMPARE(charts.size(), 4);
        const QStringList best = {QStringLiteral("vulkan"), QStringLiteral("dx12"), QStringLiteral("dx12"), QStringLiteral("vulkan")};
        const QStringList scenes = {QStringLiteral("geometry"), QStringLiteral("drawcalls"), QStringLiteral("shading"), QStringLiteral("particles")};
        for (int i = 0; i < 4; ++i) {
            QVERIFY(charts[i]->yValue == ChartValue::PerSecondFromMs);
            QCOMPARE(charts[i]->yUnit, QStringLiteral("fps"));
            QCOMPARE(charts[i]->seriesKeys, (QStringList{QStringLiteral("ft.%1.%2").arg(best[i], scenes[i])}));
            QCOMPARE(charts[i]->xAxis.tickLabels, (QStringList{QStringLiteral("0 s"), QStringLiteral("5 s"), QStringLiteral("10 s"),
                                                               QStringLiteral("15 s"), QStringLiteral("20 s")}));
            QVERIFY(charts[i]->annotations.isEmpty() && charts[i]->markers.isEmpty());
        }
        QVERIFY(indexOfTable(d, QStringLiteral("Geometry by API")) >= 0);
        int chartTitles = 0;
        for (const DetailBlock &b : d.blocks) {
            if (std::holds_alternative<LineChartBlock>(b.content))
                chartTitles += b.title == QStringLiteral("Geometry: FPS over time (Vulkan, best API)")
                               || b.title == QStringLiteral("Draw calls: FPS over time (DX12, best API)");
        }
        QCOMPARE(chartTitles, 2);
        // The summaries (D25).
        QCOMPARE(tableOf(d, QStringLiteral("Sub-tests")).summary, QStringLiteral("3D 1,244 · Native 2D 1,293 · Fill-rate 1,295"));
        const TableBlock &scenesTable = tableOf(d, QStringLiteral("3D scenes: average FPS by API"));
        QCOMPARE(scenesTable.summary, QStringLiteral("Best: Vulkan 2 scenes · DX12 2 scenes"));
        QCOMPARE(scenesTable.columns, (QStringList{QStringLiteral("Scene"), QStringLiteral("Vulkan"), QStringLiteral("DX12"),
                                                   QStringLiteral("DX11"), QStringLiteral("OpenGL")}));
        // M1: exactly one bold cell per scene row, in the column of scenes3d.<scene>.api.
        QCOMPARE(scenesTable.boldCells.size(), 4);
        const QVector<QPoint> boldWanted = {{1, 0}, {2, 1}, {2, 2}, {1, 3}};
        QCOMPARE(scenesTable.boldCells, boldWanted);
        QCOMPARE(tableOf(d, QStringLiteral("Geometry by API")).summary, QStringLiteral("Vulkan 412 fps · 1% low 350"));
        QCOMPARE(tableOf(d, QStringLiteral("Draw calls by API")).summary, QStringLiteral("DX12 188 fps · 1% low 160"));
        QCOMPARE(tableOf(d, QStringLiteral("2D fill-rate")).summary, QStringLiteral("Quads 82.6 Gpx/s (OpenGL) · Sprites 31.4 MOps/s (Vulkan)"));
        QCOMPARE(tableOf(d, QStringLiteral("Native 2D (MOps/s)")).summary, QStringLiteral("Qt raster (scored) · sub-score 1,293"));
        QCOMPARE(tableOf(d, QStringLiteral("APIs")).summary, QStringLiteral("6 OK"));
        QCOMPARE(tableOf(d, QStringLiteral("Render setup")).summary, QStringLiteral("1920×1080 at 100% · fits"));
        // The tables' cells.
        QCOMPARE(cell(scenesTable, 1, 3), QStringLiteral("92.4"));
        QCOMPARE(cell(tableOf(d, QStringLiteral("Geometry by API")), 0, 3), QStringLiteral("2.35"));
        // Titles are stable and the order is §15's (the renderer does the rest).
        QStringList titles;
        for (const DetailBlock &b : d.blocks) {
            if (std::holds_alternative<TableBlock>(b.content))
                titles << b.title;
        }
        QCOMPARE(titles, (QStringList{QStringLiteral("Sub-tests"), QStringLiteral("3D scenes: average FPS by API"), QStringLiteral("Geometry by API"),
                                      QStringLiteral("Draw calls by API"), QStringLiteral("Shading by API"), QStringLiteral("Particles by API"),
                                      QStringLiteral("2D fill-rate"), QStringLiteral("Native 2D (MOps/s)"), QStringLiteral("APIs"),
                                      QStringLiteral("Render setup")}));
        // The notes: the fill and native notes are two Neutral notes, each right after its table; the Draw calls note after its table.
        const auto noteAfter = [&](const QString &title) {
            const int i = indexOfTable(d, title);
            return i >= 0 && i + 1 < d.blocks.size() ? std::get_if<NoteBlock>(&d.blocks[i + 1].content) : nullptr;
        };
        const NoteBlock *fill = noteAfter(QStringLiteral("2D fill-rate"));
        QVERIFY(fill);
        QCOMPARE(fill->text, QStringLiteral("Fill-rate: 128 blended full-screen layers per frame, and 100,000 sprites streamed by the CPU every frame; "
                                            "the best API (bold) counts."));
        QVERIFY(fill->tone == Tone::Neutral);
        const NoteBlock *native = noteAfter(QStringLiteral("Native 2D (MOps/s)"));
        QVERIFY(native);
        QCOMPARE(native->text, QStringLiteral("Native 2D: Qt raster draws on the CPU and is scored; Direct2D draws on the GPU and is shown for comparison."));
        QVERIFY(native->tone == Tone::Neutral);
        const NoteBlock *dc = noteAfter(QStringLiteral("Draw calls by API"));
        QVERIFY(dc && dc->text.startsWith(QStringLiteral("Draw calls measures the CPU cost of submitting 38,401 draws per frame (GPU time about 2.5 ms against 5–14 ms")));
        QVERIFY(noteAfter(QStringLiteral("3D scenes: average FPS by API")));
        // No warning about the run on a clean Windows run.
        for (const NoteBlock *n : blocksOf<NoteBlock>(d))
            QVERIFY(n->tone == Tone::Neutral);
    }

    // Linux, two APIs (a real run through the fake worker): no DX columns, the N/A rows, every referenced metric present.
    void detailLinuxTwoApis()
    {
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = scored(t, t.run(ctx, {}));
        QCOMPARE(r.status, Status::Ok);
        const DetailLayout d = t.detail(r);
        checkReferences(d, r);
        QCOMPARE(blocksOf<LineChartBlock>(d).size(), 4);
        const TableBlock &scenes = tableOf(d, QStringLiteral("3D scenes: average FPS by API"));
        QCOMPARE(scenes.columns, (QStringList{QStringLiteral("Scene"), QStringLiteral("Vulkan"), QStringLiteral("OpenGL")}));
        QCOMPARE(scenes.summary, QStringLiteral("Best: Vulkan 4 scenes"));
        QCOMPARE(scenes.boldCells.size(), 4);
        QCOMPARE(cell(scenes, 0, 1), QStringLiteral("100"));
        const TableBlock &apis = tableOf(d, QStringLiteral("APIs"));
        QCOMPARE(apis.rows.size(), 6);
        QCOMPARE(cell(apis, 1, 1), QStringLiteral("N/A"));
        QCOMPARE(cell(apis, 1, 5), QStringLiteral("DirectX is Windows only"));
        QCOMPARE(cell(apis, 5, 5), QStringLiteral("CPU path, scored"));
        QCOMPARE(apis.summary, QStringLiteral("3 OK · 3 N/A"));
        QVERIFY(r.score);
        QVERIFY(d.subtitle.startsWith(QLatin1StringView("Score ")));
        QVERIFY(d.subtitle.endsWith(QLatin1StringView("2 APIs")));
        // The Direct2D column is absent when Direct2D didn't run.
        QCOMPARE(tableOf(d, QStringLiteral("Native 2D (MOps/s)")).columns, (QStringList{QStringLiteral("Workload"), QStringLiteral("Qt raster (scored)")}));
        QCOMPARE(tableOf(d, QStringLiteral("Native 2D (MOps/s)")).boldCells.size(), 4);
    }

    // Partial (an API failed in a scene): "No GPU score" in the subtitle, one Warn note, the Error cells.
    void detailPartialApiError()
    {
        gpu::GpuClient::setWorkerForTests(kFake, {QStringLiteral("crash-at-shading")});
        qputenv("SYNBENCH_GPU_FAKE_APIS", "vulkan,opengl");
        GpuTest t;
        probed(t);
        Ctx ctx;
        const TestResult r = scored(t, t.run(ctx, {}));
        QCOMPARE(r.status, Status::Partial);
        const DetailLayout d = t.detail(r);
        checkReferences(d, r);
        QVERIFY2(d.subtitle.startsWith(QStringLiteral("No GPU score · OpenGL failed in Shading · ")), qPrintable(d.subtitle));
        const NoteBlock *n = noteContaining(d, QStringLiteral("No GPU score: OpenGL failed during Shading ("));
        QVERIFY(n);
        QVERIFY(n->tone == Tone::Warn);
        QVERIFY(n->text.endsWith(QLatin1StringView("). Its other results and every other API are shown.")));
        const TableBlock &scenes = tableOf(d, QStringLiteral("3D scenes: average FPS by API"));
        QCOMPARE(cell(scenes, 2, 2), QStringLiteral("Error"));  // Shading, OpenGL
        const TableBlock &apis = tableOf(d, QStringLiteral("APIs"));
        QCOMPARE(cell(apis, 3, 1), QStringLiteral("Error"));
        QVERIFY2(cell(apis, 3, 5).startsWith(QLatin1StringView("failed during Shading: ")), qPrintable(cell(apis, 3, 5)));
        QVERIFY(apis.summary.contains(QLatin1StringView("1 Error")));
        // No GPU score row / KPI, the sub-scores of the groups that finished stay.
        QVERIFY(!r.score);
        QCOMPARE(cell(tableOf(d, QStringLiteral("Sub-tests")), 0, 2), QStringLiteral("Error"));
        QCOMPARE(tableOf(d, QStringLiteral("Sub-tests")).summary.left(4), QStringLiteral("3D —"));
    }

    // An API that drew a scene differently: the note names it ("No GPU score: DirectX 11 drew …").
    void detailOutlierAndOtherWarnings()
    {
        GpuTest t;
        TestResult r = windowsRun(t);
        r.status = Status::Partial;
        r.statusReason = QStringLiteral("No GPU score: DirectX 11 failed in Particles + post: drew Particles + post differently from Vulkan (mean 9.3/255)");
        r.score.reset();
        for (Metric &m : r.metrics) {
            if (m.key == QLatin1StringView("api.dx11.status"))
                m.value = QStringLiteral("Error: drew Particles + post differently from Vulkan (mean 9.3/255)");
        }
        addText(r, QStringLiteral("api.vulkan.layers"), QStringLiteral("VK_LAYER_OBS_HOOK"));
        addNumber(r, QStringLiteral("background.load"), 12, QStringLiteral("%"));
        addText(r, QStringLiteral("power.source"), QStringLiteral("Battery"));
        const DetailLayout d = t.detail(r);
        const NoteBlock *o = noteContaining(d, QStringLiteral("DirectX 11 drew"));
        QVERIFY(o);
        QCOMPARE(o->text, QStringLiteral("No GPU score: DirectX 11 drew Particles + post differently from Vulkan (mean 9.3/255)."));
        QVERIFY(o->tone == Tone::Warn);
        QCOMPARE(cell(tableOf(d, QStringLiteral("APIs")), 2, 5), QStringLiteral("drew Particles + post differently from Vulkan (mean 9.3/255)"));
        for (const char *part : {"Vulkan layers were active (VK_LAYER_OBS_HOOK)", "On battery", "Background load 12% before the run"}) {
            const NoteBlock *n = noteContaining(d, QLatin1StringView(part));
            QVERIFY2(n, part);
            QVERIFY(n->tone == Tone::Warn);
        }
    }

    // M5: one scaled-to-fit note, and it is Warn (it changes what the FPS means); the Render KPI and summary follow.
    void detailScaledToFit()
    {
        GpuTest t;
        TestResult r = windowsRun(t);
        for (Metric &m : r.metrics) {
            if (m.key == QLatin1StringView("render.size"))
                m.value = QStringLiteral("2880×1620");
            else if (m.key == QLatin1StringView("render.scale"))
                m.value = 150.0;
            else if (m.key == QLatin1StringView("window.size"))
                m.value = QStringLiteral("1640×923 (scaled to fit the screen)");
        }
        const DetailLayout d = t.detail(r);
        const NoteBlock *n = noteContaining(d, QStringLiteral("Scaled to fit"));
        QVERIFY(n);
        QCOMPARE(n->text, QStringLiteral("Scaled to fit this screen at 150%: 2880×1620 rendered per frame, so FPS isn't comparable with 100%."));
        QVERIFY(n->tone == Tone::Warn);
        QCOMPARE(tableOf(d, QStringLiteral("Render setup")).summary, QStringLiteral("2880×1620 at 150% · scaled to fit"));
        const auto kpis = blocksOf<KpisBlock>(d);
        QCOMPARE(kpis.size(), 1);
        QCOMPARE(kpis[0]->items.size(), 5);
        QCOMPARE(kpis[0]->items[3].label, QStringLiteral("Render"));
        QCOMPARE(kpis[0]->items[3].unitOverride, QStringLiteral("at 150% · scaled to fit"));
        // Not scaled to fit, but not 100%: the other wording, also Warn.
        for (Metric &m : r.metrics) {
            if (m.key == QLatin1StringView("window.size"))
                m.value = QStringLiteral("2400×1350");
        }
        const NoteBlock *rendered = noteContaining(t.detail(r), QStringLiteral("Rendered at 150%"));
        QVERIFY(rendered && rendered->tone == Tone::Warn);
    }

    // A Stopped run builds from what is there; nothing is invented.
    void detailCancelledAndEmpty()
    {
        GpuTest t;
        TestResult r = windowsRun(t);
        r.status = Status::Cancelled;
        r.statusReason = defaultStoppedReason();
        r.score.reset();
        for (SubResult &s : r.subResults)
            s.status = Status::Cancelled;
        const DetailLayout d = t.detail(r);
        QVERIFY(d.subtitle.startsWith(QLatin1StringView("Stopped")));
        const NoteBlock *n = noteContaining(d, QStringLiteral("Stopped before the end"));
        QVERIFY(n && n->tone == Tone::Warn);
        QCOMPARE(cell(tableOf(d, QStringLiteral("Sub-tests")), 0, 2), QStringLiteral("Stopped"));
        // A result with nothing in it: no crash, no blocks that need data.
        TestResult empty;
        empty.status = Status::Error;
        const DetailLayout e = t.detail(empty);
        QVERIFY(blocksOf<LineChartBlock>(e).isEmpty());
        QCOMPARE(indexOfTable(e, QStringLiteral("APIs")), -1);
    }
};

QTEST_GUILESS_MAIN(TestGpuTest)
#include "tst_gpu_test.moc"

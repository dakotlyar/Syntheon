// SPDX-License-Identifier: GPL-3.0-or-later
// CPU telemetry (Phase 2 design §8): the Linux sysfs readers on Knowhere's
// captured tree and on hand-made trees, the Windows PDH arithmetic, the 1 Hz
// sampler with a fake source, and the throttling rule.
#include "tests/cpu/telemetry/sampler.h"
#include "tests/cpu/telemetry/telemetry.h"
#include "tests/cpu/telemetry/throttle.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#if defined(Q_OS_LINUX)
#include "fixture_tree.h"
#endif

#include <atomic>
#include <thread>

using namespace sb::cpu::telemetry;
using namespace std::chrono_literals;

namespace {

// Writes `text` to <root>/<relative>, creating the directories.
bool put(const QString &root, const QString &relative, const QByteArray &text)
{
    const QString path = root + QLatin1Char('/') + relative;
    if (!QDir().mkpath(QFileInfo(path).path()))
        return false;
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
}

// A hwmon device <root>/class/hwmon/hwmon<n> named `name` with the given
// temp<index>_input values (millidegrees) and labels ("" = none).
bool hwmon(const QString &root, int n, const char *name, const QList<std::tuple<int, int, QByteArray>> &temps)
{
    const QString dir = QStringLiteral("class/hwmon/hwmon%1/").arg(n);
    bool ok = put(root, dir + QStringLiteral("name"), QByteArray(name) + '\n');
    for (const auto &[index, milli, label] : temps) {
        ok = ok && put(root, dir + QStringLiteral("temp%1_input").arg(index), QByteArray::number(milli) + '\n');
        if (!label.isEmpty())
            ok = ok && put(root, dir + QStringLiteral("temp%1_label").arg(index), label + '\n');
    }
    return ok;
}

// A source whose clock is the scope's CPU number (all CPUs: 1000), so tests
// can see the scope each sample used.
class FakeSource : public Source
{
public:
    std::atomic<int> reads{0};
    std::optional<double> clockMHz(const ClockScope &scope) override
    {
        ++reads;
        return scope.allCpus ? 1000.0 : double(scope.core.cpu);
    }
    std::optional<double> temperatureC() override { return 40.0 + reads.load(); }
    std::string temperatureLabel() const override { return "fake"; }
    std::optional<std::uint64_t> throttleCount() override { return std::nullopt; }
    std::optional<double> performanceLimitPercent() override { return std::nullopt; }
};

WindowTrace window(const char *name, std::vector<double> rates, std::vector<std::optional<double>> clocks,
                   bool multiThread = true, std::optional<double> temp = std::nullopt)
{
    WindowTrace w;
    w.name = name;
    w.multiThread = multiThread;
    w.repRates = std::move(rates);
    w.repClockMHz = std::move(clocks);
    w.peakTemperatureC = temp;
    return w;
}

} // namespace

class TestCpuTelemetry : public QObject
{
    Q_OBJECT

private slots:
#if defined(Q_OS_LINUX)
    void initTestCase()
    {
        QVERIFY(m_trees.isValid());
        QVERIFY(sb::testing::materializeFixture(QStringLiteral(SB_FIXTURES "/linux/knowhere"), m_trees.path()));
        m_knowhere = (m_trees.path() + QStringLiteral("/sys")).toStdString();
    }

    // Knowhere (captured 2026-10-18): amd-pstate's scaling_cur_freq per CPU.
    void knowhereClock()
    {
        QCOMPARE(linuxClockMHz(m_knowhere, {0}).value(), 2773.189);
        std::vector<int> all;
        for (int c = 0; c < 24; ++c)
            all.push_back(c);
        QVERIFY(qAbs(linuxClockMHz(m_knowhere, all).value() - 2145.641042) < 1e-6);
        // CPUs without the file are left out; none at all: unknown.
        QCOMPARE(linuxClockMHz(m_knowhere, {0, 99}).value(), 2773.189);
        QVERIFY(!linuxClockMHz(m_knowhere, {99}));
        QVERIFY(!linuxClockMHz(m_knowhere, {}));
        // The top clock (the RAM test's warm-up, AP-5 L8): cpuinfo_max_freq.
        QCOMPARE(linuxTopClockMHz(m_knowhere, 0).value(), 5057.467);
        QVERIFY(!linuxTopClockMHz(m_knowhere, 99));
    }

    // Knowhere has acpitz, amdgpu, two nvme and k10temp (Tctl): k10temp wins.
    void knowhereTemperature()
    {
        const auto t = linuxTempSource(m_knowhere);
        QVERIFY(t);
        QCOMPARE(QString::fromStdString(t->label), QStringLiteral("k10temp Tctl"));
        QCOMPARE(readHwmonCelsius(t->inputPath).value(), 35.5);
        QVERIFY(!linuxThrottleCount(m_knowhere));  // AMD: no Intel counters
    }
#endif

    // The name priority on hand-made trees.
    void temperaturePriority()
    {
        QTemporaryDir d;
        const QString r = d.path();
        QVERIFY(!linuxTempSource(r.toStdString()));
        QVERIFY(hwmon(r, 0, "nvme", {{1, 30000, "Composite"}}));
        QVERIFY(!linuxTempSource(r.toStdString()));  // not a CPU sensor
        QVERIFY(hwmon(r, 1, "acpitz", {{1, 41000, ""}}));
        QCOMPARE(QString::fromStdString(linuxTempSource(r.toStdString())->label), QStringLiteral("ACPI zone"));
        // Among "-thermal" zones a CPU one wins, whatever the hwmon order
        // (Qualcomm: aoss0, gpuss0, nsp0 come first).
        QVERIFY(hwmon(r, 2, "gpuss0-thermal", {{1, 52000, ""}}));
        QCOMPARE(QString::fromStdString(linuxTempSource(r.toStdString())->label), QStringLiteral("gpuss0-thermal"));
        QVERIFY(hwmon(r, 5, "cpu0-thermal", {{1, 54000, ""}}));
        QCOMPARE(QString::fromStdString(linuxTempSource(r.toStdString())->label), QStringLiteral("cpu0-thermal"));
        QVERIFY(hwmon(r, 3, "coretemp", {{2, 61000, "Core 0"}, {1, 63000, "Package id 0"}}));
        auto t = linuxTempSource(r.toStdString());
        QCOMPARE(QString::fromStdString(t->label), QStringLiteral("coretemp Package id 0"));
        QCOMPARE(readHwmonCelsius(t->inputPath).value(), 63.0);
        // k10temp without Tctl/Tdie: its first Tccd.
        QVERIFY(hwmon(r, 4, "k10temp", {{3, 70000, "Tccd1"}, {4, 71000, "Tccd2"}}));
        t = linuxTempSource(r.toStdString());
        QCOMPARE(QString::fromStdString(t->label), QStringLiteral("k10temp Tccd1"));
        QCOMPARE(readHwmonCelsius(t->inputPath).value(), 70.0);
        QVERIFY(hwmon(r, 4, "k10temp", {{1, 72500, "Tctl"}}));
        t = linuxTempSource(r.toStdString());
        QCOMPARE(QString::fromStdString(t->label), QStringLiteral("k10temp Tctl"));
        QCOMPARE(readHwmonCelsius(t->inputPath).value(), 72.5);
        // Zen 1 / Zen+: Tdie (the real value) before Tctl (Tdie + an offset).
        QVERIFY(hwmon(r, 4, "k10temp", {{2, 62500, "Tdie"}}));
        t = linuxTempSource(r.toStdString());
        QCOMPARE(QString::fromStdString(t->label), QStringLiteral("k10temp Tdie"));
        QCOMPARE(readHwmonCelsius(t->inputPath).value(), 62.5);
    }

    void throttleCounters()
    {
        QTemporaryDir d;
        const QString r = d.path();
        // Two CPUs of package 0 (each shows the package's 10), one of
        // package 1 (5), core counts 3 + 4 + 0: 3 + 4 + 10 + 5.
        const auto cpu = [&](int n, const char *core, const char *package, const char *packageId) {
            const QString d = QStringLiteral("devices/system/cpu/cpu%1/").arg(n);
            return put(r, d + QStringLiteral("thermal_throttle/core_throttle_count"), core)
                   && put(r, d + QStringLiteral("thermal_throttle/package_throttle_count"), package)
                   && put(r, d + QStringLiteral("topology/physical_package_id"), packageId);
        };
        QVERIFY(cpu(0, "3\n", "10\n", "0\n"));
        QVERIFY(cpu(1, "4\n", "10\n", "0\n"));
        QVERIFY(cpu(2, "0\n", "5\n", "1\n"));
        QVERIFY(put(r, QStringLiteral("devices/system/cpu/cpufreq/policy0"), ""));  // not a CPU
        QCOMPARE(linuxThrottleCount(r.toStdString()).value(), std::uint64_t(22));
        QTemporaryDir empty;
        QVERIFY(!linuxThrottleCount(empty.path().toStdString()));
    }

    void windowsArithmetic()
    {
        QCOMPARE(pdhClockMHz(120.0, 3000.0), 3600.0);  // boosting 20% above the base frequency
        QCOMPARE(pdhClockMHz(50.0, 2000.0), 1000.0);
        QVERIFY(qAbs(tenthsOfKelvinToCelsius(3031.5) - 30.0) < 1e-9);
    }

    // The sampler reads at its period from the start, in the scope set
    // last, and stops promptly.
    void sampler()
    {
        const auto fake = std::make_shared<FakeSource>();
        Sampler s(fake, 20ms);
        QCOMPARE(s.now(), 0.0);
        s.start();
        std::this_thread::sleep_for(70ms);
        sb::isa::CoreChoice core;
        core.ranked = true;
        core.cpu = 5;
        s.setScope({false, core});
        std::this_thread::sleep_for(70ms);
        const auto stopAt = std::chrono::steady_clock::now();
        s.stop();
        QVERIFY(std::chrono::steady_clock::now() - stopAt < 100ms);
        const std::vector<Sample> v = s.samples();
        QVERIFY2(v.size() >= 4, qPrintable(QString::number(v.size())));
        QCOMPARE(v.front().clockMHz.value(), 1000.0);
        QCOMPARE(v.back().clockMHz.value(), 5.0);
        QVERIFY(v.front().t < 0.050);  // the first sample at once
        for (std::size_t i = 1; i < v.size(); ++i)
            QVERIFY(v[i].t > v[i - 1].t);
        // No more samples after stop().
        std::this_thread::sleep_for(50ms);
        QCOMPARE(s.samples().size(), v.size());
    }

    // AP-5: a sensor read can take hundreds of milliseconds (ACPI, an
    // embedded controller). stopWithin() doesn't wait for it, and the
    // callback is never called after it has returned.
    void slowSensorDoesNotHoldStop()
    {
        struct SlowSource : FakeSource {
            std::optional<double> temperatureC() override
            {
                std::this_thread::sleep_for(400ms);
                return 50.0;
            }
        };
        std::atomic<int> callbacks{0};
        std::atomic<bool> stopped{false};
        std::atomic<int> callbacksAfterStop{0};
        std::chrono::steady_clock::time_point destroyAt;
        {
            Sampler s(std::make_shared<SlowSource>(), 20ms);
            s.setOnSample([&](const Sample &) {
                ++callbacks;
                if (stopped)
                    ++callbacksAfterStop;
            });
            s.start();
            std::this_thread::sleep_for(500ms);  // one reading done, the second under way
            const auto t0 = std::chrono::steady_clock::now();
            QVERIFY(!s.stopWithin(50ms));        // left to end by itself
            stopped = true;
            const auto took = std::chrono::steady_clock::now() - t0;
            QVERIFY2(took < 250ms, qPrintable(QString::number(std::chrono::duration<double, std::milli>(took).count())));
            QVERIFY(callbacks.load() >= 1);
            destroyAt = std::chrono::steady_clock::now();
        }
        // The destructor doesn't wait either: the thread was already left.
        QVERIFY(std::chrono::steady_clock::now() - destroyAt < 50ms);
        Sampler::joinAbandoned();  // the reading ends; the thread exits
        QCOMPARE(callbacksAfterStop.load(), 0);
        // A quick source ends within the wait and is joined.
        Sampler quick(std::make_shared<FakeSource>(), 20ms);
        quick.start();
        std::this_thread::sleep_for(30ms);
        QVERIFY(quick.stopWithin(50ms));
    }

    void sampleStatistics()
    {
        const std::vector<Sample> v = {{0.0, 4000.0, 50.0, {}}, {1.0, 4600.0, 70.0, {}}, {2.0, {}, 90.0, {}},
                                       {3.0, 4400.0, {}, {}}, {4.0, 3000.0, 60.0, {}}};
        QCOMPARE(medianClockMHz(v, 0.5, 3.5).value(), 4500.0);  // 4600 and 4400 (the gap at 2 s is skipped)
        QCOMPARE(medianClockMHz(v, 0.0, 4.0).value(), 4200.0);
        QVERIFY(!medianClockMHz(v, 1.5, 2.5));
        QCOMPARE(peakTemperatureC(v, 0.0, 4.0).value(), 90.0);
        QCOMPARE(peakTemperatureC(v, 3.0, 4.0).value(), 60.0);
        QVERIFY(!peakTemperatureC(v, 2.5, 3.5));
    }

    // §8.2's three rules, within multi-thread windows only.
    void throttleRule()
    {
        // Steady: nothing.
        ThrottleVerdict v = detectThrottle({window("Integer", {100, 99, 100, 98, 99}, {4900.0, 4900.0, 4890.0, 4880.0, 4870.0})},
                                           std::nullopt, std::nullopt);
        QVERIFY(!v.detected);
        QCOMPARE(throttleDetail(v), std::string());
        QCOMPARE(throttleNote(v), std::string());

        // The clock fell 16% in xz: detected, with the design's detail text.
        v = detectThrottle({window("Integer", {100, 100}, {4900.0, 4900.0}),
                            window("xz compression", {50, 48, 47, 46, 46}, {4900.0, 4700.0, 4400.0, 4200.0, 4100.0}, true, 95.4)},
                           std::nullopt, std::nullopt);
        QVERIFY(v.detected);
        QCOMPARE(v.window, std::string("xz compression"));
        QCOMPARE(throttleDetail(v), std::string("xz compression: clock 4.9 → 4.1 GHz (−16%)"));

        // The approved note wording (Dave, 2026-10-16).
        v = detectThrottle({window("xz compression", {50, 49}, {4600.0, 3900.0}, true, 95.0)}, std::nullopt, std::nullopt);
        QCOMPARE(throttleNote(v), std::string("Throttling detected in xz compression: clock speed fell from 4.6 to 3.9 GHz "
                                              "(−15%) at 95 °C. The score is not adjusted."));
        v.temperatureC.reset();
        QCOMPARE(throttleNote(v), std::string("Throttling detected in xz compression: clock speed fell from 4.6 to 3.9 GHz "
                                              "(−15%). The score is not adjusted."));

        // Single-thread windows are never compared (a cool core boosting
        // first is the burst a single-thread mark is meant to show).
        v = detectThrottle({window("single.int", {100, 80}, {5000.0, 3500.0}, false)}, std::nullopt, std::nullopt);
        QVERIFY(!v.detected);

        // At the thresholds exactly: not detected (strictly under).
        v = detectThrottle({window("Sorting", {100, 93}, {4000.0, 3600.0})}, std::nullopt, std::nullopt);
        QVERIFY(!v.detected);

        // Rule 3 without any clock: the rate fell 10%.
        v = detectThrottle({window("Physics", {100, 95, 90}, {std::nullopt, std::nullopt, std::nullopt}, true)},
                           std::nullopt, std::nullopt);
        QVERIFY(v.detected);
        QVERIFY(v.byRate);
        QCOMPARE(throttleDetail(v), std::string("Physics: rate −10% from the first repetition to the last"));
        QCOMPARE(throttleNote(v), std::string("Throttling detected in Physics: the rate fell 10% from the first repetition "
                                              "to the last. The score is not adjusted."));
        // No clock source at all (empty clock lists) works the same.
        v = detectThrottle({window("Physics", {100, 90}, {})}, std::nullopt, std::nullopt);
        QVERIFY(v.detected && v.byRate);

        // A clock drop beats a bigger rate drop elsewhere; between clock
        // drops, the larger one.
        v = detectThrottle({window("Sorting", {100, 70}, {4000.0, 4000.0}), window("SIMD", {100, 95}, {4500.0, 3900.0}),
                            window("Integer", {100, 96}, {4900.0, 4000.0})},
                           std::nullopt, std::nullopt);
        QCOMPARE(v.window, std::string("Integer"));
        QVERIFY(!v.byRate);

        // Rule 1: the Intel counters rose (and nothing else).
        v = detectThrottle({}, 10, 22);
        QVERIFY(v.detected && v.counters);
        QCOMPARE(throttleDetail(v), std::string("Intel throttle counters rose by 12"));
        // Both: the window's detail, then the counters.
        v = detectThrottle({window("Physics", {100, 95, 90}, {})}, 10, 22);
        QCOMPARE(throttleDetail(v), std::string("Physics: rate \u221210% from the first repetition to the last; Intel "
                                                "throttle counters rose by 12"));
        v = detectThrottle({}, 10, 10);
        QVERIFY(!v.detected);
        v = detectThrottle({}, std::nullopt, 22);
        QVERIFY(!v.detected);
    }

    // This machine: the readings don't fail (values depend on the machine).
    void live()
    {
        const std::unique_ptr<Source> s = makeSystemSource();
        const auto clock = s->clockMHz({});
        const auto temp = s->temperatureC();
        qInfo("clock %s MHz, temperature %s (%s), performance limit %s",
              clock ? qPrintable(QString::number(*clock, 'f', 0)) : "unknown",
              temp ? qPrintable(QString::number(*temp, 'f', 1)) : "unknown", s->temperatureLabel().c_str(),
              s->performanceLimitPercent() ? qPrintable(QString::number(*s->performanceLimitPercent())) : "unknown");
        if (clock)
            QVERIFY(*clock > 0 && *clock < 20000);
        if (temp)
            QVERIFY(*temp > -50 && *temp < 150);
    }

private:
#if defined(Q_OS_LINUX)
    QTemporaryDir m_trees;
    std::string m_knowhere;
#endif
};

QTEST_GUILESS_MAIN(TestCpuTelemetry)
#include "tst_cpu_telemetry.moc"

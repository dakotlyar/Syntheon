// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/cpu_test.h"

#include <QJsonArray>

namespace sb::tests {

namespace {

StubSpec makeSpec()
{
    StubSpec s;
    TestInfo &i = s.info;
    i.id = QStringLiteral("cpu");
    i.name = QStringLiteral("CPU");
    i.summary = QStringLiteral("Integer, FP, SIMD, compression, encryption, sorting, physics");
    i.colorToken = QStringLiteral("t-cpu");
    i.subTests = {{QStringLiteral("int_fp"), QStringLiteral("Integer + FP + primes"), true, true},
                  {QStringLiteral("simd"), QStringLiteral("SIMD"), true, true},
                  {QStringLiteral("compress"), QStringLiteral("Compression"), true, true},
                  {QStringLiteral("crypto"), QStringLiteral("Encryption"), true, true},
                  {QStringLiteral("sort"), QStringLiteral("Sorting"), true, true},
                  {QStringLiteral("physics"), QStringLiteral("Physics"), true, true}};
    s.estimate = std::chrono::seconds(24);
    s.targetScore = 1284;

    const auto metric = [&](const char *key, const char *sub, const char *label, const char *unit, double base,
                            double share, bool headline = false) {
        s.metrics.append({QString::fromLatin1(key), QString::fromLatin1(sub), QString::fromLatin1(label),
                          QString::fromLatin1(unit), base, Direction::HigherIsBetter, headline, share, {}});
    };
    metric("int_fp.int", "int_fp", "Integer math", "MOps/s", 152400, 0.15, true);
    metric("int_fp.fp", "int_fp", "Floating point", "MFLOPS", 98700, 0.15);
    metric("int_fp.prime", "int_fp", "Prime numbers", "MOps/s", 412, 0.05);
    metric("simd.ops", "simd", "SIMD", "MOps/s", 61200, 0.15);
    metric("compress.rate", "compress", "Compression", "MB/s", 1184, 0.12);
    metric("crypto.rate", "crypto", "Encryption (AES)", "MB/s", 41900, 0.12);
    metric("sort.rate", "sort", "Sorting", "MOps/s", 96.4, 0.10);
    metric("physics.fps", "physics", "Physics", "fps", 1740, 0.10);
    s.metrics.append({QStringLiteral("single.mark"), {}, QStringLiteral("Single-thread mark"), QStringLiteral("points"), 3980,
                      Direction::HigherIsBetter, true, 0.06,
                      [](const OptionValues &o) { return o.value(QStringLiteral("single_thread"), true).toBool(); }});

    s.options = [](const ProbeContext &ctx, const Availability &) {
        OptionSchema o;
        OptionDef single;
        single.key = QStringLiteral("single_thread");
        single.label = QStringLiteral("Single-thread mark");
        single.kind = OptionKind::Bool;
        single.defaultValue = true;
        single.help = QStringLiteral("Also measure one core on its own.");
        o.options.append(single);
        OptionDef threads;
        threads.key = QStringLiteral("threads");
        threads.label = QStringLiteral("Threads");
        threads.kind = OptionKind::Info;
        threads.infoValue = QStringLiteral("All %1 for every sub-test")
                                .arg(int(snapshotNumber(ctx.system(), "cpu", "threads", 16)));
        threads.infoTone = Tone::Neutral;
        o.options.append(threads);
        return o;
    };

    s.extra = [](TestResult &r, const OptionValues &, std::mt19937 &, const SystemSnapshot &sys) {
        const QJsonArray features = sys.data.value(QStringLiteral("cpu")).toObject().value(QStringLiteral("features")).toArray();
        const auto has = [&](const char *name) {
            for (const QJsonValue &f : features) {
                if (f.toString() == QLatin1StringView(name))
                    return true;
            }
            return false;
        };
        const QString path = has("avx512f") ? QStringLiteral("AVX-512")
                             : has("avx2")  ? QStringLiteral("AVX2")
                             : has("avx")   ? QStringLiteral("AVX")
                             : has("neon")  ? QStringLiteral("NEON")
                                            : QStringLiteral("Baseline");
        r.metrics.append(textMetric(QStringLiteral("simd.path"), QStringLiteral("SIMD path"), path, false));
        r.metrics.append(numberMetric(QStringLiteral("threads"), QStringLiteral("Threads used"),
                                      snapshotNumber(sys, "cpu", "threads", 16), QString()));
    };

    s.detail = [](const TestResult &r) {
        DetailLayout d;
        d.subtitle = scoreSubtitle(r, {metricText(r, QStringLiteral("simd.path")) + QStringLiteral(" path"),
                                       metricText(r, QStringLiteral("threads")) + QStringLiteral(" threads")});
        for (const char *k : {"single.mark", "int_fp.int"})
            if (r.metric(QString::fromLatin1(k)))
                d.headlineMetrics.append(QString::fromLatin1(k));
        KpisBlock kpis;
        if (r.metric(QStringLiteral("single.mark")))
            kpis.items.append({QStringLiteral("Single-thread mark"), DetailCell::metric(QStringLiteral("single.mark")), {}});
        kpis.items.append({QStringLiteral("Threads used"), DetailCell::metric(QStringLiteral("threads")), {}});
        kpis.items.append({QStringLiteral("SIMD path"), DetailCell::metric(QStringLiteral("simd.path")), {}});
        d.blocks.append({{}, kpis});
        TableBlock table;
        table.columns = {QStringLiteral("Sub-test (all threads)"), QStringLiteral("Result")};
        for (const Metric &m : r.metrics) {
            if (!m.subTestId.isEmpty())
                table.rows.append({DetailCell::literal(m.label), DetailCell::metric(m.key)});
        }
        d.blocks.append({{}, table});
        return d;
    };
    return s;
}

} // namespace

CpuTest::CpuTest() : StubTest(makeSpec()) {}

} // namespace sb::tests

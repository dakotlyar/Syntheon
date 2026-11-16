// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU Detail page (Phase 4c design §12, §15 tst_npu_detail): from a
// hand-built result — the subtitle names the device + engine, the KPI cards the
// score + three headline throughputs, the Sub-tests table three data rows + a
// bold total with sub-scores, the per-workload table the model name + precision,
// and a NotAvailable result keeps the NaStateBlock (N9).
#include "tests/npu/npu_test.h"

#include "core/detail.h"

#include <QTest>

using namespace sb;
using namespace sb::tests;

namespace {

TestResult mockupResult()
{
    TestResult r;
    r.testId = QStringLiteral("npu");
    r.status = Status::Ok;
    r.score = 1000;
    const auto num = [&](const char *key, const char *sub, double v, const char *unit) {
        r.metrics.append({QLatin1StringView(key), QLatin1StringView(sub), QString(), v, QLatin1StringView(unit),
                          Direction::Neutral, false});
    };
    const auto txt = [&](const char *key, const QString &v) {
        r.metrics.append({QLatin1StringView(key), {}, QString(), v, {}, Direction::Neutral, false});
    };
    txt("device.vendor", QStringLiteral("Intel"));
    txt("device.name", QStringLiteral("Intel AI Boost"));
    txt("device.driver", QStringLiteral("intel_vpu"));
    txt("device.runtime", QStringLiteral("2025.1.0"));
    txt("engine.name", QStringLiteral("openvino-npu"));
    txt("model.classify", QStringLiteral("resnet50-int8 · INT8"));
    txt("model.detect", QStringLiteral("yolov8n-int8 · INT8"));
    txt("model.llm", QStringLiteral("llm-1b-int4 · INT4"));
    txt("models.pack_version", QStringLiteral("1"));
    txt("power.source", QStringLiteral("AC"));
    num("classify.imgs", "classify", 1450.0, "img/s");
    num("classify.img0", "classify", 1430.0, "img/s");
    num("classify.img1", "classify", 1450.0, "img/s");
    num("classify.img2", "classify", 1470.0, "img/s");
    num("detect.imgs", "detect", 900.0, "img/s");
    num("llm.tps", "llm", 55.0, "tok/s");
    num("llm.ttft", "llm", 75.0, "ms");
    num("stability.spread", "", 2.1, "%");
    r.subResults = {{QStringLiteral("classify"), Status::Ok, {}, 1000.0},
                    {QStringLiteral("detect"), Status::Ok, {}, 1000.0},
                    {QStringLiteral("llm"), Status::Ok, {}, 1000.0}};
    return r;
}

const TableBlock *tableTitled(const DetailLayout &d, const QString &title)
{
    for (const DetailBlock &b : d.blocks)
        if (b.title == title)
            return std::get_if<TableBlock>(&b.content);
    return nullptr;
}

QStringList cells(const TableBlock &t, int row)
{
    QStringList out;
    for (const DetailCell &c : t.rows.value(row))
        out << c.text;
    return out;
}

} // namespace

class TestNpuDetail : public QObject
{
    Q_OBJECT

private slots:
    void completed()
    {
        NpuTest t;
        const DetailLayout d = t.detail(mockupResult());
        // Subtitle: the score plus the device + engine + precisions (§12).
        QVERIFY2(d.subtitle.contains(QLatin1StringView("Intel AI Boost")), qPrintable(d.subtitle));
        QVERIFY(d.subtitle.contains(QLatin1StringView("openvino-npu")));
        QVERIFY(d.subtitle.contains(QLatin1StringView("INT8/INT4")));

        // The headline metrics are the three throughputs.
        QCOMPARE(d.headlineMetrics, (QStringList{QStringLiteral("classify.imgs"), QStringLiteral("detect.imgs"),
                                                 QStringLiteral("llm.tps")}));

        // KPI cards: the score + the three headline numbers (§12).
        const auto &kpis = std::get<KpisBlock>(d.blocks.first().content);
        QCOMPARE(kpis.items.size(), 4);
        QCOMPARE(kpis.items.first().label, QStringLiteral("NPU score"));
        QCOMPARE(kpis.items.at(1).label, QStringLiteral("Classification"));
        QCOMPARE(kpis.items.at(2).label, QStringLiteral("Detection"));
        QCOMPARE(kpis.items.at(3).label, QStringLiteral("LLM generation"));

        // Sub-tests: three data rows + a bold total, each with a sub-score (§12).
        const TableBlock *groups = tableTitled(d, QStringLiteral("Sub-tests"));
        QVERIFY(groups);
        QCOMPARE(groups->rows.size(), 4);
        QCOMPARE(groups->boldRows, QVector<int>{3});
        QCOMPARE(cells(*groups, 0).first(), QStringLiteral("Classify"));
        QCOMPARE(cells(*groups, 1).first(), QStringLiteral("Detect"));
        QCOMPARE(cells(*groups, 2).first(), QStringLiteral("LLM"));
        QCOMPARE(cells(*groups, 3), (QStringList{QStringLiteral("NPU score"), QStringLiteral("100%"),
                                                QStringLiteral("weighted geometric mean"), QStringLiteral("1,000")}));

        // Per-workload table: Classify/Detect/LLM with model + precision (§12).
        const TableBlock *per = tableTitled(d, QStringLiteral("Workloads"));
        QVERIFY(per);
        QCOMPARE(per->columns, (QStringList{QStringLiteral("Workload"), QStringLiteral("Model"),
                                            QStringLiteral("Precision"), QStringLiteral("Result")}));
        QCOMPARE(cells(*per, 0), (QStringList{QStringLiteral("Classify"), QStringLiteral("resnet50-int8"),
                                              QStringLiteral("INT8"), QStringLiteral("1,450 img/s")}));
        QCOMPARE(cells(*per, 1).at(0), QStringLiteral("Detect"));
        QCOMPARE(cells(*per, 1).at(1), QStringLiteral("yolov8n-int8"));
        QCOMPARE(cells(*per, 2).at(0), QStringLiteral("LLM"));
        QCOMPARE(cells(*per, 2).at(1), QStringLiteral("llm-1b-int4"));
        QCOMPARE(cells(*per, 2).at(2), QStringLiteral("INT4"));

        // Device table: vendor → NPU → driver → runtime + engine + model pack.
        const TableBlock *dev = tableTitled(d, QStringLiteral("Device"));
        QVERIFY(dev);
        QCOMPARE(cells(*dev, 0), (QStringList{QStringLiteral("Vendor"), QStringLiteral("Intel")}));
        QCOMPARE(cells(*dev, 1), (QStringList{QStringLiteral("NPU"), QStringLiteral("Intel AI Boost")}));
        QCOMPARE(cells(*dev, 4).at(0), QStringLiteral("Engine"));
        QCOMPARE(cells(*dev, 4).at(1), QStringLiteral("openvino-npu"));
    }

    void notAvailableKeepsNaStateBlock()
    {
        NpuTest t;
        const TestResult na = TestResult::notAvailable(QStringLiteral("No supported NPU found on this machine"));
        const DetailLayout d = t.detail(na);
        QVERIFY(std::holds_alternative<NaStateBlock>(d.blocks.first().content));
    }
};

QTEST_GUILESS_MAIN(TestNpuDetail)
#include "tst_npu_detail.moc"

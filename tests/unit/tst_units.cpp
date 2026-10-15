#include "core/units.h"

#include <QTest>

using namespace sb;

class TestUnits : public QObject
{
    Q_OBJECT

private slots:
    void format_data()
    {
        QTest::addColumn<double>("value");
        QTest::addColumn<QString>("unit");
        QTest::addColumn<QString>("expected");

        // Values from the mockup.
        QTest::newRow("grouped") << 7012.0 << "MB/s" << "7,012 MB/s";
        QTest::newRow("large") << 152400.0 << "MOps/s" << "152,400 MOps/s";
        QTest::newRow("hundreds") << 398.0 << "fps" << "398 fps";
        QTest::newRow("tens") << 41.9 << "GB/s" << "41.9 GB/s";
        QTest::newRow("tens-whole") << 90.0 << "Wh" << "90.0 Wh";
        QTest::newRow("ones") << 3.14 << "it/s" << "3.1 it/s";
        QTest::newRow("sub-one") << 0.18 << "s" << "0.18 s";
        QTest::newRow("tiny") << 0.001234 << "s" << "0.0012 s";
        QTest::newRow("mt/s ungrouped") << 5600.0 << "MT/s" << "5600 MT/s";
        QTest::newRow("percent") << 98.0 << "%" << "98.0%";
        QTest::newRow("points no unit") << 1284.0 << "points" << "1,284";
        QTest::newRow("unitless") << 12.5 << "" << "12.5";
        QTest::newRow("micro") << 1210.0 << "µs" << "1,210 µs";
        QTest::newRow("zero") << 0.0 << "ns" << "0 ns";
        QTest::newRow("negative") << -1234.5 << "W" << "-1,235 W";
        QTest::newRow("carry 99.96") << 99.96 << "ns" << "100 ns";
        QTest::newRow("carry 0.996") << 0.996 << "s" << "1.0 s";
        QTest::newRow("nan") << qQNaN() << "s" << "— s";
    }

    void format()
    {
        QFETCH(double, value);
        QFETCH(QString, unit);
        QFETCH(QString, expected);
        QCOMPARE(Units::format(value, unit), expected);
    }

    void formatWithoutUnit()
    {
        QCOMPARE(Units::format(7012.0, QStringLiteral("MB/s"), false), QStringLiteral("7,012"));
    }

    void canonicalUnits()
    {
        QVERIFY(Units::isCanonical(QStringLiteral("MB/s")));
        QVERIFY(Units::isCanonical(QStringLiteral("µs")));
        QVERIFY(Units::isCanonical(QString()));
        QVERIFY(!Units::isCanonical(QStringLiteral("MiB/s")));
        QCOMPARE(Units::canonical().size(), 22);
    }
};

QTEST_GUILESS_MAIN(TestUnits)
#include "tst_units.moc"

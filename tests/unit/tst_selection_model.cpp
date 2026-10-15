#include "core/registry.h"
#include "core/selection_model.h"
#include "tests/builtin_tests.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTest>

using namespace sb;

class TestSelectionModel : public QObject
{
    Q_OBJECT

private:
    Registry m_registry;
    const QString cpu = QStringLiteral("cpu");
    const QString ram = QStringLiteral("ram");
    const QString gpu = QStringLiteral("gpu");
    const QString npu = QStringLiteral("npu");
    const QString battery = QStringLiteral("battery");

private slots:
    void initTestCase() { registerBuiltinTests(m_registry); }

    void modelBasics()
    {
        SelectionModel m(m_registry);
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(m.rowCount(), 7);
        QCOMPARE(m.data(m.index(0), SelectionModel::IdRole).toString(), cpu);
        QCOMPARE(m.data(m.index(6), SelectionModel::RunsAloneRole).toBool(), true);
        QCOMPARE(m.data(m.index(0), SelectionModel::ColorTokenRole).toString(), QStringLiteral("t-cpu"));
        QVERIFY(m.roleNames().values().contains("selected"));
        QCOMPARE(m.selectedCount(), 0);
    }

    void tickingBatteryClearsOthers()
    {
        SelectionModel m(m_registry);
        m.setSelected(cpu, true);
        m.setSelected(ram, true);
        QVERIFY(!m.runsAloneSelected());
        QVERIFY(m.setSelected(battery, true));
        QCOMPARE(m.selectedIds(), QStringList{battery});
        QVERIFY(m.runsAloneSelected());
    }

    void tickingOtherClearsBattery()
    {
        SelectionModel m(m_registry);
        m.setSelected(battery, true);
        QVERIFY(m.setSelected(gpu, true));
        QCOMPARE(m.selectedIds(), QStringList{gpu});
        QVERIFY(!m.runsAloneSelected());
    }

    void toggle()
    {
        SelectionModel m(m_registry);
        QVERIFY(m.toggle(cpu));
        QVERIFY(m.isSelected(cpu));
        QVERIFY(m.toggle(cpu));
        QVERIFY(!m.isSelected(cpu));
        m.toggle(battery);
        m.toggle(ram);
        QCOMPARE(m.selectedIds(), QStringList{ram});
    }

    void selectAllSkipsRunsAloneAndUnavailable()
    {
        SelectionModel m(m_registry);
        m.setAvailability(npu, Availability::unavailable(QStringLiteral("No supported NPU found")));
        m.setSelected(battery, true);
        m.selectAll();
        QCOMPARE(m.selectedIds(), (QStringList{cpu, ram, QStringLiteral("storage"), gpu, QStringLiteral("ai")}));
        QVERIFY(!m.runsAloneSelected());
    }

    void unavailableCantBeTicked()
    {
        SelectionModel m(m_registry);
        m.setAvailability(npu, Availability::unavailable(QStringLiteral("No supported NPU found")));
        QVERIFY(!m.setSelected(npu, true));
        QVERIFY(!m.isSelected(npu));
        const QModelIndex row = m.index(m_registry.indexOf(npu));
        QCOMPARE(m.data(row, SelectionModel::AvailableRole).toBool(), false);
        QCOMPARE(m.data(row, SelectionModel::UnavailableReasonRole).toString(), QStringLiteral("No supported NPU found"));
    }

    void becomingUnavailableUnticks()
    {
        SelectionModel m(m_registry);
        m.setSelected(gpu, true);
        QSignalSpy spy(&m, &SelectionModel::selectionChanged);
        m.setAvailability(gpu, Availability::unavailable(QStringLiteral("No GPU")));
        QVERIFY(!m.isSelected(gpu));
        QCOMPARE(spy.count(), 1);
        // Partial still counts as selectable.
        Availability partial;
        partial.state = Availability::State::Partial;
        m.setAvailability(gpu, partial);
        QVERIFY(m.setSelected(gpu, true));
    }

    void canonicalOrderNotTickOrder()
    {
        SelectionModel m(m_registry);
        m.setSelected(gpu, true);
        m.setSelected(cpu, true);
        m.setSelected(ram, true);
        QCOMPARE(m.selectedIds(), (QStringList{cpu, ram, gpu}));
    }

    void clearAndSignals()
    {
        SelectionModel m(m_registry);
        QSignalSpy changed(&m, &SelectionModel::selectionChanged);
        QSignalSpy data(&m, &QAbstractItemModel::dataChanged);
        m.selectAll();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(data.count(), 6);
        m.selectAll();  // no change, no signal
        QCOMPARE(changed.count(), 1);
        m.clear();
        QCOMPARE(m.selectedCount(), 0);
        QCOMPARE(changed.count(), 2);
    }

    void unknownIdRefused()
    {
        SelectionModel m(m_registry);
        QVERIFY(!m.setSelected(QStringLiteral("nope"), true));
        QVERIFY(!m.isSelected(QStringLiteral("nope")));
    }
};

QTEST_GUILESS_MAIN(TestSelectionModel)
#include "tst_selection_model.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/registry.h"
#include "fake_test.h"
#include "tests/builtin_tests.h"

#include <QSet>
#include <QTest>

using namespace sb;
using namespace sb::testing;

class TestRegistry : public QObject
{
    Q_OBJECT

private slots:
    void builtinListInCanonicalOrder()
    {
        Registry r;
        registerBuiltinTests(r);
        QCOMPARE(r.ids(), (QStringList{QStringLiteral("cpu"), QStringLiteral("ram"), QStringLiteral("storage"),
                                       QStringLiteral("gpu"), QStringLiteral("ai"), QStringLiteral("npu"),
                                       QStringLiteral("battery")}));
    }

    void builtinInfo()
    {
        Registry r;
        registerBuiltinTests(r);
        QSet<QString> tokens;
        for (const auto &t : r.tests()) {
            const TestInfo &i = t->info();
            QVERIFY2(!i.name.isEmpty(), qPrintable(i.id));
            QVERIFY2(!i.summary.isEmpty(), qPrintable(i.id));
            QVERIFY2(i.colorToken.startsWith(QLatin1StringView("t-")), qPrintable(i.id));
            tokens.insert(i.colorToken);
            // Phase 1 stubs are version 0 (§2.9); the real CPU and RAM tests
            // are 1 since their calibrations (Phase 2 task 18, Phase 3 task 14).
            const int version = i.id == QLatin1StringView("cpu") || i.id == QLatin1StringView("ram") ? 1 : 0;
            QCOMPARE(i.testVersion, version);
            QCOMPARE(i.formulaVersion, version);
            const bool battery = i.id == QLatin1StringView("battery");
            QCOMPARE(i.exclusivity == Exclusivity::RunsAlone, battery);
            QCOMPARE(i.recovery == RecoveryMode::FromCheckpoints, battery);
        }
        QCOMPARE(tokens.size(), 7);
    }

    void findAndIndex()
    {
        Registry r;
        registerBuiltinTests(r);
        QVERIFY(r.find(QStringLiteral("gpu")));
        QCOMPARE(r.find(QStringLiteral("gpu"))->info().name, QStringLiteral("GPU"));
        QCOMPARE(r.indexOf(QStringLiteral("cpu")), 0);
        QCOMPARE(r.indexOf(QStringLiteral("battery")), 6);
        QCOMPARE(r.indexOf(QStringLiteral("nope")), -1);
        QVERIFY(!r.find(QStringLiteral("nope")));
    }

    void refusesDuplicatesAndEmpty()
    {
        Registry r;
        QVERIFY(r.add(std::make_unique<FakeTest>(FakeTest::makeInfo(QStringLiteral("a")))));
        QVERIFY(!r.add(std::make_unique<FakeTest>(FakeTest::makeInfo(QStringLiteral("a")))));
        QVERIFY(!r.add(std::make_unique<FakeTest>(FakeTest::makeInfo(QString()))));
        QVERIFY(!r.add(nullptr));
        QCOMPARE(r.size(), 1);
    }

    void skeletonDefaults()
    {
        Registry r;
        registerBuiltinTests(r);
        // Battery has no time limit (it runs until shutdown); the others do.
        QCOMPARE(r.find(QStringLiteral("battery"))->timeLimit({}), std::chrono::seconds(0));
        QVERIFY(r.find(QStringLiteral("cpu"))->timeLimit({}) > std::chrono::seconds(0));
    }

    void autoSubTestsOption()
    {
        TestInfo info = FakeTest::makeInfo(QStringLiteral("cpu"));
        info.subTests = {{QStringLiteral("int_fp"), QStringLiteral("Integer + FP"), true, true},
                         {QStringLiteral("simd"), QStringLiteral("SIMD"), true, false},
                         {QStringLiteral("physics"), QStringLiteral("Physics"), true, true},
                         {QStringLiteral("hidden"), QStringLiteral("Hidden"), false, true}};
        FakeTest t(info);
        OptionDef own;
        own.key = QStringLiteral("threads");
        own.label = QStringLiteral("Threads");
        own.kind = OptionKind::Bool;
        own.defaultValue = true;
        t.schema.options = {own};

        Availability a;
        a.subTestUnavailable.insert(QStringLiteral("physics"), QStringLiteral("no FPU"));

        FakeProbeContext ctx;
        const OptionSchema s = effectiveOptionSchema(t, ctx, a);
        QCOMPARE(s.options.size(), 2);
        const OptionDef &sub = s.options.first();
        QCOMPARE(sub.key, QStringLiteral("subtests"));
        QCOMPARE(sub.kind, OptionKind::MultiSelect);
        QCOMPARE(sub.items.size(), 3);  // "hidden" isn't user-selectable
        QVERIFY(!sub.item(QStringLiteral("physics"))->enabled);
        QCOMPARE(sub.item(QStringLiteral("physics"))->disabledReason, QStringLiteral("no FPU"));
        // Defaults: defaultOn and available only.
        QCOMPARE(sub.defaultValue.toStringList(), QStringList{QStringLiteral("int_fp")});
        QVERIFY(validateOptions(s, s.defaults()).ok());
        QVERIFY(!validateOptions(s, {{QStringLiteral("subtests"), QStringList{QStringLiteral("physics")}}}).ok());
    }

    void noSubTestsNoAutoOption()
    {
        FakeTest t(FakeTest::makeInfo(QStringLiteral("ram")));
        FakeProbeContext ctx;
        QVERIFY(effectiveOptionSchema(t, ctx, {}).options.isEmpty());
    }

    void defaultDetailIsGeneric()
    {
        FakeTest t(FakeTest::makeInfo(QStringLiteral("ram")));
        TestResult r;
        r.metrics = {{QStringLiteral("x"), {}, QStringLiteral("X"), 1.0, QString(), Direction::Neutral, true}};
        QCOMPARE(t.detail(r), genericDetailLayout(r));
    }
};

QTEST_GUILESS_MAIN(TestRegistry)
#include "tst_registry.moc"

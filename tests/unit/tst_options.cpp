#include "core/options.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace sb;

namespace {

OptionSchema sampleSchema()
{
    OptionSchema s;

    OptionDef b;
    b.key = QStringLiteral("single_thread");
    b.label = QStringLiteral("Single-thread mark");
    b.kind = OptionKind::Bool;
    b.defaultValue = true;
    s.options.append(b);

    OptionDef m;
    m.key = QStringLiteral("runtimes");
    m.label = QStringLiteral("Runtimes");
    m.kind = OptionKind::MultiSelect;
    m.items = {{QStringLiteral("llama"), QStringLiteral("llama.cpp")},
               {QStringLiteral("ort"), QStringLiteral("ONNX Runtime")},
               {QStringLiteral("openvino"), QStringLiteral("OpenVINO"), false, QStringLiteral("no Intel device")}};
    m.minSelected = 1;
    m.defaultValue = QStringList{QStringLiteral("llama"), QStringLiteral("ort")};
    s.options.append(m);

    OptionDef p;
    p.key = QStringLiteral("profile");
    p.label = QStringLiteral("Profile");
    p.kind = OptionKind::SingleSelect;
    p.presentation = SelectPresentation::Segmented;
    p.items = {{QStringLiteral("light"), QStringLiteral("Light")},
               {QStringLiteral("mixed"), QStringLiteral("Mixed")},
               {QStringLiteral("heavy"), QStringLiteral("Heavy")}};
    p.defaultValue = QStringLiteral("mixed");
    s.options.append(p);

    OptionDef d;
    d.key = QStringLiteral("drive");
    d.label = QStringLiteral("Drive");
    d.kind = OptionKind::DynamicSelect;
    d.items = {{QStringLiteral("/"), QStringLiteral("/"), true, {}, QStringLiteral("NVMe · 812 GB free · btrfs")}};
    s.options.append(d);

    OptionDef n;
    n.key = QStringLiteral("duration");
    n.label = QStringLiteral("Duration");
    n.kind = OptionKind::Number;
    n.minimum = 5;
    n.maximum = 600;
    n.unit = QStringLiteral("s");
    n.defaultValue = 30.0;
    s.options.append(n);

    OptionDef path;
    path.key = QStringLiteral("target");
    path.label = QStringLiteral("Target folder");
    path.kind = OptionKind::Path;
    s.options.append(path);

    OptionDef info;
    info.key = QStringLiteral("window");
    info.label = QStringLiteral("Window");
    info.kind = OptionKind::Info;
    info.infoValue = QStringLiteral("1280×720 · vsync off");
    info.infoTone = Tone::Good;
    s.options.append(info);

    return s;
}

QStringList errorKeys(const Validation &v)
{
    QStringList keys;
    for (const ValidationError &e : v.errors)
        keys.append(e.key);
    return keys;
}

} // namespace

class TestOptions : public QObject
{
    Q_OBJECT

private slots:
    void defaultsSkipInfoAndUnset()
    {
        const OptionValues d = sampleSchema().defaults();
        QCOMPARE(d.size(), 4);
        QCOMPARE(d.value(QStringLiteral("profile")).toString(), QStringLiteral("mixed"));
        QVERIFY(!d.contains(QStringLiteral("window")));
        QVERIFY(!d.contains(QStringLiteral("drive")));
    }

    void defaultsAreValid()
    {
        const OptionSchema s = sampleSchema();
        QVERIFY(validateOptions(s, s.defaults()).ok());
    }

    void withDefaultsOverrides()
    {
        const OptionSchema s = sampleSchema();
        const OptionValues v = s.withDefaults({{QStringLiteral("profile"), QStringLiteral("heavy")}});
        QCOMPARE(v.value(QStringLiteral("profile")).toString(), QStringLiteral("heavy"));
        QCOMPARE(v.value(QStringLiteral("duration")).toDouble(), 30.0);
    }

    void missingKeysAreFine()
    {
        QVERIFY(validateOptions(sampleSchema(), {}).ok());
    }

    void unknownKeyRejected()
    {
        const Validation v = validateOptions(sampleSchema(), {{QStringLiteral("bogus"), 1}});
        QCOMPARE(errorKeys(v), QStringList{QStringLiteral("bogus")});
    }

    void typeChecks_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");
        QTest::addColumn<bool>("ok");

        QTest::newRow("bool ok") << "single_thread" << QVariant(false) << true;
        QTest::newRow("bool wrong type") << "single_thread" << QVariant(QStringLiteral("yes")) << false;
        QTest::newRow("multi ok") << "runtimes" << QVariant(QStringList{QStringLiteral("ort")}) << true;
        QTest::newRow("multi as variant list") << "runtimes" << QVariant(QVariantList{QStringLiteral("ort")}) << true;
        QTest::newRow("multi unknown item") << "runtimes" << QVariant(QStringList{QStringLiteral("tensorrt")}) << false;
        QTest::newRow("multi disabled item") << "runtimes" << QVariant(QStringList{QStringLiteral("openvino")}) << false;
        QTest::newRow("multi below min") << "runtimes" << QVariant(QStringList{}) << false;
        QTest::newRow("multi wrong type") << "runtimes" << QVariant(3) << false;
        QTest::newRow("single ok") << "profile" << QVariant(QStringLiteral("light")) << true;
        QTest::newRow("single unknown") << "profile" << QVariant(QStringLiteral("extreme")) << false;
        QTest::newRow("single wrong type") << "profile" << QVariant(2) << false;
        QTest::newRow("dynamic ok") << "drive" << QVariant(QStringLiteral("/")) << true;
        QTest::newRow("dynamic stale") << "drive" << QVariant(QStringLiteral("/mnt/usb")) << false;
        QTest::newRow("number ok") << "duration" << QVariant(60) << true;
        QTest::newRow("number edge") << "duration" << QVariant(600.0) << true;
        QTest::newRow("number low") << "duration" << QVariant(1.0) << false;
        QTest::newRow("number high") << "duration" << QVariant(601) << false;
        QTest::newRow("number wrong type") << "duration" << QVariant(QStringLiteral("60")) << false;
        QTest::newRow("path ok") << "target" << QVariant(QStringLiteral("/tmp")) << true;
        QTest::newRow("path wrong type") << "target" << QVariant(5) << false;
        QTest::newRow("info read-only") << "window" << QVariant(QStringLiteral("x")) << false;
    }

    void typeChecks()
    {
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        QFETCH(bool, ok);
        const Validation v = validateOptions(sampleSchema(), {{key, value}});
        QCOMPARE(v.ok(), ok);
        if (!ok) {
            QCOMPARE(errorKeys(v), QStringList{key});
            QVERIFY(!v.errors.first().message.isEmpty());
        }
    }

    void disabledReasonInMessage()
    {
        const Validation v = validateOptions(sampleSchema(),
                                             {{QStringLiteral("runtimes"), QStringList{QStringLiteral("openvino")}}});
        QVERIFY(v.errors.first().message.contains(QStringLiteral("no Intel device")));
    }

    void jsonRoundTripStillValid()
    {
        // Options are stored as JSON per result; values read back must validate.
        const OptionSchema s = sampleSchema();
        const QJsonObject json = QJsonObject::fromVariantMap(s.defaults());
        const OptionValues back = QJsonDocument::fromJson(QJsonDocument(json).toJson()).object().toVariantMap();
        QVERIFY(validateOptions(s, back).ok());
    }
};

QTEST_GUILESS_MAIN(TestOptions)
#include "tst_options.moc"

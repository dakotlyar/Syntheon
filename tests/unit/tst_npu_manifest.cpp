// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU model-pack manifest schema (Phase 4c design §6.1, §14.1, §15):
// parse the §6.1 verbatim shape, fail closed on structural/schema problems
// (path traversal, non-positive/non-integer size, non-hex sha256, missing or
// unknown vendor/workload/precision), and filter a vendor's models.
#include "tests/npu/models/manifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QCryptographicHash>

#include <functional>

using namespace sb::npu;

namespace {

const QString kSha64 = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

QJsonObject goodFile()
{
    return QJsonObject{{QStringLiteral("path"), QStringLiteral("intel/classify/resnet50-int8/1/resnet50.bin")},
                       {QStringLiteral("size"), 98000000},
                       {QStringLiteral("sha256"), kSha64}};
}

QJsonObject goodModel()
{
    return QJsonObject{{QStringLiteral("id"), QStringLiteral("resnet50-int8")},
                       {QStringLiteral("vendor"), QStringLiteral("intel")},
                       {QStringLiteral("workload"), QStringLiteral("classify")},
                       {QStringLiteral("precision"), QStringLiteral("INT8")},
                       {QStringLiteral("files"), QJsonArray{goodFile()}}};
}

QJsonObject goodRoot()
{
    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("pack_version"), QStringLiteral("1")},
                       {QStringLiteral("models"), QJsonArray{goodModel()}}};
}

QByteArray json(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool isBad(const QJsonObject &o, QString *error)
{
    return !parse(json(o), error).has_value();
}

// Replace the single model with a copy of `goodModel()` mutated by `edit`.
QJsonObject rootWithModel(std::function<void(QJsonObject &)> edit)
{
    QJsonObject model = goodModel();
    edit(model);
    QJsonObject root = goodRoot();
    root.insert(QStringLiteral("models"), QJsonArray{model});
    return root;
}

// Replace the single file with a copy of `goodFile()` mutated by `edit`.
QJsonObject rootWithFile(std::function<void(QJsonObject &)> edit)
{
    QJsonObject file = goodFile();
    edit(file);
    QJsonObject model = goodModel();
    model.insert(QStringLiteral("files"), QJsonArray{file});
    QJsonObject root = goodRoot();
    root.insert(QStringLiteral("models"), QJsonArray{model});
    return root;
}

} // namespace

class TestNpuManifest : public QObject
{
    Q_OBJECT

private slots:
    void wellFormedParses()
    {
        QString error;
        const std::optional<Manifest> m = parse(json(goodRoot()), &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        QCOMPARE(m->schema, 1);
        QCOMPARE(m->pack_version, QStringLiteral("1"));
        QCOMPARE(m->models.size(), 1);
        QCOMPARE(m->models[0].id, QStringLiteral("resnet50-int8"));
        QCOMPARE(m->models[0].precision, QStringLiteral("INT8"));
        QCOMPARE(m->models[0].files.size(), 1);
        QCOMPARE(m->models[0].files[0].sha256, kSha64);
        QCOMPARE(m->models[0].files[0].size, qint64(98000000));
    }

    void invalidJsonRejected()
    {
        QString error;
        QVERIFY(!parse(QByteArrayLiteral("{ not json"), &error).has_value());
        QVERIFY(!error.isEmpty());
    }

    void wrongSchemaRejected()
    {
        QString error;
        QJsonObject root = goodRoot();
        root.insert(QStringLiteral("schema"), 2);
        QVERIFY(!parse(json(root), &error).has_value());
        QVERIFY(!error.isEmpty());
    }

    void missingPackVersionRejected()
    {
        QJsonObject root = goodRoot();
        root.remove(QStringLiteral("pack_version"));
        QVERIFY(isBad(root, nullptr));
    }

    void badPackVersionRejected()
    {
        QJsonObject root = goodRoot();
        root.insert(QStringLiteral("pack_version"), QStringLiteral("a/b"));
        QVERIFY(isBad(root, nullptr));
    }

    void missingIdRejected()
    {
        QVERIFY(isBad(rootWithModel([](QJsonObject &m) { m.remove(QStringLiteral("id")); }), nullptr));
    }

    void unknownVendorRejected()
    {
        QVERIFY(isBad(rootWithModel([](QJsonObject &m) { m.insert(QStringLiteral("vendor"), QStringLiteral("nvidia")); }), nullptr));
    }

    void unknownWorkloadRejected()
    {
        QVERIFY(isBad(rootWithModel([](QJsonObject &m) { m.insert(QStringLiteral("workload"), QStringLiteral("whisper")); }), nullptr));
    }

    void missingPrecisionRejected()
    {
        QVERIFY(isBad(rootWithModel([](QJsonObject &m) { m.remove(QStringLiteral("precision")); }), nullptr));
    }

    void emptyModelsRejected()
    {
        QJsonObject root = goodRoot();
        root.insert(QStringLiteral("models"), QJsonArray{});
        QVERIFY(isBad(root, nullptr));
    }

    void emptyFilesRejected()
    {
        QVERIFY(isBad(rootWithModel([](QJsonObject &m) { m.insert(QStringLiteral("files"), QJsonArray{}); }), nullptr));
    }

    void negativeSizeRejected()
    {
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) { f.insert(QStringLiteral("size"), -1); }), nullptr));
    }

    void zeroSizeRejected()
    {
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) { f.insert(QStringLiteral("size"), 0); }), nullptr));
    }

    void nonIntegerSizeRejected()
    {
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) { f.insert(QStringLiteral("size"), 98000000.5); }), nullptr));
    }

    void badShaLengthRejected()
    {
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) {
            f.insert(QStringLiteral("sha256"), QStringLiteral("0123456789abcdef0123456789abcdef"));
        }), nullptr));
    }

    void nonHexShaRejected()
    {
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) {
            QString bad = kSha64;
            bad[0] = QLatin1Char('g');
            f.insert(QStringLiteral("sha256"), bad);
        }), nullptr));
    }

    void uppercaseHexNormalised()
    {
        // Upper-case hex is lower-cased on the way in, so it is accepted and
        // normalised to lower-case (§6.1 doc comment).
        QString error;
        const std::optional<Manifest> m = parse(json(rootWithFile([](QJsonObject &f) {
            f.insert(QStringLiteral("sha256"), kSha64.toUpper());
        })), &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        QCOMPARE(m->models[0].files[0].sha256, kSha64);
    }

    void dotDotPathRejected()
    {
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) {
            f.insert(QStringLiteral("path"), QStringLiteral("intel/../resnet50.bin"));
        }), nullptr));
    }

    void absolutePathRejected()
    {
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) {
            f.insert(QStringLiteral("path"), QStringLiteral("/abs/resnet50.bin"));
        }), nullptr));
        QVERIFY(isBad(rootWithFile([](QJsonObject &f) {
            f.insert(QStringLiteral("path"), QStringLiteral("c:\\abs\\resnet50.bin"));
        }), nullptr));
    }

    void nonCalibratedManifestRejected()
    {
        // The frozen calibration pin (§6.1) covers the *real* pack; any test
        // fixture docs (or a tampered index) is not it.
        const QString pin = calibratedManifestSha256();
        QCOMPARE(pin.size(), 64);
        // A fixture manifest never hashes to the frozen pin.
        QVERIFY(QString::fromLatin1(
                    QCryptographicHash::hash(json(goodRoot()), QCryptographicHash::Sha256).toHex())
                != pin);
    }

    void vendorFilter()
    {
        // Build a manifest with one intel + one amd model.
        QJsonObject afile{{QStringLiteral("path"), QStringLiteral("amd/detect/a.bin")},
                          {QStringLiteral("size"), 1},
                          {QStringLiteral("sha256"), kSha64}};
        QJsonObject a{{QStringLiteral("id"), QStringLiteral("amd-model")},
                      {QStringLiteral("vendor"), QStringLiteral("amd")},
                      {QStringLiteral("workload"), QStringLiteral("detect")},
                      {QStringLiteral("precision"), QStringLiteral("INT8")},
                      {QStringLiteral("files"), QJsonArray{afile}}};
        QJsonDocument base = QJsonDocument::fromJson(json(goodRoot()));
        QJsonObject root = base.object();
        QJsonArray models = root.value(QStringLiteral("models")).toArray();
        models.append(a);
        root.insert(QStringLiteral("models"), models);

        QString error;
        const std::optional<Manifest> m = parse(json(root), &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        const QVector<ManifestModel> intel = forVendor(*m, QStringLiteral("intel"));
        QCOMPARE(intel.size(), 1);
        QCOMPARE(intel[0].id, QStringLiteral("resnet50-int8"));
        const QVector<ManifestModel> amd = forVendor(*m, QStringLiteral("AMD"));   // case-insensitive
        QCOMPARE(amd.size(), 1);
        QCOMPARE(amd[0].id, QStringLiteral("amd-model"));
        QCOMPARE(forVendor(*m, QStringLiteral("qualcomm")).size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestNpuManifest)
#include "tst_npu_manifest.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU model-pack manifest schema (Phase 4c design §6.1, §14.1, §15):
// parse the §6.1 verbatim shape, reject a wrong schema or a bad sha256 length,
// and filter a vendor's models.
#include "tests/npu/models/manifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace sb::npu;

namespace {

const QString kSha64 = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
const QString kSha32 = QStringLiteral("0123456789abcdef0123456789abcdef");

QByteArray manifestJson(int schema, const QString &sha, const QString &vendor = QStringLiteral("intel"))
{
    QJsonObject file{{QStringLiteral("path"), QStringLiteral("intel/classify/resnet50-int8/1/resnet50.bin")},
                     {QStringLiteral("size"), 98000000},
                     {QStringLiteral("sha256"), sha}};
    QJsonObject model{{QStringLiteral("id"), QStringLiteral("resnet50-int8")},
                      {QStringLiteral("vendor"), vendor},
                      {QStringLiteral("workload"), QStringLiteral("classify")},
                      {QStringLiteral("precision"), QStringLiteral("INT8")},
                      {QStringLiteral("files"), QJsonArray{file}}};
    QJsonObject root{{QStringLiteral("schema"), schema},
                     {QStringLiteral("pack_version"), QStringLiteral("1")},
                     {QStringLiteral("models"), QJsonArray{model}}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

class TestNpuManifest : public QObject
{
    Q_OBJECT

private slots:
    void wellFormedParses()
    {
        QString error;
        const std::optional<Manifest> m = parse(manifestJson(1, kSha64), &error);
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

    void wrongSchemaRejected()
    {
        QString error;
        QVERIFY(!parse(manifestJson(2, kSha64), &error).has_value());
        QVERIFY(!error.isEmpty());
    }

    void badShaLengthRejected()
    {
        QString error;
        QVERIFY(!parse(manifestJson(1, kSha32), &error).has_value());
        QVERIFY(error.contains(QStringLiteral("sha256")));
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
        QJsonDocument base = QJsonDocument::fromJson(manifestJson(1, kSha64, QStringLiteral("intel")));
        QJsonObject root = base.object();
        QJsonArray models = root.value(QStringLiteral("models")).toArray();
        models.append(a);
        root.insert(QStringLiteral("models"), models);

        QString error;
        const std::optional<Manifest> m = parse(QJsonDocument(root).toJson(QJsonDocument::Compact), &error);
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

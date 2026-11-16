// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU model-pack downloader (Phase 4c design §6.2, §14.1, §15): against a
// file:// base URL in a QTemporaryDir — a good file is SHA256-verified and
// cached, a tampered file fails closed leaving no partial, and a missing file,
// an unconfigured base URL, an unknown vendor and a cancel all report a clear
// error. No real network, no real home (a QTemporaryDir cache root and
// test_isolation.cpp's Qt test mode).
#include "tests/npu/models/downloader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace sb::npu;

namespace {

const QString kPackVersion = QStringLiteral("1");
const QString kRelPath = QStringLiteral("intel/classify/resnet50-int8/1/resnet50.bin");
const QString kVendor = QStringLiteral("intel");

QByteArray fixtureContent(int seed)
{
    QByteArray b;
    b.reserve(8192);
    for (int i = 0; i < 8192; ++i)
        b.append(char((i * 31 + seed) & 0xFF));
    return b;
}

QByteArray sha256Hex(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// A manifest describing one intel/classify model file whose bytes are `content`.
QByteArray manifestFor(const QByteArray &content)
{
    const QJsonObject file{{QStringLiteral("path"), kRelPath},
                           {QStringLiteral("size"), content.size()},
                           {QStringLiteral("sha256"), QString::fromLatin1(sha256Hex(content))}};
    const QJsonObject model{{QStringLiteral("id"), QStringLiteral("resnet50-int8")},
                            {QStringLiteral("vendor"), QStringLiteral("intel")},
                            {QStringLiteral("workload"), QStringLiteral("classify")},
                            {QStringLiteral("precision"), QStringLiteral("INT8")},
                            {QStringLiteral("files"), QJsonArray{file}}};
    const QJsonObject root{{QStringLiteral("schema"), 1},
                           {QStringLiteral("pack_version"), kPackVersion},
                           {QStringLiteral("models"), QJsonArray{model}}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// The "server" file path under a fixture dir (the source the downloader reads).
QString servedPath(const QString &fixture)
{
    return QDir(fixture).filePath(QStringLiteral("npu-models/1/") + kRelPath);
}

// The cache path the downloader computes for the pack file.
QString cachedPath(const QString &cacheRoot)
{
    return QDir(cacheRoot).filePath(QStringLiteral("npu-models/1/") + kRelPath);
}

// Writes the manifest and the served model file into `fixture` (the fake server).
// Returns false on any write failure.
bool writeServer(QTemporaryDir &fixture, const QByteArray &manifest, const QByteArray &served)
{
    const QString manifestPath = QDir(fixture.path()).filePath(QStringLiteral("npu-models/manifest.json"));
    const QString filePath = servedPath(fixture.path());
    if (!QDir().mkpath(QFileInfo(filePath).absolutePath()))
        return false;
    QFile mf(manifestPath);
    QFile sf(filePath);
    if (!mf.open(QIODevice::WriteOnly) || !sf.open(QIODevice::WriteOnly))
        return false;
    mf.write(manifest);
    sf.write(served);
    return true;
}

} // namespace

class TestNpuDownloader : public QObject
{
    Q_OBJECT

private slots:
    void goodFileCachesAndVerifies()
    {
        QTemporaryDir fixture;
        QTemporaryDir cache;
        QVERIFY(fixture.isValid());
        QVERIFY(cache.isValid());
        const QByteArray content = fixtureContent(1);
        QVERIFY(writeServer(fixture, manifestFor(content), content));

        NpuDownloader downloader(QUrl::fromLocalFile(fixture.path() + QLatin1Char('/')).toString(), cache.path());
        const DownloadResult r = downloader.download(kVendor);

        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.manifest.has_value());
        QCOMPARE(r.manifest->pack_version, kPackVersion);
        QCOMPARE(r.models.size(), 1);
        QCOMPARE(r.models[0].vendor, kVendor);
        // The file is cached, byte-for-byte, and its SHA256 matches the manifest.
        const QString cached = cachedPath(cache.path());
        QVERIFY(QFile::exists(cached));
        QFile f(cached);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), content);
        QCOMPARE(QString::fromLatin1(sha256Hex(content)), r.manifest->models[0].files[0].sha256);
    }

    void tamperedFileFailsClosed()
    {
        QTemporaryDir fixture;
        QTemporaryDir cache;
        QVERIFY(fixture.isValid());
        QVERIFY(cache.isValid());
        // The manifest declares one SHA256; the served bytes are something else.
        const QByteArray good = fixtureContent(2);
        QVERIFY(writeServer(fixture, manifestFor(good), fixtureContent(3)));

        NpuDownloader downloader(QUrl::fromLocalFile(fixture.path() + QLatin1Char('/')).toString(), cache.path());
        const DownloadResult r = downloader.download(kVendor);

        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
        // Fail closed: no partial or unverified file is left in the cache (P-NET).
        QVERIFY(!QFile::exists(cachedPath(cache.path())));
    }

    void missingFileFailsClosed()
    {
        QTemporaryDir fixture;
        QTemporaryDir cache;
        QVERIFY(fixture.isValid());
        QVERIFY(cache.isValid());
        // The manifest references a file that isn't served (a "not found").
        QVERIFY(writeServer(fixture, manifestFor(fixtureContent(4)), QByteArray{}));
        QVERIFY(QFile::remove(servedPath(fixture.path())));

        NpuDownloader downloader(QUrl::fromLocalFile(fixture.path() + QLatin1Char('/')).toString(), cache.path());
        const DownloadResult r = downloader.download(kVendor);

        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
        QVERIFY(!QFile::exists(cachedPath(cache.path())));
    }

    void notConfiguredFails()
    {
        // Empty base URL = "not configured" (D2): the pack is unavailable.
        NpuDownloader downloader(QString(), QStringLiteral("/unused"));
        const DownloadResult r = downloader.download(kVendor);
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

    void cacheHitReusesExisting()
    {
        QTemporaryDir fixture;
        QTemporaryDir cache;
        QVERIFY(fixture.isValid());
        QVERIFY(cache.isValid());
        const QByteArray content = fixtureContent(5);
        QVERIFY(writeServer(fixture, manifestFor(content), content));

        const QString baseUrl = QUrl::fromLocalFile(fixture.path() + QLatin1Char('/')).toString();
        NpuDownloader first(baseUrl, cache.path());
        const DownloadResult r1 = first.download(kVendor);
        QVERIFY2(r1.ok, qPrintable(r1.error));

        // Remove the served file: a second download must still succeed from the
        // verified cache, proving it doesn't re-fetch the (now missing) source.
        QVERIFY(QFile::remove(servedPath(fixture.path())));
        NpuDownloader second(baseUrl, cache.path());
        const DownloadResult r2 = second.download(kVendor);
        QVERIFY2(r2.ok, qPrintable(r2.error));
        QVERIFY(QFile::exists(cachedPath(cache.path())));
    }

    void unknownVendorFails()
    {
        QTemporaryDir fixture;
        QTemporaryDir cache;
        QVERIFY(fixture.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(writeServer(fixture, manifestFor(fixtureContent(6)), fixtureContent(6)));

        NpuDownloader downloader(QUrl::fromLocalFile(fixture.path() + QLatin1Char('/')).toString(), cache.path());
        const DownloadResult r = downloader.download(QStringLiteral("amd"));
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

    void cancelLeavesNoFile()
    {
        QTemporaryDir fixture;
        QTemporaryDir cache;
        QVERIFY(fixture.isValid());
        QVERIFY(cache.isValid());
        const QByteArray content = fixtureContent(7);
        QVERIFY(writeServer(fixture, manifestFor(content), content));

        NpuDownloader downloader(QUrl::fromLocalFile(fixture.path() + QLatin1Char('/')).toString(), cache.path());
        const DownloadResult r = downloader.download(kVendor, [] { return true; });
        QVERIFY(!r.ok);
        QVERIFY(!QFile::exists(cachedPath(cache.path())));
    }
};

QTEST_GUILESS_MAIN(TestNpuDownloader)
#include "tst_npu_downloader.moc"

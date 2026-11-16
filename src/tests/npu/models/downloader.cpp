// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/models/downloader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>

#include <utility>

namespace sb::npu {

namespace {

// The SHA256 of a file on disk, as 64 lower-case hex characters; empty if the
// file can't be read.
QByteArray sha256HexFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buf[64 * 1024];
    while (true) {
        const qint64 n = f.read(buf, qint64(sizeof buf));
        if (n <= 0)
            break;
        hash.addData(buf, int(n));
    }
    return hash.result().toHex();
}

} // namespace

NpuDownloader::NpuDownloader(QString baseUrl, QString cacheRoot)
    : m_baseUrl(std::move(baseUrl)),
      m_cacheRoot(std::move(cacheRoot)),
      m_nam(std::make_unique<QNetworkAccessManager>())
{
}

NpuDownloader::~NpuDownloader() = default;

QUrl NpuDownloader::urlFor(const QString &relative) const
{
    // Treat the base URL as a directory root: append the (safe, relative) path
    // to its decoded path and re-encode. The manifest schema constrains the
    // fragments here to safe relative components, so setting the path is enough.
    QUrl url(m_baseUrl);
    const QString basePath = url.path(QUrl::FullyDecoded);
    const QString joined = basePath.isEmpty()
        ? QLatin1Char('/') + relative
        : (basePath.endsWith(QLatin1Char('/')) ? basePath + relative : basePath + QLatin1Char('/') + relative);
    url.setPath(QDir::cleanPath(joined));
    return url;
}

QString NpuDownloader::cacheFilePath(const QString &packVersion, const QString &relativePath) const
{
    return QDir(m_cacheRoot).filePath(QStringLiteral("npu-models/") + packVersion + QLatin1Char('/') + relativePath);
}

QString NpuDownloader::localPath(const Manifest &manifest, const ManifestFile &file) const
{
    return cacheFilePath(manifest.pack_version, file.path);
}

bool NpuDownloader::fetchBytes(const QUrl &url, const std::function<bool()> &cancelled,
                               QByteArray *out, QString *error)
{
    QNetworkReply *reply = m_nam->get(QNetworkRequest(url));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        if (cancelled && cancelled())
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = (cancelled && cancelled())
            ? QStringLiteral("cancelled")
            : QStringLiteral("failed to download %1: %2").arg(url.toString(), reply->errorString());
        reply->deleteLater();
        if (error)
            *error = msg;
        return false;
    }
    *out = reply->readAll();
    reply->deleteLater();
    return true;
}

bool NpuDownloader::ensureFile(const QString &packVersion, const ManifestFile &file,
                               const std::function<bool()> &cancelled, QString *error)
{
    const QString target = cacheFilePath(packVersion, file.path);

    // Cached + verified: reuse the downloaded file, so re-runs don't re-fetch
    // the whole pack (§6.2 "cached"). Re-hashing also catches a file tampered
    // on disk (a silently wrong model is an EC-P +2, so fail closed).
    if (QFile::exists(target) && sha256HexFile(target) == file.sha256)
        return true;

    const QString parent = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(parent)) {
        if (error)
            *error = QStringLiteral("cannot create the model cache directory %1").arg(parent);
        return false;
    }

    QNetworkReply *reply =
        m_nam->get(QNetworkRequest(urlFor(QStringLiteral("npu-models/") + packVersion + QLatin1Char('/') + file.path)));

    // Atomic per-file write (QSaveFile): the bytes are hashed as they are
    // streamed, and committed only if the digest matches. A mismatch or a
    // failed transfer cancels the write and leaves no partial file (P-NET).
    QSaveFile save(target);
    if (!save.open(QIODevice::WriteOnly)) {
        reply->abort();
        reply->deleteLater();
        if (error)
            *error = QStringLiteral("cannot write %1").arg(target);
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QEventLoop loop;
    const auto drain = [&]() {
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            hash.addData(chunk);
            save.write(chunk);
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        if (cancelled && cancelled()) {
            reply->abort();
            return;
        }
        drain();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    drain();  // any bytes delivered after the last readyRead

    if (reply->error() != QNetworkReply::NoError) {
        save.cancelWriting();
        reply->deleteLater();
        if (cancelled && cancelled())
            *error = QStringLiteral("cancelled");
        else
            *error = QStringLiteral("failed to download %1: %2").arg(file.path, reply->errorString());
        return false;
    }
    reply->deleteLater();

    if (hash.result().toHex() != file.sha256) {
        save.cancelWriting();  // fail closed: no unverified file left behind
        if (error)
            *error = QStringLiteral("the downloaded file %1 failed its SHA256 check").arg(file.path);
        return false;
    }
    if (!save.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(target);
        return false;
    }
    return true;
}

DownloadResult NpuDownloader::download(const QString &vendor, const std::function<bool()> &cancelled)
{
    DownloadResult result;

    // Already cancelled before any I/O: short-circuit (P-STOP) without touching
    // the network or the cache.
    if (cancelled && cancelled()) {
        result.error = QStringLiteral("cancelled");
        return result;
    }

    if (m_baseUrl.isEmpty()) {
        result.error = QStringLiteral("no model-pack download source is configured");
        return result;
    }

    // 1. Fetch + parse the manifest.
    QByteArray manifestBytes;
    QString error;
    if (!fetchBytes(urlFor(QStringLiteral("npu-models/manifest.json")), cancelled, &manifestBytes, &error)) {
        result.error = error;
        return result;
    }
    const std::optional<Manifest> manifest = parse(manifestBytes, &error);
    if (!manifest) {
        result.error = error;
        return result;
    }

    // 2. Resolve the primary vendor's models (§6.2: only that vendor is fetched).
    const QVector<ManifestModel> models = forVendor(*manifest, vendor);
    if (models.isEmpty()) {
        result.error = QStringLiteral("the model pack lists no models for vendor '%1'").arg(vendor);
        return result;
    }

    // 3. Download + verify each file into the cache, failing closed.
    for (const ManifestModel &model : models) {
        for (const ManifestFile &file : model.files) {
            if (cancelled && cancelled()) {
                result.error = QStringLiteral("cancelled");
                return result;
            }
            QString fileError;
            if (!ensureFile(manifest->pack_version, file, cancelled, &fileError)) {
                result.error = fileError;
                return result;
            }
        }
    }

    result.ok = true;
    result.manifest = manifest;
    result.models = models;
    return result;
}

} // namespace sb::npu

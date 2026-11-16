// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/models/downloader.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace sb::npu {

namespace {

// The SHA256 of a file on disk, as 64 lower-case hex characters; empty if the
// file can't be read or the caller cancelled mid-hash (P-STOP: hashing a
// ~1.5–1.8 GB model in 64 KB steps must still observe a Stop).
QByteArray sha256HexFile(const QString &path, const std::function<bool()> &cancelled)
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
        hash.addData(QByteArrayView(buf, qsizetype(n)));
        if (cancelled && cancelled())
            return {};
    }
    return hash.result().toHex();
}

} // namespace

NpuDownloader::NpuDownloader(QString baseUrl, QString cacheRoot)
    : m_baseUrl(std::move(baseUrl)),
      m_cacheRoot(std::move(cacheRoot))
{
    m_baseUrl = m_baseUrl.trimmed();
}

NpuDownloader::~NpuDownloader() = default;

QUrl NpuDownloader::urlFor(const QString &relative) const
{
    // Append the (safe, relative) path onto the base URL's *encoded* path, so a
    // literal '%' in a file:// path round-trips instead of being double-decoded
    // (FullyDecoded → setPath would re-encode, mangling '%'; L1). The manifest
    // constrains `relative` to safe components (manifest.cpp); its '/' separators
    // stay literal and everything else is percent-encoded defensively.
    QUrl url(m_baseUrl);
    QString path = url.path(QUrl::FullyEncoded);
    if (path.isEmpty())
        path = QLatin1Char('/');
    else if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    path += QString::fromLatin1(QUrl::toPercentEncoding(relative, QByteArray("/")));
    url.setPath(path, QUrl::StrictMode);
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

bool NpuDownloader::fetchBytes(QNetworkAccessManager &nam, const QUrl &url,
                               const std::function<bool()> &cancelled, QByteArray *out, QString *error)
{
    // A manifest is ~arrays of metadata; cap it so a mis-served endpoint can't
    // stream a huge body into RAM (flag #2 recommendation).
    constexpr qint64 kManifestCap = 1024 * 1024;  // ~1 MiB

    QNetworkReply *reply = nam.get(QNetworkRequest(url));
    QEventLoop loop;

    // Poll cancel on a ~100 ms coarse ticker alongside the event loop, so a Stop
    // still lands during DNS/connect/TLS/headers/stall when no readyRead fires
    // (P-STOP). The QNetworkAccessManager transfer timeout (set in download())
    // separately bounds a 30 s zero-progress stall.
    QTimer watchdog;
    watchdog.setInterval(100);
    watchdog.setTimerType(Qt::CoarseTimer);
    QObject::connect(&watchdog, &QTimer::timeout, reply, [reply, &cancelled]() {
        if (cancelled && cancelled())
            reply->abort();
    });
    watchdog.start();

    bool tooLarge = false;
    // A file:// reply delivers the whole body with `finished` (no readyRead
    // first), so the timeout must *also* drain remaining bytes — otherwise a
    // local manifest is never read. `finished` is connected with `reply` as the
    // context so it is not a child of the stack-local `nam`.
    const auto drain = [&]() {
        if (cancelled && cancelled()) {
            reply->abort();
            return;
        }
        out->append(reply->readAll());
        if (out->size() > kManifestCap) {
            tooLarge = true;
            reply->abort();
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, reply, drain);
    QObject::connect(reply, &QNetworkReply::finished, reply, [&]() {
        drain();
        loop.quit();
    });
    if (!reply->isFinished())
        loop.exec();
    watchdog.stop();

    // Unconditionally consume whatever the reply holds once finished. This is
    // required for file:// (and any instantly-completing) replies: get() can
    // emit finished before the signals above are connected, so neither
    // readyRead nor finished fires and the body would otherwise sit in the
    // reply buffer with `out` empty.
    if (!(cancelled && cancelled())) {
        out->append(reply->readAll());
        if (out->size() > kManifestCap)
            tooLarge = true;
    }

    if (tooLarge) {
        reply->deleteLater();
        if (error)
            *error = QStringLiteral("the manifest at %1 exceeds the %2-byte limit")
                         .arg(url.toString())
                         .arg(kManifestCap);
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = (cancelled && cancelled())
            ? QStringLiteral("cancelled")
            : QStringLiteral("failed to download %1: %2").arg(url.toString(), reply->errorString());
        reply->deleteLater();
        if (error)
            *error = msg;
        return false;
    }
    reply->deleteLater();
    return true;
}

bool NpuDownloader::ensureFile(QNetworkAccessManager &nam, const QString &packVersion, const ManifestFile &file,
                               const std::function<bool()> &cancelled, QString *error)
{
    const QString target = cacheFilePath(packVersion, file.path);

    // Cached + verified: reuse the downloaded file, so re-runs don't re-fetch
    // the whole pack (§6.2 "cached"). Re-hashing also catches a file tampered
    // on disk (a silently wrong model is an EC-P +2, so fail closed). A
    // wrong-size cache entry skips the hash and is stale; a hash mismatch is a
    // corrupt file, removed so a later failed re-download doesn't leave it at
    // the canonical path (P-NET).
    if (QFile::exists(target)) {
        bool reused = false;
        if (QFileInfo(target).size() == file.size) {
            const QByteArray digest = sha256HexFile(target, cancelled);
            if (digest == file.sha256)
                reused = true;
            else if (!(cancelled && cancelled()))
                QFile::remove(target);
        } else {
            QFile::remove(target);
        }
        if (reused)
            return true;
    }

    const QString parent = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(parent)) {
        if (error)
            *error = QStringLiteral("cannot create the model cache directory %1").arg(parent);
        return false;
    }

    QNetworkReply *reply =
        nam.get(QNetworkRequest(urlFor(QStringLiteral("npu-models/") + packVersion + QLatin1Char('/') + file.path)));

    // Atomic per-file write (QSaveFile): the bytes are hashed as they are
    // streamed, and committed only if the digest matches. A mismatch, a short
    // write or a failed transfer cancels the write and leaves no partial file
    // (P-NET).
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
    QString writeError;  // a short/partial write, or a stream over its declared size
    qint64 received = 0;
    bool done = false;

    const auto drain = [&]() {
        if (done)
            return;
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        received += chunk.size();
        if (received > file.size) {
            // Runaway/misconfigured stream: stop before it exhausts disk or RAM
            // ahead of the post-download hash check (flag #2 recommendation).
            writeError = QStringLiteral("the downloaded file %1 is larger than its declared size").arg(file.path);
            done = true;
            reply->abort();
            return;
        }
        hash.addData(chunk);
        if (save.write(chunk) != chunk.size()) {
            // A POSIX short/partial write would otherwise commit a truncated
            // file whose hash could still match: fail closed (P-NET).
            writeError = QStringLiteral("writing %1 failed (short write)").arg(target);
            done = true;
            save.cancelWriting();
            reply->abort();
        }
    };

    QTimer watchdog;
    watchdog.setInterval(100);
    watchdog.setTimerType(Qt::CoarseTimer);
    QObject::connect(&watchdog, &QTimer::timeout, reply, [reply, &cancelled]() {
        if (cancelled && cancelled())
            reply->abort();
    });
    watchdog.start();

    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&]() {
        if (cancelled && cancelled()) {
            reply->abort();
            return;
        }
        drain();
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [&]() {
        drain();  // any bytes delivered with the final signal
        loop.quit();
    });
    if (!reply->isFinished())
        loop.exec();
    watchdog.stop();

    // Same synchronous-reply handling as fetchBytes: file:// (and any
    // instantly-completing) reply delivers the body before the signals above
    // are connected, so drain any bytes the reply still holds once finished.
    if (!(cancelled && cancelled()) && reply->error() == QNetworkReply::NoError)
        drain();

    if (reply->error() != QNetworkReply::NoError) {
        save.cancelWriting();
        reply->deleteLater();
        if (cancelled && cancelled())
            *error = QStringLiteral("cancelled");
        else if (!writeError.isEmpty())
            *error = writeError;
        else
            *error = QStringLiteral("failed to download %1: %2").arg(file.path, reply->errorString());
        return false;
    }
    reply->deleteLater();

    // A file:// (or instantly-completing) reply skips the watchdog, so poll
    // cancel once more right before committing: by now QSaveFile has at least
    // opened (a temp exists), so a Stop is still honoured and the write is
    // cleanly cancelled rather than committed (P-STOP).
    if (cancelled && cancelled()) {
        save.cancelWriting();
        reply->deleteLater();
        if (error)
            *error = QStringLiteral("cancelled");
        return false;
    }

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

    // Only https:// (with a host) or file:// (tests) is accepted: a cleartext
    // http:// base URL would let a MITM substitute models for the verified
    // SHA256 (P-NET), and a schemeless/whitespace-only value is a config
    // mistake Qt reports cryptically ("" is unknown).
    const QUrl base(m_baseUrl);
    const QString scheme = base.scheme();
    const bool httpsOk = scheme == QLatin1String("https") && !base.host().isEmpty();
    const bool fileOk = scheme == QLatin1String("file");
    if (!httpsOk && !fileOk) {
        result.error = QStringLiteral("the model-pack base URL must be an https:// or file:// URL (got '%1')")
                           .arg(m_baseUrl);
        return result;
    }

    // The QNetworkAccessManager is stack-local so it lives on the run thread
    // (design §6.2), which makes a thread-affinity bug impossible; the transfer
    // timeout is an activity timer that resets on each received byte, so only a
    // 30 s zero-progress stall (headers/connect/stall) aborts, never an active
    // download (P-STOP).
    QNetworkAccessManager nam;
    nam.setTransferTimeout(30000);

    // 1. Fetch + parse the manifest.
    QByteArray manifestBytes;
    QString error;
    if (!fetchBytes(nam, urlFor(QStringLiteral("npu-models/manifest.json")), cancelled, &manifestBytes, &error)) {
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
            if (!ensureFile(nam, manifest->pack_version, file, cancelled, &fileError)) {
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

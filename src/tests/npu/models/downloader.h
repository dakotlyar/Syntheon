// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU model-pack downloader (Phase 4c design §6.2, §7 step 2): fetches the
// versioned manifest, resolves the detected vendor's models, then downloads
// each file — atomically (QSaveFile) — into the app-data cache, verifying SHA256
// against the manifest and failing closed (P-NET). Supports file:// (tests) and
// http(s) (production); cancellation is polled between chunks (P-STOP).
//
// The base URL and the cache root are supplied by the caller — the Settings
// value (SettingsStore::npuModelsBaseUrl(), D2) and the resolved data location
// (Storage::dataDir()), read by NpuTest (§3.4, §7). This class therefore has no
// settings or storage dependency (design §1.2: syntheon_test_npu depends on
// QtCore/QtNetwork/syntheon_core/syntheon_test_common, never syntheon_storage).
// An empty base URL means "not configured".
//
// Layout (server and cache mirror each other, §6.1/§6.2):
//   manifest   <base_url>/npu-models/manifest.json
//   each file  <base_url>/npu-models/<pack_version>/<path>
//   cache file <cache_root>/npu-models/<pack_version>/<path>

#include "tests/npu/models/manifest.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QUrl;

namespace sb::npu {

// The outcome of one download(). Fail-closed (P-NET): on anything that cannot be
// verified, ok is false, `error` explains it, and no unverified file remains in
// the cache.
struct DownloadResult {
    bool ok = false;
    QString error;
    std::optional<Manifest> manifest;   // the parsed manifest (set when ok)
    QVector<ManifestModel> models;      // the downloaded vendor's models (set when ok)
};

class NpuDownloader
{
public:
    // baseUrl: the pack root — "https://…" or "file:///…" (tests); "" = not configured.
    // cacheRoot: the app data location (Storage::dataDir()).
    // expectedManifestSha256: the compiled-in calibration pin (§6.1 "The
    // calibration SHA-pin"), 64 lower-case hex. Empty = no top-level manifest
    // pin check (do NOT use in production — the app pins the calibrated pack).
    // When set, a manifest whose bytes don't hash to it → download fails closed.
    explicit NpuDownloader(QString baseUrl, QString cacheRoot, QString expectedManifestSha256 = {});
    ~NpuDownloader();
    NpuDownloader(const NpuDownloader &) = delete;
    NpuDownloader &operator=(const NpuDownloader &) = delete;

    QString baseUrl() const { return m_baseUrl; }
    QString cacheRoot() const { return m_cacheRoot; }

    // Fetch the manifest, resolve `vendor`'s models (§6.2 downloads only the
    // primary vendor), then download + SHA256-verify each file into the cache.
    // A file already present with a matching SHA256 is reused (the "cached"
    // path). `cancelled` is polled between chunks and files (P-STOP); on cancel
    // the in-flight file is discarded and ok=false with error "cancelled".
    DownloadResult download(const QString &vendor, const std::function<bool()> &cancelled = {});

    // The cache path of a manifest file: <cacheRoot>/npu-models/<pack_version>/<path>.
    // Safe by construction — pack_version is a single safe component and
    // `file.path` is a validated relative path (manifest.cpp), so nothing can
    // escape npu-models/<pack_version>/ (P-NET).
    QString localPath(const Manifest &manifest, const ManifestFile &file) const;

private:
    QUrl urlFor(const QString &relative) const;
    QString cacheFilePath(const QString &packVersion, const QString &relativePath) const;
    bool fetchBytes(QNetworkAccessManager &nam, const QUrl &url, const std::function<bool()> &cancelled,
                    QByteArray *out, QString *error);
    bool ensureFile(QNetworkAccessManager &nam, const QString &packVersion, const ManifestFile &file,
                    const std::function<bool()> &cancelled, QString *error);

    QString m_baseUrl;
    QString m_cacheRoot;
    QString m_expectedManifestSha256;
};

} // namespace sb::npu

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU model-pack manifest schema (Phase 4c design §6.1, D6): a versioned
// JSON document at <base_url>/npu-models/manifest.json listing, per vendor ×
// workload, the model files with their size + SHA256. Pure (QtCore only):
// parse + validate here, the downloader verifies against it.

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace sb::npu {

struct ManifestFile {
    QString path;      // "intel/classify/resnet50-int8/1/resnet50.xml" (relative to the pack root)
    qint64 size = 0;   // bytes
    QString sha256;    // 64 lower-case hex characters
};

struct ManifestModel {
    QString id;         // "resnet50-int8" (stable; the model.<workload> source)
    QString vendor;     // "intel" | "amd" | "qualcomm"
    QString workload;   // "classify" | "detect" | "llm"
    QString precision;  // "INT8" | "INT4" (text)
    QVector<ManifestFile> files;
};

struct Manifest {
    int schema = 0;          // must be 1 (§6.1)
    QString pack_version;    // "1"; anchors + scores tie to this (N5)
    QVector<ManifestModel> models;
};

// Parses + validates the §6.1 manifest JSON. Returns nullopt and sets *error
// on anything malformed: not an object, an unsupported schema, a missing/empty
// pack_version, a model missing/unknown id/vendor/workload/precision, a file
// with an unsafe path (absolute, drive letter, or ".." traversal), a
// non-positive/non-integer size, or a sha256 that isn't 64 lower-case hex
// characters. sha256 strings are lower-cased on the way in (so upper-case hex
// is accepted and normalised).
std::optional<Manifest> parse(const QByteArray &json, QString *error);

// The models for one vendor (case-insensitive), in manifest order (N3: the
// downloader only fetches the detected vendor's files).
QVector<ManifestModel> forVendor(const Manifest &manifest, const QString &vendor);

} // namespace sb::npu

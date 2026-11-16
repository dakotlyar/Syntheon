// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/models/manifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace sb::npu {

namespace {
constexpr int kSchema = 1;
constexpr int kSha256Len = 64;
} // namespace

std::optional<Manifest> parse(const QByteArray &json, QString *error)
{
    const auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return std::optional<Manifest>{};
    };

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("the manifest isn't valid JSON (%1)").arg(perr.errorString()));
    const QJsonObject root = doc.object();

    const int schema = root.value(QStringLiteral("schema")).toInt();
    if (schema != kSchema)
        return fail(QStringLiteral("unsupported manifest schema %1 (expected %2)").arg(schema).arg(kSchema));

    const QString version = root.value(QStringLiteral("pack_version")).toString();
    if (version.isEmpty())
        return fail(QStringLiteral("the manifest has no pack_version"));

    Manifest m;
    m.schema = schema;
    m.pack_version = version;

    const QJsonArray models = root.value(QStringLiteral("models")).toArray();
    for (const QJsonValue &mv : models) {
        const QJsonObject mo = mv.toObject();
        ManifestModel model;
        model.id = mo.value(QStringLiteral("id")).toString();
        model.vendor = mo.value(QStringLiteral("vendor")).toString();
        model.workload = mo.value(QStringLiteral("workload")).toString();
        model.precision = mo.value(QStringLiteral("precision")).toString();
        if (model.id.isEmpty() || model.vendor.isEmpty() || model.workload.isEmpty())
            return fail(QStringLiteral("a model entry is missing id/vendor/workload"));

        const QJsonArray files = mo.value(QStringLiteral("files")).toArray();
        if (files.isEmpty())
            return fail(QStringLiteral("model %1 lists no files").arg(model.id));
        for (const QJsonValue &fv : files) {
            const QJsonObject fo = fv.toObject();
            ManifestFile file;
            file.path = fo.value(QStringLiteral("path")).toString();
            file.sha256 = fo.value(QStringLiteral("sha256")).toString().toLower();
            const QJsonValue size = fo.value(QStringLiteral("size"));
            if (file.path.isEmpty())
                return fail(QStringLiteral("a file of model %1 has no path").arg(model.id));
            if (!size.isDouble() || size.toDouble() < 0)
                return fail(QStringLiteral("file %1 has a bad size").arg(file.path));
            file.size = qint64(size.toDouble());
            if (file.sha256.size() != kSha256Len)
                return fail(QStringLiteral("file %1 has a sha256 of length %2 (expected 64)")
                                .arg(file.path)
                                .arg(file.sha256.size()));
            model.files.append(file);
        }
        m.models.append(model);
    }

    if (m.models.isEmpty())
        return fail(QStringLiteral("the manifest lists no models"));
    return m;
}

QVector<ManifestModel> forVendor(const Manifest &manifest, const QString &vendor)
{
    QVector<ManifestModel> out;
    for (const ManifestModel &model : manifest.models)
        if (model.vendor.compare(vendor, Qt::CaseInsensitive) == 0)
            out.append(model);
    return out;
}

} // namespace sb::npu

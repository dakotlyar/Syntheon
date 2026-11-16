// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/models/manifest.h"

#include "tests/npu/ids.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

#include <cmath>

namespace sb::npu {

namespace {
constexpr int kSchema = 1;
constexpr int kSha256Len = 64;
constexpr double kMaxFileSize = 1.0e15;   // 1 PB: safely below 2^53 and qint64 max

bool isLowerHex(const QString &s)
{
    for (const QChar c : s) {
        const char ch = c.toLatin1();
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return true;
}

// pack_version is a single, safe path component: no separators, not a dot path.
bool isSafeVersion(const QString &v)
{
    if (v.isEmpty())
        return false;
    return !v.contains(QLatin1Char('/')) && !v.contains(QLatin1Char('\\'))
           && v != QLatin1StringView("..") && v != QLatin1StringView(".");
}

// A manifest file path must stay inside the pack root (P-NET): relative, no
// leading separator, no drive letter, no ".." traversal segment.
bool isSafeRelativePath(const QString &p)
{
    if (p.isEmpty())
        return false;
    if (p.startsWith(QLatin1Char('/')) || p.startsWith(QLatin1Char('\\')))
        return false;
    if (p.size() >= 2 && p.at(1) == QLatin1Char(':'))   // "C:…"
        return false;
    const QStringList segs = p.split(QRegularExpression(QStringLiteral("[/\\\\]")), Qt::SkipEmptyParts);
    for (const QString &seg : segs)
        if (seg == QLatin1StringView(".."))
            return false;
    return true;
}
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
    if (!isSafeVersion(version))
        return fail(QStringLiteral("the manifest has a bad pack_version"));

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
        if (model.id.isEmpty())
            return fail(QStringLiteral("a model entry is missing its id"));
        if (!vendorFromId(QStringView{model.vendor}))
            return fail(QStringLiteral("model %1 has an unknown vendor '%2'").arg(model.id, model.vendor));
        if (!workloadFromId(QStringView{model.workload}))
            return fail(QStringLiteral("model %1 has an unknown workload '%2'").arg(model.id, model.workload));
        if (model.precision.isEmpty())
            return fail(QStringLiteral("model %1 is missing a precision").arg(model.id));

        const QJsonArray files = mo.value(QStringLiteral("files")).toArray();
        if (files.isEmpty())
            return fail(QStringLiteral("model %1 lists no files").arg(model.id));
        for (const QJsonValue &fv : files) {
            const QJsonObject fo = fv.toObject();
            ManifestFile file;
            file.path = fo.value(QStringLiteral("path")).toString();
            if (!isSafeRelativePath(file.path))
                return fail(QStringLiteral("file of model %1 has an unsafe path '%2'").arg(model.id, file.path));

            const QJsonValue size = fo.value(QStringLiteral("size"));
            const double bytes = size.isDouble() ? size.toDouble() : -1.0;
            // Fail closed on 0, negative, non-integer, or absurdly large sizes.
            if (bytes <= 0.0 || bytes > kMaxFileSize || std::floor(bytes) != bytes)
                return fail(QStringLiteral("file %1 has a bad size").arg(file.path));
            file.size = qint64(bytes);

            file.sha256 = fo.value(QStringLiteral("sha256")).toString().toLower();
            if (file.sha256.size() != kSha256Len || !isLowerHex(file.sha256))
                return fail(QStringLiteral("file %1 has a sha256 that isn't 64 lower-case hex chars").arg(file.path));

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

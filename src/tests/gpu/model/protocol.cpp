// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <limits>

namespace sb::gpu::protocol {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

QString S(const char *s) { return QString::fromLatin1(s); }

GpuKind kindFromText(const QString &t)
{
    for (const GpuKind k : {GpuKind::Discrete, GpuKind::Integrated, GpuKind::Software}) {
        if (kindText(k) == t)
            return k;
    }
    return GpuKind::Other;
}

// ---- encode ----

QJsonObject encodeGpu(const GpuEntry &g)
{
    QJsonObject apis;
    for (auto it = g.apis.cbegin(); it != g.apis.cend(); ++it) {
        QJsonObject a{{S("ok"), it->ok}};
        if (it->ok) {
            a.insert(S("version"), it->version);
            a.insert(S("driver"), it->driver);
            a.insert(S("note"), it->note);
        } else {
            a.insert(S("reason"), it->reason);
        }
        apis.insert(it.key(), a);
    }
    return {{S("ev"), S("gpu")},
            {S("id"), g.id},
            {S("name"), g.name},
            {S("vendorId"), double(g.vendorId)},
            {S("deviceId"), double(g.deviceId)},
            {S("kind"), kindText(g.kind)},
            {S("vramBytes"), double(g.vramBytes)},
            {S("pciSlot"), g.pciSlot},
            {S("kernelDriver"), g.kernelDriver},
            {S("apis"), apis}};
}

QJsonObject encodeEvent(const Event &event)
{
    return std::visit(
        Overloaded{
            [](const GpuEvent &e) { return encodeGpu(e.gpu); },
            [](const GlEvent &e) {
                QJsonObject o{{S("ev"), S("gl")}, {S("ok"), e.ok}, {S("version"), e.version}, {S("renderer"), e.renderer},
                              {S("vendor"), e.vendor}, {S("vendorId"), double(e.vendorId)}, {S("note"), e.note}};
                if (!e.ok)
                    o.insert(S("reason"), e.reason);
                return o;
            },
            [](const ReadyEvent &e) {
                return QJsonObject{{S("ev"), S("ready")}, {S("api"), e.api}, {S("device"), e.device}, {S("version"), e.version},
                                   {S("driver"), e.driver}, {S("present"), e.present}, {S("note"), e.note}, {S("layers"), e.layers},
                                   {S("renderW"), e.renderW}, {S("renderH"), e.renderH}, {S("windowW"), e.windowW},
                                   {S("windowH"), e.windowH}, {S("scale"), e.scale}, {S("screen"), e.screen}, {S("fit"), e.fit},
                                   {S("gpuTimestamps"), e.gpuTimestamps}};
            },
            [](const StageEvent &e) {
                return QJsonObject{{S("ev"), S("stage")}, {S("workload"), e.workload}, {S("phase"), e.phase}, {S("seconds"), e.seconds}};
            },
            [](const ResultEvent &e) {
                QJsonArray series;
                for (const QPointF &p : e.series)
                    series.append(QJsonArray{p.x(), p.y()});
                QJsonObject o{{S("ev"), S("result")}, {S("workload"), e.workload}, {S("frames"), e.frames},
                              {S("seconds"), e.seconds}, {S("ftMs"), QJsonObject{{S("p1"), e.ftP1}, {S("p50"), e.ftP50}, {S("p99"), e.ftP99}}},
                              {S("fps"), e.fps}, {S("cpuMs"), e.cpuMs}, {S("series"), series},
                              {S("sig"), QString::fromLatin1(e.signature.toHex())}};
                if (e.gpuMs)
                    o.insert(S("gpuMs"), *e.gpuMs);
                return o;
            },
            [](const ErrorEvent &e) {
                return QJsonObject{{S("ev"), S("error")}, {S("workload"), e.workload}, {S("reason"), e.reason}, {S("na"), e.na}};
            },
            [](const DoneEvent &) { return QJsonObject{{S("ev"), S("done")}}; }},
        event);
}

// ---- parse ----

enum class Type { String, Number, Int, Bool, Object, Array };

bool typeOk(const QJsonValue &v, Type t)
{
    switch (t) {
    case Type::String: return v.isString();
    case Type::Number: return v.isDouble();
    case Type::Bool: return v.isBool();
    case Type::Object: return v.isObject();
    case Type::Array: return v.isArray();
    case Type::Int: {
        if (!v.isDouble())
            return false;
        const double d = v.toDouble();
        return d == std::floor(d) && d >= 0 && d <= double(std::numeric_limits<int>::max());
    }
    }
    return false;
}

// A required key: present ("<ev>: missing <key>") and of the right type
// ("<ev>: bad <key>"): anything else is a malformed line.
bool need(const QJsonObject &o, const char *ev, const char *key, Type type, QString *error)
{
    const auto it = o.constFind(S(key));
    if (it == o.constEnd()) {
        if (error)
            *error = QStringLiteral("%1: missing %2").arg(S(ev), S(key));
        return false;
    }
    if (!typeOk(it.value(), type)) {
        if (error)
            *error = QStringLiteral("%1: bad %2").arg(S(ev), S(key));
        return false;
    }
    return true;
}

// An optional key: absent is fine, present must be of the right type.
bool optional(const QJsonObject &o, const char *ev, const char *key, Type type, QString *error)
{
    return !o.contains(S(key)) || need(o, ev, key, type, error);
}

std::optional<Event> parseGpu(const QJsonObject &o, QString *error)
{
    for (const char *k : {"id", "name"}) {
        if (!need(o, "gpu", k, Type::String, error))
            return std::nullopt;
    }
    GpuEntry g;
    g.id = o.value(S("id")).toString();
    g.name = o.value(S("name")).toString();
    g.vendorId = quint32(o.value(S("vendorId")).toDouble());
    g.deviceId = quint32(o.value(S("deviceId")).toDouble());
    g.kind = kindFromText(o.value(S("kind")).toString());
    g.vramBytes = qint64(o.value(S("vramBytes")).toDouble());
    g.pciSlot = o.value(S("pciSlot")).toString();
    g.kernelDriver = o.value(S("kernelDriver")).toString();
    const QJsonObject apis = o.value(S("apis")).toObject();
    for (auto it = apis.begin(); it != apis.end(); ++it) {
        const QJsonObject a = it.value().toObject();
        ApiAvailability av;
        av.ok = a.value(S("ok")).toBool();
        av.version = a.value(S("version")).toString();
        av.driver = a.value(S("driver")).toString();
        av.note = a.value(S("note")).toString();
        av.reason = a.value(S("reason")).toString();
        g.apis.insert(it.key(), av);
    }
    return Event{GpuEvent{g}};
}

std::optional<Event> parseGl(const QJsonObject &o, QString *error)
{
    if (!need(o, "gl", "ok", Type::Bool, error))
        return std::nullopt;
    GlEvent e;
    e.ok = o.value(S("ok")).toBool();
    e.version = o.value(S("version")).toString();
    e.renderer = o.value(S("renderer")).toString();
    e.vendor = o.value(S("vendor")).toString();
    e.note = o.value(S("note")).toString();
    e.reason = o.value(S("reason")).toString();
    e.vendorId = quint32(o.value(S("vendorId")).toDouble());
    return Event{e};
}

std::optional<Event> parseReady(const QJsonObject &o, QString *error)
{
    for (const char *k : {"api", "device"}) {
        if (!need(o, "ready", k, Type::String, error))
            return std::nullopt;
    }
    for (const char *k : {"renderW", "renderH", "windowW", "windowH"}) {
        if (!need(o, "ready", k, Type::Int, error))
            return std::nullopt;
    }
    if (!optional(o, "ready", "scale", Type::Number, error))
        return std::nullopt;
    ReadyEvent e;
    e.api = o.value(S("api")).toString();
    e.device = o.value(S("device")).toString();
    e.version = o.value(S("version")).toString();
    e.driver = o.value(S("driver")).toString();
    e.present = o.value(S("present")).toString();
    e.note = o.value(S("note")).toString();
    e.layers = o.value(S("layers")).toString();
    e.screen = o.value(S("screen")).toString();
    e.renderW = o.value(S("renderW")).toInt();
    e.renderH = o.value(S("renderH")).toInt();
    e.windowW = o.value(S("windowW")).toInt();
    e.windowH = o.value(S("windowH")).toInt();
    e.scale = o.value(S("scale")).toDouble(1.0);
    e.fit = o.value(S("fit")).toBool();
    e.gpuTimestamps = o.value(S("gpuTimestamps")).toBool();
    return Event{e};
}

std::optional<Event> parseStage(const QJsonObject &o, QString *error)
{
    for (const char *k : {"workload", "phase"}) {
        if (!need(o, "stage", k, Type::String, error))
            return std::nullopt;
    }
    if (!need(o, "stage", "seconds", Type::Int, error))
        return std::nullopt;
    return Event{StageEvent{o.value(S("workload")).toString(), o.value(S("phase")).toString(), o.value(S("seconds")).toInt()}};
}

std::optional<Event> parseResult(const QJsonObject &o, QString *error)
{
    struct Key {
        const char *name;
        Type type;
    };
    for (const Key &k : {Key{"workload", Type::String}, Key{"frames", Type::Int}, Key{"seconds", Type::Number},
                         Key{"fps", Type::Number}, Key{"ftMs", Type::Object}, Key{"cpuMs", Type::Number}, Key{"sig", Type::String}}) {
        if (!need(o, "result", k.name, k.type, error))
            return std::nullopt;
    }
    if (!optional(o, "result", "gpuMs", Type::Number, error) || !optional(o, "result", "series", Type::Array, error))
        return std::nullopt;
    const QJsonObject ft = o.value(S("ftMs")).toObject();
    for (const char *k : {"p1", "p50", "p99"}) {
        if (!need(ft, "result", k, Type::Number, error)) {
            if (error)
                error->replace(QStringLiteral(": "), QStringLiteral(": ftMs."));
            return std::nullopt;
        }
    }
    ResultEvent e;
    e.workload = o.value(S("workload")).toString();
    e.frames = o.value(S("frames")).toInt();
    e.seconds = o.value(S("seconds")).toDouble();
    e.fps = o.value(S("fps")).toDouble();
    e.ftP1 = ft.value(S("p1")).toDouble();
    e.ftP50 = ft.value(S("p50")).toDouble();
    e.ftP99 = ft.value(S("p99")).toDouble();
    e.cpuMs = o.value(S("cpuMs")).toDouble();
    if (o.contains(S("gpuMs")))
        e.gpuMs = o.value(S("gpuMs")).toDouble();
    for (const QJsonValue &v : o.value(S("series")).toArray()) {
        const QJsonArray p = v.toArray();
        if (p.size() != 2) {
            if (error)
                *error = S("result: bad series");
            return std::nullopt;
        }
        e.series.append(QPointF(p[0].toDouble(), p[1].toDouble()));
    }
    const QByteArray hex = o.value(S("sig")).toString().toLatin1();
    const auto decoded = QByteArray::fromHex(hex);
    const auto isHex = [](const QByteArray &h) {
        if (h.size() % 2)
            return false;
        for (const char c : h) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        return true;
    };
    if (!isHex(hex)) {
        if (error)
            *error = S("result: bad sig");
        return std::nullopt;
    }
    e.signature = decoded;
    return Event{e};
}

std::optional<Event> parseError(const QJsonObject &o, QString *error)
{
    if (!need(o, "error", "reason", Type::String, error))
        return std::nullopt;
    return Event{ErrorEvent{o.value(S("workload")).toString(), o.value(S("reason")).toString(), o.value(S("na")).toBool()}};
}

} // namespace

QString planText(const QVector<PlanItem> &plan)
{
    QStringList parts;
    for (const PlanItem &p : plan)
        parts.append(QStringLiteral("%1:%2:%3").arg(p.workload).arg(p.warmupMs).arg(p.measureMs));
    return parts.join(QLatin1Char(','));
}

std::optional<QVector<PlanItem>> parsePlan(const QString &text, QString *error)
{
    QVector<PlanItem> plan;
    if (text.isEmpty())
        return plan;
    for (const QString &item : text.split(QLatin1Char(','))) {
        const QStringList f = item.split(QLatin1Char(':'));
        bool okW = false, okM = false;
        const int warm = f.size() == 3 ? f[1].toInt(&okW) : 0;
        const int meas = f.size() == 3 ? f[2].toInt(&okM) : 0;
        if (f.size() != 3 || f[0].isEmpty() || !okW || !okM || warm < 0 || meas < 0) {
            if (error)
                *error = QStringLiteral("bad plan item '%1'").arg(item);
            return std::nullopt;
        }
        plan.append({f[0], warm, meas});
    }
    return plan;
}

std::optional<Event> parseLine(const QByteArray &line, QString *error)
{
    if (line.size() > kMaxLine) {
        if (error)
            *error = S("line too long");
        return std::nullopt;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = S("not a JSON object");
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    const QString ev = o.value(S("ev")).toString();
    if (ev.isEmpty()) {
        if (error)
            *error = S("no ev");
        return std::nullopt;
    }
    if (ev == QLatin1StringView("gpu"))
        return parseGpu(o, error);
    if (ev == QLatin1StringView("gl"))
        return parseGl(o, error);
    if (ev == QLatin1StringView("ready"))
        return parseReady(o, error);
    if (ev == QLatin1StringView("stage"))
        return parseStage(o, error);
    if (ev == QLatin1StringView("result"))
        return parseResult(o, error);
    if (ev == QLatin1StringView("error"))
        return parseError(o, error);
    if (ev == QLatin1StringView("done"))
        return Event{DoneEvent{}};
    if (error)
        *error = QStringLiteral("unknown event '%1'").arg(ev);
    return std::nullopt;
}

QByteArray encode(const Event &event)
{
    return QJsonDocument(encodeEvent(event)).toJson(QJsonDocument::Compact);
}

} // namespace sb::gpu::protocol

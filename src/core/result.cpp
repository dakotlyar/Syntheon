#include "core/result.h"

namespace sb {

const Metric *TestResult::metric(const QString &key) const
{
    for (const Metric &m : metrics) {
        if (m.key == key)
            return &m;
    }
    return nullptr;
}

TestResult TestResult::withStatus(Status status, const QString &reason)
{
    TestResult r;
    r.status = status;
    r.statusReason = reason;
    return r;
}

namespace {
const QLatin1StringView kVersionKey("sysinfo_version");
const QLatin1StringView kMachineLabelKey("machine_label");
} // namespace

QJsonObject SystemSnapshot::toJson() const
{
    QJsonObject json = data;
    json.insert(kVersionKey, version);
    json.insert(kMachineLabelKey, machineLabel);
    return json;
}

SystemSnapshot SystemSnapshot::fromJson(const QJsonObject &json)
{
    SystemSnapshot s;
    s.version = json.value(kVersionKey).toInt(0);
    s.machineLabel = json.value(kMachineLabelKey).toString();
    s.data = json;
    s.data.remove(kVersionKey);
    s.data.remove(kMachineLabelKey);
    return s;
}

std::optional<double> computeOverall(const QVector<TestResult> &tests,
                                     const std::function<bool(const QString &)> &countsTowardOverall)
{
    std::optional<double> sum;
    for (const TestResult &t : tests) {
        if (!t.score || !countsTowardOverall(t.testId))
            continue;
        sum = sum.value_or(0.0) + *t.score;
    }
    return sum;
}

} // namespace sb

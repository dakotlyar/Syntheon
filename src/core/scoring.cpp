// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/scoring.h"

#include <cmath>

namespace sb {

std::optional<double> weightedScore(const TestResult &result, double constant,
                                    const QVector<ScoreWeight> &weights)
{
    double sum = 0.0;
    for (const ScoreWeight &w : weights) {
        const Metric *m = result.metric(w.metricKey);
        if (!m || !m->isNumeric())
            return std::nullopt;
        sum += w.weight * std::get<double>(m->value);
    }
    return std::round(constant * sum);
}

GeometricScore weightedGeometricScore(const TestResult &result, double constant, const QVector<GeometricGroup> &groups)
{
    GeometricScore out;
    double logTotal = 0.0;
    bool complete = !groups.isEmpty();
    for (const GeometricGroup &g : groups) {
        double logGroup = 0.0;
        bool present = !g.metrics.isEmpty();
        for (const GeometricMetric &k : g.metrics) {
            const Metric *m = result.metric(k.metricKey);
            if (!m || !m->isNumeric() || !(std::get<double>(m->value) > 0) || !(k.anchor > 0)) {
                present = false;
                break;
            }
            const double r = std::get<double>(m->value);
            logGroup += k.weight * std::log(k.lowerIsBetter ? k.anchor / r : r / k.anchor);
        }
        if (!present) {
            complete = false;
            continue;
        }
        out.groups.insert(g.id, std::round(constant * std::exp(logGroup)));
        logTotal += g.weight * logGroup;
    }
    if (complete)
        out.total = std::round(constant * std::exp(logTotal));
    return out;
}

} // namespace sb

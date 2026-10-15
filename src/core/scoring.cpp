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

} // namespace sb

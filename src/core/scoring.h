// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/result.h"

#include <QMap>
#include <QString>
#include <QVector>

#include <optional>

namespace sb {

// The shape every score formula takes: score = round(C × Σ wᵢ·mᵢ).
// Phase 1 stubs use it with made-up weights; real phases use it with theirs.
struct ScoreWeight {
    QString metricKey;
    double weight = 0.0;
};

// Returns nullopt if any weighted metric is missing or isn't numeric, so a
// partial measurement never produces a misleading score.
std::optional<double> weightedScore(const TestResult &result, double constant,
                                    const QVector<ScoreWeight> &weights);

// The weighted geometric mean (Phase 2 design §7.2): each metric is divided
// by its anchor, n = r / A; a group's score is G = exp(sum v.ln n) with the
// metrics' weights v summing to 1; the total is round(C x exp(sum W.ln G))
// over the groups, their weights W summing to 1. Unit-free: an anchor only
// rescales every result by the same factor.
struct GeometricMetric {
    QString metricKey;
    double anchor = 1.0;
    double weight = 0.0;  // within its group
};
struct GeometricGroup {
    QString id;      // a sub-test id
    double weight = 0.0;
    QVector<GeometricMetric> metrics;
};
struct GeometricScore {
    // nullopt unless every group has every metric (the rule weightedScore()
    // follows: a partial measurement never produces a misleading score).
    std::optional<double> total;
    // round(C x G) for each group whose metrics are all there.
    QMap<QString, double> groups;
};
// A metric that is missing, not numeric, or not positive (no logarithm)
// makes its group missing.
GeometricScore weightedGeometricScore(const TestResult &result, double constant, const QVector<GeometricGroup> &groups);

} // namespace sb

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/result.h"

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

} // namespace sb

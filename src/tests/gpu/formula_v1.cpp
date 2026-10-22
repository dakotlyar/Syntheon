// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/formula_v1.h"

namespace sb::gpu::formula_v1 {

namespace {
constexpr double kProvisionalFps = 100.0;   // versions 0: replaced by the calibration (task 36)
constexpr double kProvisionalMops = 1.0;
}

const QVector<GeometricGroup> &groups()
{
    static const QVector<GeometricGroup> g = {
        {QStringLiteral("scenes3d"), 0.80,
         {{QStringLiteral("scenes3d.geometry.fps"), kProvisionalFps, 0.20},
          {QStringLiteral("scenes3d.drawcalls.fps"), kProvisionalFps, 0.20},
          {QStringLiteral("scenes3d.shading.fps"), kProvisionalFps, 0.30},
          {QStringLiteral("scenes3d.particles.fps"), kProvisionalFps, 0.30}}},
        // §11.1: Qt raster's MOps/s (D8), 0.25 each; provisional anchors of 1 MOps/s (task 36 freezes them).
        {QStringLiteral("native2d"), 0.10,
         {{QStringLiteral("native2d.shapes"), kProvisionalMops, 0.25},
          {QStringLiteral("native2d.text"), kProvisionalMops, 0.25},
          {QStringLiteral("native2d.blits"), kProvisionalMops, 0.25},
          {QStringLiteral("native2d.gradients"), kProvisionalMops, 0.25}}},
    };
    return g;
}

ScoreBreakdown score(const TestResult &result)
{
    const GeometricScore s = weightedGeometricScore(result, kScale, groups());
    ScoreBreakdown b;
    b.subScores = s.groups;
    // A group whose part ended in an Error (or N/A, Stop) gets no sub-score
    // even if its metrics are all there.
    for (const SubResult &sub : result.subResults) {
        if (sub.status != Status::Ok)
            b.subScores.remove(sub.id);
    }
    // Only an Ok result gets a total; a Partial keeps the sub-scores of its complete groups.
    if (result.status == Status::Ok && b.subScores.size() == s.groups.size())
        b.total = s.total;
    return b;
}

} // namespace sb::gpu::formula_v1

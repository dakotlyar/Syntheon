// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/formula_v1.h"

#include <cmath>

namespace sb::gpu::formula_v1 {

namespace {
// Anchors (frozen, formula v1): the median of each scored metric over 3 runs on Knowhere (Knowhere = 1,000), rounded to
// 3 significant figures. Radeon 8060S (Strix Halo), CachyOS, RADV (Mesa 26.2.4), Wayland, the display at 125% (D21) so the
// render size is 2400x1350 in a 1920x1080 window. Build linux-x64 5570135, GPU results 20-22 (runs 9-11), 2026-10-23, AC,
// balanced profile, background load 0.3-0.8%, no driver settings or overlays, other apps closed. Never change one
// without bumping formulaVersion.
constexpr double kGeometryFps = 673, kDrawcallsFps = 273, kShadingFps = 122, kParticlesFps = 98.4;
constexpr double kShapes = 0.0194, kText = 1.8, kBlits = 0.0132, kGradients = 0.0217;  // Qt raster, MOps/s
constexpr double kQuadsGpx = 74.4, kSpritesMops = 38.9;
}

const QVector<GeometricGroup> &groups()
{
    static const QVector<GeometricGroup> g = {
        {QStringLiteral("scenes3d"), 0.80,
         {{QStringLiteral("scenes3d.geometry.fps"), kGeometryFps, 0.20},
          {QStringLiteral("scenes3d.drawcalls.fps"), kDrawcallsFps, 0.20},
          {QStringLiteral("scenes3d.shading.fps"), kShadingFps, 0.30},
          {QStringLiteral("scenes3d.particles.fps"), kParticlesFps, 0.30}}},
        // §11.1: Qt raster's MOps/s (D8), 0.25 each.
        {QStringLiteral("native2d"), 0.10,
         {{QStringLiteral("native2d.shapes"), kShapes, 0.25},
          {QStringLiteral("native2d.text"), kText, 0.25},
          {QStringLiteral("native2d.blits"), kBlits, 0.25},
          {QStringLiteral("native2d.gradients"), kGradients, 0.25}}},
        // §11.1: the best API's fill rates, 0.50 each.
        {QStringLiteral("fill2d"), 0.10,
         {{QStringLiteral("fill2d.quads"), kQuadsGpx, 0.50},
          {QStringLiteral("fill2d.sprites"), kSpritesMops, 0.50}}},
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

std::optional<double> score2d(const TestResult &result)
{
    // Exact groups: the constant is large enough that rounding the group scores costs nothing, so the combined score is
    // round(1000 x sqrt(G_native x G_fill)) of the real G, not of two rounded sub-scores.
    constexpr double kExact = 1e9;
    const GeometricScore s = weightedGeometricScore(result, kExact, groups());
    const auto native = s.groups.constFind(QStringLiteral("native2d"));
    const auto fill = s.groups.constFind(QStringLiteral("fill2d"));
    if (native == s.groups.cend() || fill == s.groups.cend())
        return std::nullopt;
    for (const SubResult &sub : result.subResults) {
        if ((sub.id == QLatin1StringView("native2d") || sub.id == QLatin1StringView("fill2d")) && sub.status != Status::Ok)
            return std::nullopt;
    }
    return std::round(kScale * std::sqrt((*native / kExact) * (*fill / kExact)));
}

} // namespace sb::gpu::formula_v1

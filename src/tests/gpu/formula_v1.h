// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU score, formula v1 (Phase 4a design §11): the weighted geometric
// mean of metric / anchor, built with weightedGeometricScore() as the RAM
// score is. Task 15 had the 3D scenes group only; task 28 adds native 2D; task 32
// adds fill-rate (all three groups). PROVISIONAL anchors (versions 0), until the
// calibration on Imagionationmake (task 36) freezes them.
//
//   scenes3d   W 0.80   scenes3d.geometry.fps 0.20, .drawcalls.fps 0.20,
//                       .shading.fps 0.30, .particles.fps 0.30 (Dave, D3)
//   native2d   W 0.10   native2d.shapes, .text, .blits, .gradients 0.25 each (Qt raster, D8)
//   fill2d     W 0.10   fill2d.quads (Gpx/s) 0.50, fill2d.sprites (MOps/s) 0.50 (the best API's)
//   GPU score  round(1000 x exp(sum W.ln G)); sub-scores round(1000 x G);
//              a total only for an Ok result; a group whose sub-test isn't Ok
//              has no sub-score.

#include "core/result.h"
#include "core/scoring.h"

#include <optional>

namespace sb::gpu::formula_v1 {

inline constexpr double kScale = 1000.0;

const QVector<GeometricGroup> &groups();
ScoreBreakdown score(const TestResult &result);
// The combined 2D score (P21): round(1000 x sqrt(G_native x G_fill)); nullopt unless both groups are complete and
// their sub-results Ok (the result's own status doesn't matter: a Partial keeps it).
std::optional<double> score2d(const TestResult &result);

} // namespace sb::gpu::formula_v1

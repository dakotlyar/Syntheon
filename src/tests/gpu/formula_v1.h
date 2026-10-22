// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU score, formula v1 (Phase 4a design §11): the weighted geometric
// mean of metric / anchor, built with weightedGeometricScore() as the RAM
// score is. Task 15 had the 3D scenes group only; task 28 adds native 2D; the
// fill-rate group comes with task 32. PROVISIONAL anchors (versions 0): 100 fps
// each, until the sizing run (task 17) and the calibration on Imagionationmake
// (task 36) freeze them.
//
//   scenes3d   W 0.80   scenes3d.geometry.fps 0.20, .drawcalls.fps 0.20,
//                       .shading.fps 0.30, .particles.fps 0.30 (Dave, D3)
//   native2d   W 0.10   native2d.shapes, .text, .blits, .gradients 0.25 each (Qt raster, D8)
//   GPU score  round(1000 x exp(sum W.ln G)); sub-scores round(1000 x G);
//              a total only for an Ok result; a group whose sub-test isn't Ok
//              has no sub-score.

#include "core/result.h"
#include "core/scoring.h"

namespace sb::gpu::formula_v1 {

inline constexpr double kScale = 1000.0;

const QVector<GeometricGroup> &groups();
ScoreBreakdown score(const TestResult &result);

} // namespace sb::gpu::formula_v1

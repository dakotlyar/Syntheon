// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The test window's size, DPI scale and the fit rule (Phase 4a design §3.1,
// §3.2, D2). Integer arithmetic only, so the table in the design is exact. Pure.

#include <QString>

namespace sb::gpu {

struct Size {
    int width = 0, height = 0;
    bool operator==(const Size &) const = default;
};

inline constexpr Size kLogical{1920, 1080};
inline constexpr int kMarginX = 64, kMarginY = 96;

// The smallest window the test runs in. Below it the screen is "too small for
// the test window": the GPU test reports N/A with that reason (task 9+).
inline constexpr Size kMinWindow{640, 360};

struct WindowPlan {
    Size windowLogical, windowPhysical, render;
    bool fit = false;
    bool tooSmall = false;  // the fitted window would be smaller than kMinWindow
};

// availableLogical = the screen's available geometry (taskbar excluded), in
// logical pixels. render = lround(1920 x s) by lround(1080 x s), always.
WindowPlan planWindow(Size availableLogical, double scale);
QString sizeText(Size s);        // "1920×1080" (U+00D7)
int scalePercent(double scale);  // lround(scale x 100)

} // namespace sb::gpu

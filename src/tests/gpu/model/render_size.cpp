// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/render_size.h"

#include <cmath>

namespace sb::gpu {

namespace {
Size physical(Size logical, double scale)
{
    return {int(std::lround(logical.width * scale)), int(std::lround(logical.height * scale))};
}
} // namespace

WindowPlan planWindow(Size availableLogical, double scale)
{
    WindowPlan plan;
    const int aw = availableLogical.width - kMarginX;
    const int ah = availableLogical.height - kMarginY;
    if (aw >= kLogical.width && ah >= kLogical.height) {
        plan.windowLogical = kLogical;
        plan.fit = false;
    } else if (aw * 9 < ah * 16) {
        plan.windowLogical = {aw, aw * 9 / 16};  // width-limited
        plan.fit = true;
    } else {
        plan.windowLogical = {ah * 16 / 9, ah};  // height-limited
        plan.fit = true;
    }
    plan.windowPhysical = physical(plan.windowLogical, scale);
    plan.render = physical(kLogical, scale);
    return plan;
}

QString sizeText(Size s)
{
    return QStringLiteral("%1×%2").arg(s.width).arg(s.height);
}

int scalePercent(double scale) { return int(std::lround(scale * 100.0)); }

} // namespace sb::gpu

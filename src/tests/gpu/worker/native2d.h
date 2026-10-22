// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The native 2D paths' interface (Phase 4a design §7.1, task 25): Qt raster (task 26, every OS) and
// Direct2D (task 27, Windows). Like IBackend it never makes content decisions: it draws the ops it is
// given, every frame, under the global translation of native_ops.h (nativeTranslation(t)). A native
// path can also be an IBackend (the window flow init / resume / suspend, the worker's present target
// and overlay are the same): then the one object implements both interfaces, and the worker drives
// it through IBackend for the window and through this interface for the workload.

#include "tests/gpu/worker/backend.h"
#include "tests/gpu/worker/scene/native_ops.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sb::gpu {

class INative2D
{
public:
    virtual ~INative2D() = default;
    virtual std::string init(const BackendInit &, const PresentTarget &, BackendInfo *) = 0;
    virtual std::string load(const NativeOps &ops) = 0;
    // Draws every op with the §7.1 translation for time t, presents (the window, or nothing headless).
    virtual std::string frame(double t) = 0;
    // The frame at time t, RGBA8 at render size, top row first, before the overlay (the verification frame).
    virtual std::string readback(double t, std::vector<std::uint8_t> *rgba8) = 0;
    virtual void unload() = 0;
    // Waits until every frame the path has queued has finished (the 3D backends' drain): called before the measured
    // window's end time is read, so queued work is inside it. A CPU path has nothing queued.
    virtual std::string waitIdle() { return {}; }
    // The overlay text (§5.9) drawn after the scaled blit; "" = none.
    virtual void setOverlay(const std::string &text) = 0;
    // The path's name in the overlay and the window title: "Qt raster", "Direct2D".
    virtual const char *overlayName() const = 0;
    // A warning for the result (a fallback the path made, e.g. a missing bundled font); "" = none. The worker
    // sends it as a `warning` event after `ready`; it never fails the run.
    virtual std::string warning() const { return {}; }
};

} // namespace sb::gpu

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the worker's Qt side gives a backend (Phase 4a design §5.2, P2).
// Task 9: the headless and raster parts. The Vulkan part (task 10), OpenGL
// (task 21) and the HWND (task 18) are added beside them.

#include "tests/gpu/worker/backend.h"

class WorkerWindow;

namespace sb::gpu::glue {

// Dev hook --headless: no window, no swapchain.
PresentTarget headlessTarget();
// For init() when there is a window (task 9 call order, backend.h): the window
// isn't shown yet, so the target carries no surface, HWND or swap; a backend
// creates its device and pipelines only. Tasks 10/21 fill in their API's
// part (the Vulkan instance extensions, a current GL context's entry points).
PresentTarget windowlessTarget(Api api);
// A shown raster window (the null backend; Qt raster in task 26): its
// physical size only.
PresentTarget rasterTarget(const WorkerWindow &window);

} // namespace sb::gpu::glue

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the worker's Qt side gives a backend (Phase 4a design §5.2, P2).
// Task 9: the headless and raster parts; task 10: Vulkan. OpenGL (task 21)
// and the HWND (task 18) are added beside them.

#include "tests/gpu/worker/backend.h"

class WorkerWindow;

namespace sb::gpu::glue {

// Dev hook --headless: no window, no swapchain.
PresentTarget headlessTarget();
// For init() when there is a window (task 9 call order, backend.h): the window
// isn't shown yet, so the target carries no surface, HWND or swap; a backend
// creates its device and pipelines only. Vulkan: the window system's instance
// extensions (task 10); task 21: a current GL context's entry points.
PresentTarget windowlessTarget(Api api);
// A shown raster window (the null backend; Qt raster in task 26): its
// physical size only.
PresentTarget rasterTarget(const WorkerWindow &window);
// A shown Vulkan window (task 10): its physical size and scale, the instance
// extensions, and makeVulkanSurface, which adopts the backend's VkInstance
// into a QVulkanInstance the window owns (setVkInstance, create) and returns
// QVulkanInstance::surfaceForWindow(&window). The surface belongs to the
// window: Qt destroys it with the platform window (on Wayland already at hide).
PresentTarget vulkanTarget(WorkerWindow &window);
// A shown Direct3D window (task 18; DX11 and D2D too): its physical size and
// scale and its native handle (HWND, from winId()).
PresentTarget nativeWindowTarget(WorkerWindow &window);
// "VK_KHR_surface" + "VK_KHR_win32_surface" / "VK_KHR_xcb_surface" /
// "VK_KHR_wayland_surface" by QGuiApplication::platformName(); none otherwise
// (offscreen).
std::vector<std::string> vulkanSurfaceExtensions();

} // namespace sb::gpu::glue

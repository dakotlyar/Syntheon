// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the worker's Qt side gives a backend (Phase 4a design §5.2, P2).
// Task 9: the headless and raster parts; task 10: Vulkan. OpenGL (task 21)
// and the HWND (task 18) are added beside them.

#include "tests/gpu/worker/backend.h"

#include <QSurfaceFormat>

#include <memory>

class QOffscreenSurface;
class QOpenGLContext;
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

// The worker's one OpenGL context (task 21, §4.3): 3.3 core, swap interval 0,
// a debug context under --validation. It is current on an offscreen surface
// for init(), between workloads and at the end, and on the test window from
// resume() to suspend() (GL-2: the main thread only). Linux/Windows: GLX, EGL
// or WGL through Qt; the backend loads it with glad and never sees Qt.
// What a context says about itself (the GL probe, task 23).
struct GlInfo {
    std::string version, renderer, vendor;  // glGetString; version as the driver gives it ("4.6.0 NVIDIA 550.1")
};

class GlContext
{
public:
    GlContext();
    ~GlContext();
    // The format of the context, the offscreen surface and the test window
    // (set on the window before it is first shown).
    static QSurfaceFormat format(bool debug);
    // Creates the context and makes it current offscreen. "" or the reason:
    // "N/A: Needs OpenGL 3.3 (the driver has X.Y)" when no 3.3 core context exists.
    std::string create(bool debug);
    // Current offscreen: glGetProcAddress only (init(), or the headless runs).
    PresentTarget offscreenTarget(bool headless);
    // Current on the shown window: its size and scale, swap, default
    // framebuffer and the swap interval Qt reports.
    PresentTarget windowTarget(WorkerWindow &window);
    void makeOffscreenCurrent();
    // The made context's strings (create() succeeded), or a default context's (what the driver gives when
    // no 3.3 core context exists: the version for "Needs OpenGL 3.3 (the driver has 1.1)", the renderer
    // for a GDI Generic check); empty strings when there is no context at all.
    GlInfo info();
    static GlInfo defaultInfo();

private:
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOffscreenSurface> m_surface;
};

} // namespace sb::gpu::glue

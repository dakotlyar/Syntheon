// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU worker's backend interface (Phase 4a design §5.2, P2). Qt-free:
// the worker's Qt glue (qt_glue.h) gives a backend its window as a
// PresentTarget, and every backend uploads what SceneData says and draws
// what it says, nothing else.
//
// Call order (task 9, binding for every backend; Implementation notes from task 9):
//   init(init, target)          once. target is headless (dev hook), or has
//                               NO window yet: create the device, queues and
//                               pipelines, never a swapchain here.
//   per workload:  resume(windowTarget) -> load -> frame... -> drain ->
//                  readback -> unload -> suspend
//   resume() gets the shown window (headless: the headless target again) and
//   makes the swapchain; suspend() releases it. unload() is also called after
//   a failed load() or frame().
//   Vulkan (task 10): the window's VkSurfaceKHR belongs to the window (Qt's
//   QVulkanInstance makes and destroys it); the worker destroys the window's
//   platform window before the backend, so the surface goes before the instance.

#include "tests/gpu/worker/scene/scenes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sb::gpu {
enum class Api : std::uint8_t { Vulkan, Dx12, Dx11, OpenGl, Null };

// What the Qt glue gives a backend (P2). Exactly one of the members is used.
struct PresentTarget {
    bool headless = false;                       // dev hook: no window, no swapchain
    void *hwnd = nullptr;                        // D3D12, D3D11 (HWND)
    // Vulkan: the instance extensions the window system needs, and a call
    // that makes the VkSurfaceKHR for an instance (QVulkanInstance glue).
    std::vector<std::string> vulkanInstanceExtensions;
    std::function<std::uint64_t(void *vkInstance)> makeVulkanSurface;  // returns VkSurfaceKHR, 0 on failure
    // OpenGL: the current context's entry points and swap (QOpenGLContext glue).
    std::function<void *(const char *)> glGetProcAddress;
    std::function<void()> glSwapBuffers;
    std::uint32_t glDefaultFramebuffer = 0;
    int glSwapInterval = -1;  // task 21: the interval Qt reports for the window's context (§5.5; the result's "present")
    std::uint32_t windowWidth = 0, windowHeight = 0;   // physical px (0 if headless)
    // Task 10: the window's devicePixelRatio, for the overlay's size (§5.9); 1 when headless.
    double scale = 1.0;
};

struct BackendInit {
    std::string gpuId;
    std::uint32_t vendorId = 0, deviceId = 0;
    std::string pciSlot;                 // Linux
    std::array<std::uint8_t, 8> luid{};  // Windows
    bool hasLuid = false;
    std::uint32_t renderWidth = 1920, renderHeight = 1080;  // physical
    bool allowSoftware = false, validation = false;
};

struct BackendInfo {
    std::string deviceName, apiVersion, driver, presentMode, note, layers;
    std::uint32_t vendorId = 0, deviceId = 0;
    bool gpuTimestamps = false;
};

// Every call returns "" on success, or a reason. A reason starting with
// "N/A: " means the API isn't available (exit 5); "LOST: " means the device
// was lost (Error); anything else is an Error.
class IBackend
{
public:
    virtual ~IBackend() = default;
    virtual Api api() const = 0;
    virtual std::string init(const BackendInit &init, const PresentTarget &target, BackendInfo *info) = 0;
    virtual std::string load(const SceneData &scene) = 0;   // uploads; replaces the previous workload
    // One frame of the loaded workload at animation time t (seconds):
    // record, submit, present pass, present. Appends to gpuMs the GPU time
    // of every earlier frame whose timestamps became available (P8).
    virtual std::string frame(double t, std::vector<double> *gpuMs) = 0;
    virtual std::string drain(std::vector<double> *gpuMs) = 0;  // waits for the GPU, collects the rest
    // The verification frame (P14): draws t into the render target, reads it
    // back (after tone map / post, before the present pass) as RGBA8, top row first.
    virtual std::string readback(double t, std::vector<std::uint8_t> *rgba8) = 0;
    virtual void unload() = 0;
    // Between serve commands (D12, §2.5): release the swapchain/surface and
    // hide nothing else (the window is the glue's job); keep the device,
    // pipelines and uploaded shared content. resume() recreates the
    // swapchain for the re-shown window. Both "" on success.
    virtual std::string suspend() = 0;
    virtual std::string resume(const PresentTarget &target) = 0;
    // The overlay text for the present pass (§5.9): ASCII 32–126, at most 40
    // characters, "" = no overlay. Takes effect from the next frame().
    virtual void setOverlay(const std::string &text) = 0;
    // Draw calls issued by the last frame() before its present pass (task 11+:
    // must equal SceneData::drawsPerFrame; a self-check, not a metric).
    virtual std::uint32_t lastFrameDraws() const = 0;
    // The present mode the last resume() chose ("IMMEDIATE", "MAILBOX"), for
    // the result's optional "present" key (Antilles' decision after task 9);
    // "" when there is none (headless, or an API that doesn't choose one).
    virtual std::string presentMode() const { return {}; }
    // The render size for the next load(): the window can land on a screen with another
    // scale than the one init() planned for (D1), and the render size follows the scale.
    // Only called between workloads (after unload(), before resume()/load()).
    virtual void setRenderSize(std::uint32_t, std::uint32_t) {}
};

std::unique_ptr<IBackend> makeNullBackend();      // dev hook and tests
std::unique_ptr<IBackend> makeVulkanBackend();
std::unique_ptr<IBackend> makeOpenGlBackend();
#if defined(_WIN32)
std::unique_ptr<IBackend> makeD3D12Backend();
std::unique_ptr<IBackend> makeD3D11Backend();
#endif
}

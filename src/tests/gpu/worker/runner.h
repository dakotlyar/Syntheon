// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One workload on one backend: load, warm-up, the measured window, the
// verification frame, unload (Phase 4a design §5.8, P6, P7, P14). Qt-free.

#include "tests/gpu/worker/backend.h"
#include "tests/gpu/worker/scene/scenes.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sb::gpu {

// WorkloadResult::error for a stop (stdin "stop", Esc or closing the window).
inline constexpr const char *kStopped = "stopped";
// WorkloadResult::error when Hooks::windowOk failed during the measurement;
// the worker replaces it with the window's own reason (WorkerWindow::problem).
inline constexpr const char *kWindowLost = "the test window was hidden or minimised during the measurement";

// The overlay's names (§5.9): "Geometry" ... "Sprites"; "Vulkan" "DX12" "DX11" "OpenGL" "Null".
const char *workloadName(Workload w);
const char *apiOverlayName(Api a);
// The protocol's API ids: "vulkan" "dx12" "dx11" "opengl" "null".
const char *apiId(Api a);

struct WorkloadPlan { Workload workload; int warmupMs = 0; int measureMs = 0; };
struct WorkloadResult {
    Workload workload;
    std::string error;                    // "" = ok
    int frames = 0;
    double seconds = 0;
    std::vector<double> frameMs;          // every measured interval
    std::vector<double> gpuMs, cpuMs;     // per measured frame where known
    std::vector<std::uint8_t> signature;  // 432 bytes, or empty
};
class Runner {
public:
    using Clock = std::chrono::steady_clock;
    struct Hooks {
        std::function<bool()> stopRequested;     // stdin "stop" or Esc
        std::function<bool()> windowOk;          // exposed and unchanged (§3.3)
        std::function<void()> pumpEvents;        // processEvents()
        std::function<void(Workload, const char *phase, int seconds)> stage;
    };
    // Task 9: what the declaration in §5.8 left out. The runner reduces the
    // readback at the render size (the backend's BackendInit), and --no-overlay
    // (dev hook) turns the overlay off.
    struct Settings {
        std::uint32_t renderWidth = 1920, renderHeight = 1080;
        bool overlay = true;
    };
    Runner(IBackend &backend, Hooks hooks);
    Runner(IBackend &backend, Hooks hooks, Settings settings);
    WorkloadResult run(const WorkloadPlan &plan, const SceneData &scene);
    // The readback's size after the window moved to a screen with another scale (D1).
    void setRenderSize(std::uint32_t w, std::uint32_t h) { m_settings.renderWidth = w, m_settings.renderHeight = h; }

private:
    IBackend &m_backend;
    Hooks m_hooks;
    Settings m_settings;
};

} // namespace sb::gpu

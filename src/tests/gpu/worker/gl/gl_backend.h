// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The OpenGL 3.3 core backend (Phase 4a design §4.3, §5.3-§5.6, task 21):
// task 10's structure on GL. The context is the glue's (qt_glue.h: a
// QOpenGLContext made current before every call, on the worker's main thread
// only, GL-2); this backend loads it with glad through the target's
// glGetProcAddress and never touches Qt. Two frames in flight on fences
// (GL-5), GL_TIMESTAMP queries, the present pass into the default framebuffer
// with flipV = 1 (GL-1). Task 21 draws the clear-only workload (ldrA cleared
// to (64, 128, 192, 255)); task 22 adds the scenes (gl_workloads.cpp).
//
// GL's render targets are bottom-up: every pass that samples one flips v
// (flipV in Present and in the Post blocks, task 19's note), and readback
// flips the rows, so readback() is top row first like every other API.

#include "tests/gpu/worker/backend.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sb::gpu::gl {

class GlBackend final : public IBackend
{
public:
    GlBackend();
    ~GlBackend() override;
    GlBackend(const GlBackend &) = delete;
    GlBackend &operator=(const GlBackend &) = delete;

    Api api() const override { return Api::OpenGl; }
    // target: the glue's current context (glGetProcAddress), headless or not.
    std::string init(const BackendInit &init, const PresentTarget &target, BackendInfo *info) override;
    std::string load(const SceneData &scene) override;
    std::string frame(double t, std::vector<double> *gpuMs) override;
    std::string drain(std::vector<double> *gpuMs) override;
    std::string readback(double t, std::vector<std::uint8_t> *rgba8) override;
    void unload() override;
    // Revised (Dave): glFinish; the glue hides the window and keeps the context.
    std::string suspend() override;
    // The re-shown window: the glue made the context current on it; its swap, default
    // framebuffer and swap interval come in the target.
    std::string resume(const PresentTarget &target) override;
    void setOverlay(const std::string &text) override { m_overlay = text; }
    std::uint32_t lastFrameDraws() const override { return m_lastDraws; }
    std::string presentMode() const override { return m_presentModeName; }
    void setRenderSize(std::uint32_t w, std::uint32_t h) override { m_init.renderWidth = w, m_init.renderHeight = h; }

    // Everything released (the context must be current); the validation counters stay readable.
    void shutdown();
    int validationErrors() const { return m_validationErrors.load(); }
    std::string firstValidationError() const { return m_firstValidationError; }
#if defined(SYNBENCH_DEV_HOOKS)
    // Dev hook, headless only (task 21's overlay test, as Vulkan's): the last
    // frame's present-pass output (render size, RGBA8, top row first), after drain().
    std::string readbackPresented(std::vector<std::uint8_t> *rgba8);
#endif

    // The KHR_debug callback's sink (an ERROR-type message is a validation error).
    void onDebugMessage(std::uint32_t type, const char *message);

private:
    struct Target { std::uint32_t texture = 0, fbo = 0, width = 0, height = 0; };
    struct Slot { void *fence = nullptr; std::array<std::uint32_t, 2> queries{}; std::uint32_t frameConstants = 0; bool submitted = false; };
    // A linked program and, per texture unit 0-5, the sampler object index
    // (0 s0, 1 s1, 2 s2) its combined sampler names, or -1 (none / Load).
    struct Program { std::uint32_t id = 0; std::array<int, 6> unitSampler{-1, -1, -1, -1, -1, -1}; };

    std::string makeProgram(const char *shader, const char *vs, const char *fs, Program *out);
    std::string makeTarget(Target &t, std::uint32_t w, std::uint32_t h);
    void destroyTarget(Target &t);
    void useProgram(const Program &p);
    std::string waitSlot(std::uint32_t slot, std::vector<double> *gpuMs);  // gpuMs null: drop its timestamps
    std::string waitAll();
    void recordWorkload(Slot &slot, double t);  // leaves the output (ldrA) drawn
    void presentPass(Slot &slot, std::uint32_t fbo, std::uint32_t w, std::uint32_t h);
    std::string readTarget(const Target &t, std::vector<std::uint8_t> *rgba8);
    std::string validationProblem();

    BackendInit m_init;
    bool m_headless = false;
    std::array<Slot, 2> m_slots;
    std::uint64_t m_frame = 0;
    std::array<std::uint32_t, 3> m_samplers{};
    std::uint32_t m_vao = 0, m_atlas = 0;
    Program m_present;
    Target m_ldrA, m_headlessTarget;
    std::vector<std::uint8_t> m_constants;  // the CPU copy of a slot's frame constants, uploaded per frame
    bool m_timestamps = false;
    std::function<void()> m_swap;
    std::uint32_t m_defaultFramebuffer = 0, m_windowW = 0, m_windowH = 0;
    double m_windowScale = 1.0;
    std::uint32_t m_lastDraws = 0;
    std::string m_overlay, m_presentModeName;
    bool m_loaded = false, m_resumed = false, m_inited = false;
    std::atomic<int> m_validationErrors{0};
    std::string m_firstValidationError;
};

} // namespace sb::gpu::gl

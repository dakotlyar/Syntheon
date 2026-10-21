// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The null backend (Phase 4a task 9, dev hook --api null and the tests): no
// GPU at all. A frame sleeps 1 ms; the GPU time of the frame two back is 0.5 ms;
// the readback is a gradient at the render size.

#include "tests/gpu/worker/backend.h"

namespace sb::gpu {

class NullBackend final : public IBackend
{
public:
    Api api() const override { return Api::Null; }
    std::string init(const BackendInit &init, const PresentTarget &target, BackendInfo *info) override;
    std::string load(const SceneData &scene) override;
    std::string frame(double t, std::vector<double> *gpuMs) override;
    std::string drain(std::vector<double> *gpuMs) override;
    std::string readback(double t, std::vector<std::uint8_t> *rgba8) override;
    void unload() override;
    std::string suspend() override;
    std::string resume(const PresentTarget &target) override;
    void setOverlay(const std::string &text) override { m_overlay = text; }
    std::uint32_t lastFrameDraws() const override { return m_draws; }

#if defined(SYNBENCH_DEV_HOOKS)
    int suspendCount() const { return m_suspends; }
    int resumeCount() const { return m_resumes; }
#endif

private:
    std::uint32_t m_width = 0, m_height = 0;
    std::uint32_t m_draws = 0, m_loadedDraws = 0;
    std::uint64_t m_frames = 0, m_reported = 0;  // GPU times handed out so far
    std::string m_overlay;
    int m_suspends = 0, m_resumes = 0;
};

} // namespace sb::gpu

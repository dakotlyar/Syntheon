// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/null/null_backend.h"

#include <chrono>
#include <thread>

namespace sb::gpu {

namespace {
constexpr double kNullGpuMs = 0.5;
}

std::string NullBackend::init(const BackendInit &init, const PresentTarget &, BackendInfo *info)
{
    m_width = init.renderWidth;
    m_height = init.renderHeight;
    if (info) {
        info->deviceName = "Null";
        info->apiVersion = "Null 1.0";
        info->presentMode = "none";
        info->gpuTimestamps = true;
    }
    return {};
}

std::string NullBackend::load(const SceneData &scene)
{
    m_loadedDraws = scene.drawsPerFrame;
    m_draws = 0;
    m_frames = m_reported = 0;
    return {};
}

std::string NullBackend::frame(double, std::vector<double> *gpuMs)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++m_frames;
    m_draws = m_loadedDraws;
    // Frame n's timestamps are read two frames later (P8).
    while (m_reported + 2 < m_frames) {
        if (gpuMs)
            gpuMs->push_back(kNullGpuMs);
        ++m_reported;
    }
    return {};
}

std::string NullBackend::drain(std::vector<double> *gpuMs)
{
    for (; m_reported < m_frames; ++m_reported) {
        if (gpuMs)
            gpuMs->push_back(kNullGpuMs);
    }
    return {};
}

std::string NullBackend::readback(double, std::vector<std::uint8_t> *rgba8)
{
    // A horizontal red and vertical green gradient, blue 128.
    rgba8->assign(std::size_t(m_width) * m_height * 4, 0);
    std::uint8_t *p = rgba8->data();
    for (std::uint32_t y = 0; y < m_height; ++y) {
        for (std::uint32_t x = 0; x < m_width; ++x, p += 4) {
            p[0] = std::uint8_t(m_width > 1 ? x * 255 / (m_width - 1) : 0);
            p[1] = std::uint8_t(m_height > 1 ? y * 255 / (m_height - 1) : 0);
            p[2] = 128;
            p[3] = 255;
        }
    }
    return {};
}

void NullBackend::unload()
{
    m_loadedDraws = m_draws = 0;
}

std::string NullBackend::suspend()
{
    ++m_suspends;
    return {};
}

std::string NullBackend::resume(const PresentTarget &)
{
    ++m_resumes;
    return {};
}

std::unique_ptr<IBackend> makeNullBackend()
{
    return std::make_unique<NullBackend>();
}

} // namespace sb::gpu

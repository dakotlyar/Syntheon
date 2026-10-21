// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/qt_glue.h"

#include "tests/gpu/worker/worker_window.h"

#include <QGuiApplication>
#include <QVulkanInstance>

#include <cstring>
#include <memory>

namespace sb::gpu::glue {

PresentTarget headlessTarget()
{
    PresentTarget t;
    t.headless = true;
    return t;
}

std::vector<std::string> vulkanSurfaceExtensions()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1StringView("windows"))
        return {"VK_KHR_surface", "VK_KHR_win32_surface"};
    if (platform == QLatin1StringView("xcb"))
        return {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    if (platform.startsWith(QLatin1StringView("wayland")))
        return {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    return {};
}

PresentTarget windowlessTarget(Api api)
{
    PresentTarget t;
    if (api == Api::Vulkan)
        t.vulkanInstanceExtensions = vulkanSurfaceExtensions();
    return t;
}

PresentTarget rasterTarget(const WorkerWindow &window)
{
    PresentTarget t;
    const QSize size = window.physicalSize();
    t.windowWidth = std::uint32_t(size.width());
    t.windowHeight = std::uint32_t(size.height());
    t.scale = window.devicePixelRatio();
    return t;
}

PresentTarget vulkanTarget(WorkerWindow &window)
{
    PresentTarget t = rasterTarget(window);
    t.vulkanInstanceExtensions = vulkanSurfaceExtensions();
    t.makeVulkanSurface = [&window](void *vkInstance) -> std::uint64_t {
        const auto instance = static_cast<VkInstance>(vkInstance);
        if (!window.vulkanInstance() || window.vulkanInstance()->vkInstance() != instance) {
            auto adopted = std::make_unique<QVulkanInstance>();
            adopted->setVkInstance(instance);  // not owned: the backend destroys it
            if (!adopted->create())
                return 0;
            window.setVulkanInstance(adopted.get());
            window.keepVulkanInstance(std::move(adopted));
        }
        const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(&window);
        std::uint64_t out = 0;
        static_assert(sizeof surface == sizeof out, "VkSurfaceKHR is a 64-bit handle");
        std::memcpy(&out, &surface, sizeof out);
        return out;
    };
    return t;
}

} // namespace sb::gpu::glue

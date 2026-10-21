// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Vulkan backend's instance, device and queue (Phase 4a design §4.1, §4.2,
// §5.6, task 10). Qt-free: the window system's instance extensions and the
// surface come from the PresentTarget (qt_glue).

#include "tests/gpu/worker/backend.h"

#include <volk.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace sb::gpu::vk {

// "VK_ERROR_DEVICE_LOST", or the number for codes not listed.
std::string resultName(VkResult r);
// Synchronisation validation only on layers new enough not to misclassify a sampled image's usage
// as a storage read (Ubuntu 22.04's layer, spec 1.3.204, does, and reports a hazard that isn't
// there): specVersion (the Vulkan header the layer was built from) >= 1.3.250. Core validation
// is always on. Pure.
inline constexpr std::uint32_t kSyncValidationMinSpec = VK_MAKE_API_VERSION(0, 1, 3, 250);
inline bool layerSupportsSyncValidation(std::uint32_t specVersion)
{
    return specVersion >= kSyncValidationMinSpec;
}

// A failed call as a backend reason: "LOST: the GPU driver reset (VK_ERROR_DEVICE_LOST)"
// for a lost device, else "<what> failed (VK_ERROR_...)".
std::string failure(const char *what, VkResult r);

class Context
{
public:
    // volkInitialize (Windows: volkInitializeCustom with System32's vulkan-1.dll), instance 1.3 with
    // target.vulkanInstanceExtensions (+ VK_EXT_debug_utils and the validation layer if validation),
    // device by id (§4.1: vendor/device + VK_EXT_pci_bus_info slot or LUID), Vulkan 1.3 features
    // dynamicRendering + synchronization2 required, samplerAnisotropy; one graphics queue that can present.
    std::string create(const BackendInit &init, const PresentTarget &target, bool validation, BackendInfo *info);
    void destroy();
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice physical = VK_NULL_HANDLE; VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE; std::uint32_t queueFamily = 0; VkSurfaceKHR surface = VK_NULL_HANDLE;
    double timestampPeriodNs = 0; bool timestamps = false;
    std::atomic<int> validationErrors{0}; std::string firstValidationError;   // from the debug messenger

    // Beyond the declaration in task 10:
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t timestampValidBits = 0;
    bool anisotropy = false;   // samplerAnisotropy is enabled
    bool swapchain = false;    // VK_KHR_swapchain is enabled (not headless)
    VkCommandPool uploadPool = VK_NULL_HANDLE;  // one-time submits (uploads, readbacks)
    std::mutex validationMutex;  // guards firstValidationError

    // "validation: <the first error>" once the validation layer reported an error, else "".
    std::string validationProblem();
    // The memory type for typeBits with props; UINT32_MAX if there is none.
    std::uint32_t memoryType(std::uint32_t typeBits, VkMemoryPropertyFlags props) const;
    // Records with record(), submits, waits (1 s timeouts, at most kWaitLimitS s).
    std::string oneTimeSubmit(const std::function<void(VkCommandBuffer)> &record);
    // vkWaitForFences in 1 s steps (P-STOP: never UINT64_MAX); after kWaitLimitS
    // seconds "LOST: the GPU stopped responding".
    std::string waitFence(VkFence fence);

    static constexpr int kWaitLimitS = 10;

private:
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    static VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT types,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *data, void *self);
};

// The platform's loader into volk: Linux volkInitialize(); Windows
// System32's vulkan-1.dll (vk_loader.h). "" or "N/A: No Vulkan driver is installed".
std::string loadVulkanLoader();

} // namespace sb::gpu::vk

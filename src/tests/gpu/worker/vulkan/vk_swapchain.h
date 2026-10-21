// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Vulkan swapchain of the test window (Phase 4a design §5.3, §5.4, §5.5,
// task 10): 3 images, BGRA8_UNORM (else RGBA8_UNORM), vsync off; one
// render-finished semaphore per swapchain image (pitfall VK-1).

#include "tests/gpu/worker/vulkan/vk_context.h"

#include <string>
#include <vector>

namespace sb::gpu::vk {

// IMMEDIATE > MAILBOX > "N/A: Vsync can't be turned off (only FIFO present mode)"
std::string choosePresentMode(const std::vector<VkPresentModeKHR> &available, VkPresentModeKHR *out);
const char *presentModeName(VkPresentModeKHR);   // "IMMEDIATE" "MAILBOX"

// The §3.2 reason for a swapchain that no longer fits its window.
inline constexpr const char *kWindowChanged = "the test window was resized or moved to another screen during the measurement";

class Swapchain {
public:
    std::string create(Context &ctx, std::uint32_t width, std::uint32_t height);  // 3 images, BGRA8_UNORM else RGBA8_UNORM
    std::string acquire(Context &ctx, VkSemaphore signal, std::uint32_t *index);   // OUT_OF_DATE -> "the test window was resized ..."
    std::string present(Context &ctx, VkSemaphore wait, std::uint32_t index);
    void destroy(Context &ctx);
    VkSwapchainKHR swapchain = VK_NULL_HANDLE; VkFormat format = VK_FORMAT_UNDEFINED; VkPresentModeKHR mode{};
    std::uint32_t width = 0, height = 0; std::vector<VkImage> images; std::vector<VkImageView> views;
    std::vector<VkSemaphore> renderFinished;   // one per swapchain image
};

} // namespace sb::gpu::vk

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/vulkan/vk_swapchain.h"

#include <algorithm>

namespace sb::gpu::vk {

std::string choosePresentMode(const std::vector<VkPresentModeKHR> &available, VkPresentModeKHR *out)
{
    // §5.5: vsync off; FIFO and FIFO_RELAXED both wait for the display.
    for (const VkPresentModeKHR want : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
        if (std::find(available.begin(), available.end(), want) != available.end()) {
            *out = want;
            return {};
        }
    }
    return "N/A: Vsync can't be turned off (only FIFO present mode)";
}

const char *presentModeName(VkPresentModeKHR m)
{
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "";
    }
}

std::string Swapchain::create(Context &ctx, std::uint32_t w, std::uint32_t h)
{
    VkSurfaceCapabilitiesKHR caps{};
    if (const VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical, ctx.surface, &caps); r != VK_SUCCESS)
        return failure("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);

    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical, ctx.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical, ctx.surface, &n, formats.data());
    formats.resize(n);
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (const VkSurfaceFormatKHR &f : formats) {
            if (chosen.format == VK_FORMAT_UNDEFINED && f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED)
        return "the test window offers no BGRA8 or RGBA8 UNORM swapchain format";

    n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physical, ctx.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physical, ctx.surface, &n, modes.data());
    modes.resize(n);
    VkPresentModeKHR presentMode{};
    if (std::string e = choosePresentMode(modes, &presentMode); !e.empty())
        return e;

    // The surface decides the extent where it can (X11, Windows); otherwise the window's physical size.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return "the test window was hidden or minimised during the measurement";
    std::uint32_t imageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount > 0)
        imageCount = std::min(imageCount, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (const VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & a) {
                alpha = a;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = ctx.surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    if (const VkResult r = vkCreateSwapchainKHR(ctx.device, &ci, nullptr, &swapchain); r != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return failure("vkCreateSwapchainKHR", r);
    }
    format = chosen.format;
    mode = presentMode;
    width = extent.width;
    height = extent.height;

    n = 0;
    vkGetSwapchainImagesKHR(ctx.device, swapchain, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(ctx.device, swapchain, &n, images.data());
    images.resize(n);
    for (VkImage image : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (const VkResult r = vkCreateImageView(ctx.device, &vi, nullptr, &view); r != VK_SUCCESS) {
            destroy(ctx);
            return failure("vkCreateImageView (swapchain)", r);
        }
        views.push_back(view);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        if (const VkResult r = vkCreateSemaphore(ctx.device, &si, nullptr, &s); r != VK_SUCCESS) {
            destroy(ctx);
            return failure("vkCreateSemaphore", r);
        }
        renderFinished.push_back(s);
    }
    return {};
}

std::string Swapchain::acquire(Context &ctx, VkSemaphore signal, std::uint32_t *index)
{
    // 1 s steps, never UINT64_MAX (P-STOP); a window that stays unavailable is a stuck GPU.
    for (int s = 0; s < Context::kWaitLimitS; ++s) {
        const VkResult r = vkAcquireNextImageKHR(ctx.device, swapchain, 1000000000ull, signal, VK_NULL_HANDLE, index);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            return {};
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
            return kWindowChanged;
        if (r != VK_TIMEOUT && r != VK_NOT_READY)
            return failure("vkAcquireNextImageKHR", r);
    }
    return "LOST: the GPU stopped responding";
}

std::string Swapchain::present(Context &ctx, VkSemaphore wait, std::uint32_t index)
{
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &index;
    const VkResult r = vkQueuePresentKHR(ctx.queue, &pi);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
        return {};
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
        return kWindowChanged;
    return failure("vkQueuePresentKHR", r);
}

void Swapchain::destroy(Context &ctx)
{
    for (VkSemaphore s : renderFinished)
        vkDestroySemaphore(ctx.device, s, nullptr);
    for (VkImageView v : views)
        vkDestroyImageView(ctx.device, v, nullptr);
    if (swapchain)
        vkDestroySwapchainKHR(ctx.device, swapchain, nullptr);
    renderFinished.clear();
    views.clear();
    images.clear();
    swapchain = VK_NULL_HANDLE;
    format = VK_FORMAT_UNDEFINED;
    width = height = 0;
}

} // namespace sb::gpu::vk

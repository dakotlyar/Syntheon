// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Images, buffers, uploads and synchronization2 layout transitions for the
// Vulkan backend (Phase 4a task 10).

#include "tests/gpu/worker/vulkan/vk_context.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sb::gpu::vk {

struct Image { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
               VkFormat format = VK_FORMAT_UNDEFINED; std::uint32_t width = 0, height = 0, mips = 1; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };
struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void *mapped = nullptr; VkDeviceSize size = 0; };

std::string makeImage(Context &, std::uint32_t w, std::uint32_t h, VkFormat, VkImageUsageFlags, std::uint32_t mips, Image *out);
std::string makeBuffer(Context &, VkDeviceSize, VkBufferUsageFlags, bool hostVisible, Buffer *out);   // hostVisible -> mapped, coherent
std::string uploadBuffer(Context &, Buffer &dst, const void *data, std::size_t bytes);              // staging + one-time submit + wait
std::string uploadTexture(Context &, const Texture &level0, Image *out);                            // mipChain(), SHADER_READ_ONLY_OPTIMAL
void destroy(Context &, Image &); void destroy(Context &, Buffer &);
void transition(VkCommandBuffer, Image &, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);   // synchronization2, updates img.layout

// The same barrier for an image that isn't an Image (a swapchain image): one
// mip, one layer, colour or depth aspect by format.
void imageBarrier(VkCommandBuffer, VkImage, VkFormat, std::uint32_t mips, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
bool isDepthFormat(VkFormat);

} // namespace sb::gpu::vk

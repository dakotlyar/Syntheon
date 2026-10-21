// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/vulkan/vk_resources.h"

#include "tests/gpu/worker/scene/textures.h"

#include <cstring>
#include <vector>

namespace sb::gpu::vk {

bool isDepthFormat(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT
           || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

std::string makeImage(Context &ctx, std::uint32_t w, std::uint32_t h, VkFormat format, VkImageUsageFlags usage,
                      std::uint32_t mips, Image *out)
{
    Image img;
    img.format = format;
    img.width = w;
    img.height = h;
    img.mips = mips;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (const VkResult r = vkCreateImage(ctx.device, &ci, nullptr, &img.image); r != VK_SUCCESS)
        return failure("vkCreateImage", r);
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = ctx.memoryType(req.memoryTypeBits, 0);
    if (const VkResult r = vkAllocateMemory(ctx.device, &ai, nullptr, &img.memory); r != VK_SUCCESS) {
        destroy(ctx, img);
        return failure("vkAllocateMemory (image)", r);
    }
    vkBindImageMemory(ctx.device, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VkImageAspectFlags(isDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, mips, 0, 1};
    if (const VkResult r = vkCreateImageView(ctx.device, &vi, nullptr, &img.view); r != VK_SUCCESS) {
        destroy(ctx, img);
        return failure("vkCreateImageView", r);
    }
    *out = img;
    return {};
}

std::string makeBuffer(Context &ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer *out)
{
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (const VkResult r = vkCreateBuffer(ctx.device, &ci, nullptr, &b.buffer); r != VK_SUCCESS)
        return failure("vkCreateBuffer", r);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx.device, b.buffer, &req);
    const VkMemoryPropertyFlags props = hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.memoryType(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX && !hostVisible)
        ai.memoryTypeIndex = ctx.memoryType(req.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        destroy(ctx, b);
        return "no host-visible coherent Vulkan memory";
    }
    if (const VkResult r = vkAllocateMemory(ctx.device, &ai, nullptr, &b.memory); r != VK_SUCCESS) {
        destroy(ctx, b);
        return failure("vkAllocateMemory (buffer)", r);
    }
    vkBindBufferMemory(ctx.device, b.buffer, b.memory, 0);
    if (hostVisible) {
        if (const VkResult r = vkMapMemory(ctx.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped); r != VK_SUCCESS) {
            destroy(ctx, b);
            return failure("vkMapMemory", r);
        }
    }
    *out = b;
    return {};
}

std::string uploadBuffer(Context &ctx, Buffer &dst, const void *data, std::size_t bytes)
{
    if (bytes > dst.size)
        return "an upload larger than its buffer";
    if (dst.mapped) {
        std::memcpy(dst.mapped, data, bytes);
        return {};
    }
    Buffer staging;
    if (std::string e = makeBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &staging); !e.empty())
        return e;
    std::memcpy(staging.mapped, data, bytes);
    std::string e = ctx.oneTimeSubmit([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
        // The copy before every later use of the buffer, in any later submission.
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    destroy(ctx, staging);
    return e;
}

std::string uploadTexture(Context &ctx, const Texture &level0, Image *out)
{
    const std::vector<Texture> chain = level0.mips ? mipChain(level0) : std::vector<Texture>{level0};
    Image img;
    if (std::string e = makeImage(ctx, level0.width, level0.height, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, std::uint32_t(chain.size()), &img);
        !e.empty())
        return e;
    VkDeviceSize total = 0;
    for (const Texture &t : chain)
        total += t.rgba8.size();
    Buffer staging;
    if (std::string e = makeBuffer(ctx, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &staging); !e.empty()) {
        destroy(ctx, img);
        return e;
    }
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    for (std::uint32_t level = 0; level < chain.size(); ++level) {
        std::memcpy(static_cast<std::uint8_t *>(staging.mapped) + offset, chain[level].rgba8.data(), chain[level].rgba8.size());
        VkBufferImageCopy r{};
        r.bufferOffset = offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        r.imageExtent = {chain[level].width, chain[level].height, 1};
        regions.push_back(r);
        offset += chain[level].rgba8.size();
    }
    std::string e = ctx.oneTimeSubmit([&](VkCommandBuffer cmd) {
        transition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(regions.size()),
                               regions.data());
        transition(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    destroy(ctx, staging);
    if (!e.empty()) {
        destroy(ctx, img);
        return e;
    }
    *out = img;
    return {};
}

void destroy(Context &ctx, Image &img)
{
    if (img.view)
        vkDestroyImageView(ctx.device, img.view, nullptr);
    if (img.image)
        vkDestroyImage(ctx.device, img.image, nullptr);
    if (img.memory)
        vkFreeMemory(ctx.device, img.memory, nullptr);
    img = Image{};
}

void destroy(Context &ctx, Buffer &b)
{
    if (b.buffer)
        vkDestroyBuffer(ctx.device, b.buffer, nullptr);
    if (b.memory)
        vkFreeMemory(ctx.device, b.memory, nullptr);  // unmaps
    b = Buffer{};
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkFormat format, std::uint32_t mips, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VkImageAspectFlags(isDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, mips, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void transition(VkCommandBuffer cmd, Image &img, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    imageBarrier(cmd, img.image, img.format, img.mips, img.layout, to, srcStage, srcAccess, dstStage, dstAccess);
    img.layout = to;
}

} // namespace sb::gpu::vk

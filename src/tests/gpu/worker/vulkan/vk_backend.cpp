// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/vulkan/vk_backend.h"

#include "tests/gpu/worker/vulkan/vk_workload_state.h"

#include "tests/gpu/worker/present_constants.h"

#include "tests/gpu/worker/scene/bitmap_font.h"
#include "tests/gpu/worker/scene/overlay.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
#include "tests/gpu/worker/shaders/generated/present_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/present_vs.spv.inc"
} // namespace

namespace sb::gpu::vk {

namespace {

// The clear-only workload of task 10 (§5.3 Copy mode): ldrA = (64, 128, 192, 255).
constexpr float kClearColour[4] = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};

std::string makeShader(VkDevice device, const std::uint32_t *code, std::size_t bytes, VkShaderModule *out)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    if (const VkResult r = vkCreateShaderModule(device, &ci, nullptr, out); r != VK_SUCCESS)
        return failure("vkCreateShaderModule", r);
    return {};
}

} // namespace

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend()
{
    shutdown();
}

void VulkanBackend::shutdown()
{
    VkDevice d = m_ctx.device;
    if (d) {
        vkDeviceWaitIdle(d);
        unload();
        releaseWindow();
        for (Slot &s : m_slots) {
            destroy(m_ctx, s.frameConstants);
            destroy(m_ctx, s.objectConstants);
            destroy(m_ctx, s.spriteRing);
            if (s.fence)
                vkDestroyFence(d, s.fence, nullptr);
            if (s.pool)
                vkDestroyCommandPool(d, s.pool, nullptr);
            s = Slot{};
        }
        if (m_queries)
            vkDestroyQueryPool(d, m_queries, nullptr);
        if (m_presentPipeline)
            vkDestroyPipeline(d, m_presentPipeline, nullptr);
        if (m_presentLayout)
            vkDestroyPipelineLayout(d, m_presentLayout, nullptr);
        if (m_pool)
            vkDestroyDescriptorPool(d, m_pool, nullptr);
        for (VkDescriptorSetLayout &l : m_setLayouts) {
            if (l)
                vkDestroyDescriptorSetLayout(d, l, nullptr);
            l = VK_NULL_HANDLE;
        }
        for (VkSampler &s : m_samplers) {
            if (s)
                vkDestroySampler(d, s, nullptr);
            s = VK_NULL_HANDLE;
        }
        destroy(m_ctx, m_atlas);
        destroy(m_ctx, m_dummyDepth);
    }
    m_queries = VK_NULL_HANDLE;
    m_presentPipeline = VK_NULL_HANDLE;
    m_presentLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_ctx.destroy();
}

std::string VulkanBackend::init(const BackendInit &init, const PresentTarget &target, BackendInfo *info)
{
    m_init = init;
    m_headless = target.headless;
    if (std::string e = m_ctx.create(init, target, init.validation, info); !e.empty())
        return e;
    VkDevice d = m_ctx.device;
    if (m_ctx.properties.limits.minUniformBufferOffsetAlignment > kFrameBlock)
        return "uniform alignment > 256";

    // The immutable samplers s0, s1, s2, s3 (§13.1).
    VkSamplerCreateInfo s0{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s0.magFilter = s0.minFilter = VK_FILTER_LINEAR;
    s0.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s0.addressModeU = s0.addressModeV = s0.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s0.anisotropyEnable = m_ctx.anisotropy ? VK_TRUE : VK_FALSE;
    s0.maxAnisotropy = m_ctx.anisotropy ? std::min(8.0f, m_ctx.properties.limits.maxSamplerAnisotropy) : 1.0f;
    s0.maxLod = VK_LOD_CLAMP_NONE;
    VkSamplerCreateInfo s1 = s0;
    s1.addressModeU = s1.addressModeV = s1.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s1.anisotropyEnable = VK_FALSE;
    s1.maxAnisotropy = 1.0f;
    VkSamplerCreateInfo s2 = s1;
    s2.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s2.addressModeU = s2.addressModeV = s2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    s2.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    s2.compareEnable = VK_TRUE;
    s2.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    s2.maxLod = 0.0f;
    // A 2x2 PCF per tap needs linear filtering of D32F (task 12); nearest where the format can't.
    VkFormatProperties depthProps{};
    vkGetPhysicalDeviceFormatProperties(m_ctx.physical, VK_FORMAT_D32_SFLOAT, &depthProps);
    if (!(depthProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        s2.magFilter = s2.minFilter = VK_FILTER_NEAREST;
    // s3: plain bilinear wrap, no mips, no anisotropy (the Quads, §6.5; checkpoint 5 L5).
    VkSamplerCreateInfo s3 = s1;
    s3.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s3.addressModeU = s3.addressModeV = s3.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s3.maxLod = 0.0f;
    const VkSamplerCreateInfo *samplerInfo[4] = {&s0, &s1, &s2, &s3};
    for (int i = 0; i < 4; ++i) {
        if (const VkResult r = vkCreateSampler(d, samplerInfo[i], nullptr, &m_samplers[std::size_t(i)]); r != VK_SUCCESS)
            return failure("vkCreateSampler", r);
    }

    // The shared descriptor set layouts and the pipeline layout (vk_backend.h).
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding set0[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, &m_samplers[0]},
        {5, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, &m_samplers[1]},
        {6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, &m_samplers[2]},
        {7, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, &m_samplers[3]},
    };
    const VkDescriptorSetLayoutBinding set1[] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr}};
    const VkDescriptorSetLayoutBinding set2[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
    };
    const std::pair<const VkDescriptorSetLayoutBinding *, std::uint32_t> sets[3] = {{set0, 8}, {set1, 1}, {set2, 4}};
    for (std::size_t i = 0; i < 3; ++i) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = sets[i].second;
        ci.pBindings = sets[i].first;
        if (const VkResult r = vkCreateDescriptorSetLayout(d, &ci, nullptr, &m_setLayouts[i]); r != VK_SUCCESS)
            return failure("vkCreateDescriptorSetLayout", r);
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 3;
    pli.pSetLayouts = m_setLayouts.data();
    if (const VkResult r = vkCreatePipelineLayout(d, &pli, nullptr, &m_presentLayout); r != VK_SUCCESS)
        return failure("vkCreatePipelineLayout", r);

    // The glyph atlas, once (§5.9).
    if (std::string e = uploadTexture(m_ctx, glyphAtlas(), &m_atlas); !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, 1, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1,
                                  &m_dummyDepth);
        !e.empty())
        return e;

    // Set 0 per slot.
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
                                          {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4},
                                          {VK_DESCRIPTOR_TYPE_SAMPLER, 8}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    if (const VkResult r = vkCreateDescriptorPool(d, &dpi, nullptr, &m_pool); r != VK_SUCCESS)
        return failure("vkCreateDescriptorPool", r);

    for (Slot &s : m_slots) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = m_ctx.queueFamily;
        if (const VkResult r = vkCreateCommandPool(d, &pci, nullptr, &s.pool); r != VK_SUCCESS)
            return failure("vkCreateCommandPool", r);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (const VkResult r = vkAllocateCommandBuffers(d, &ai, &s.cmd); r != VK_SUCCESS)
            return failure("vkAllocateCommandBuffers", r);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (const VkResult r = vkCreateFence(d, &fi, nullptr, &s.fence); r != VK_SUCCESS)
            return failure("vkCreateFence", r);
        if (std::string e = makeBuffer(m_ctx, kFrameConstantsBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &s.frameConstants);
            !e.empty())
            return e;
        std::memset(s.frameConstants.mapped, 0, std::size_t(kFrameConstantsBytes));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = m_pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &m_setLayouts[0];
        if (const VkResult r = vkAllocateDescriptorSets(d, &dai, &s.set0); r != VK_SUCCESS)
            return failure("vkAllocateDescriptorSets", r);
        const VkDescriptorBufferInfo frame{s.frameConstants.buffer, 0, kFrameBlock};
        const VkDescriptorBufferInfo pass{s.frameConstants.buffer, 0, kPassRange};
        const VkDescriptorImageInfo atlas{VK_NULL_HANDLE, m_atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[3]{};
        for (VkWriteDescriptorSet &x : w) {
            x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.dstSet = s.set0;
            x.descriptorCount = 1;
        }
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &frame;
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[1].pBufferInfo = &pass;
        w[2].dstBinding = 3;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[2].pImageInfo = &atlas;
        vkUpdateDescriptorSets(d, 3, w, 0, nullptr);
    }

    if (m_ctx.timestamps) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * std::uint32_t(m_slots.size());
        if (const VkResult r = vkCreateQueryPool(d, &qi, nullptr, &m_queries); r != VK_SUCCESS)
            return failure("vkCreateQueryPool", r);
    }

    // The present pipeline for the likely target format; resume() remakes it
    // if the window's swapchain format differs.
    if (std::string e = createPresentPipeline(m_headless ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM); !e.empty())
        return e;
    return m_ctx.validationProblem();
}

std::string VulkanBackend::createPresentPipeline(VkFormat format)
{
    if (m_presentPipeline) {
        vkDestroyPipeline(m_ctx.device, m_presentPipeline, nullptr);
        m_presentPipeline = VK_NULL_HANDLE;
    }
    std::string e = buildPresentPipeline(format, &m_presentPipeline);
    m_presentFormat = e.empty() ? format : VK_FORMAT_UNDEFINED;
    return e;
}

// The present pass's pipeline for a colour target of `format` (the swapchain's, or RGBA8 for the readback).
std::string VulkanBackend::buildPresentPipeline(VkFormat format, VkPipeline *out)
{
    VkDevice d = m_ctx.device;
    *out = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    std::string e = makeShader(d, kPresentVsSpv, sizeof kPresentVsSpv, &vs);
    if (e.empty())
        e = makeShader(d, kPresentPsSpv, sizeof kPresentPsSpv, &ps);
    if (e.empty()) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "VSMain";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "PSMain";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &ri;
        gp.stageCount = 2;
        gp.pStages = st;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dy;
        gp.layout = m_presentLayout;
        if (const VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, out); r != VK_SUCCESS) {
            *out = VK_NULL_HANDLE;
            e = failure("vkCreateGraphicsPipelines (present)", r);
        }
    }
    if (vs)
        vkDestroyShaderModule(d, vs, nullptr);
    if (ps)
        vkDestroyShaderModule(d, ps, nullptr);
    return e;
}

std::string VulkanBackend::load(const SceneData &scene)
{
    if (m_loaded)
        unload();
    VkDevice d = m_ctx.device;
    // The render targets (§5.3, D12: load() .. unload()). Task 10's clear
    // workload uses ldrA only; tasks 11, 12 and 29 add theirs.
    if (std::string e = makeImage(m_ctx, m_init.renderWidth, m_init.renderHeight, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1,
                                  &m_ldrA);
        !e.empty())
        return e;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 64;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    if (const VkResult r = vkCreateDescriptorPool(d, &dpi, nullptr, &m_loadPool); r != VK_SUCCESS)
        return failure("vkCreateDescriptorPool", r);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = m_loadPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_setLayouts[2];
    if (const VkResult r = vkAllocateDescriptorSets(d, &dai, &m_presentSource); r != VK_SUCCESS)
        return failure("vkAllocateDescriptorSets", r);
    const VkDescriptorImageInfo src{VK_NULL_HANDLE, m_ldrA.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_presentSource;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &src;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);

    m_work = std::make_unique<WorkloadState>();
    m_work->workload = scene.workload;
    m_loaded = true;
    m_lastDraws = 0;
    // The four scenes render into sceneColor (tasks 11, 12), Quads and Sprites into ldrA (task 29);
    // a native 2D workload (never sent to a 3D API) is the clear workload.
    std::string e;
    switch (scene.workload) {
    case Workload::Geometry:
    case Workload::DrawCalls: e = loadScene(scene); break;
    case Workload::Shading: e = loadShading(scene); break;
    case Workload::Particles: e = loadParticles(scene); break;
    case Workload::Quads:
    case Workload::Sprites: e = loadFill(scene); break;
    case Workload::Shapes:
    case Workload::Text:
    case Workload::Blits:
    case Workload::Gradients: break;
    }
    if (!e.empty())
        return e;
    return m_ctx.validationProblem();
}

void VulkanBackend::unload()
{
    if (!m_ctx.device)
        return;
    vkDeviceWaitIdle(m_ctx.device);
    for (Slot &s : m_slots)
        s.submitted = false;  // after the idle wait: their timestamps belong to the old workload
    unloadScene();
    m_work.reset();
    if (m_loadPool)
        vkDestroyDescriptorPool(m_ctx.device, m_loadPool, nullptr);
    m_loadPool = VK_NULL_HANDLE;
    m_presentSource = VK_NULL_HANDLE;
    for (Image *img : {&m_sceneColor, &m_sceneDepth, &m_hdrB, &m_ldrA, &m_ldrB, &m_shadow})
        destroy(m_ctx, *img);
    for (Image &img : m_bloom)
        destroy(m_ctx, img);
    // Set 0 b2 held the shadow map's view: point it at the dummy one now that it is destroyed (L1).
    if (m_dummyDepth.view) {
        for (Slot &s : m_slots) {
            if (!s.set0)
                continue;
            const VkDescriptorImageInfo ii{VK_NULL_HANDLE, m_dummyDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstSet = s.set0;
            x.dstBinding = 2;
            x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            x.pImageInfo = &ii;
            vkUpdateDescriptorSets(m_ctx.device, 1, &x, 0, nullptr);
        }
    }
    m_loaded = false;
    m_lastDraws = 0;
}

void VulkanBackend::releaseWindow()
{
    VkDevice d = m_ctx.device;
    m_swap.destroy(m_ctx);
    for (Slot &s : m_slots) {
        if (s.acquired)
            vkDestroySemaphore(d, s.acquired, nullptr);
        s.acquired = VK_NULL_HANDLE;
    }
    destroy(m_ctx, m_headlessTarget);
    // The VkSurfaceKHR belongs to the window (Qt's QVulkanInstance makes it and
    // destroys it with the platform window, or at hide on Wayland): forget it.
    m_ctx.surface = VK_NULL_HANDLE;
    m_resumed = false;
}

std::string VulkanBackend::suspend()
{
    if (!m_ctx.device)
        return {};
    vkDeviceWaitIdle(m_ctx.device);
    for (Slot &s : m_slots)
        s.submitted = false;
    releaseWindow();
    return m_ctx.validationProblem();
}

std::string VulkanBackend::resume(const PresentTarget &target)
{
    if (!m_ctx.device)
        return "resume() before init()";
    if (m_resumed)
        releaseWindow();
    m_windowScale = target.scale > 0 ? target.scale : 1.0;
    m_presentModeName.clear();
    const auto fail = [this](std::string e) {
        releaseWindow();
        return e;
    };
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    if (m_headless) {
        if (std::string e = makeImage(m_ctx, m_init.renderWidth, m_init.renderHeight, VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, &m_headlessTarget);
            !e.empty())
            return fail(e);
    } else {
        if (!target.makeVulkanSurface)
            return fail("the test window has no Vulkan surface");
        const std::uint64_t surface = target.makeVulkanSurface(m_ctx.instance);
        static_assert(sizeof(VkSurfaceKHR) == sizeof surface, "VkSurfaceKHR is a 64-bit handle");
        std::memcpy(&m_ctx.surface, &surface, sizeof surface);
        if (!m_ctx.surface)
            return fail("the test window couldn't make a Vulkan surface");
        VkBool32 canPresent = VK_FALSE;
        if (const VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(m_ctx.physical, m_ctx.queueFamily, m_ctx.surface, &canPresent);
            r != VK_SUCCESS)
            return fail(failure("vkGetPhysicalDeviceSurfaceSupportKHR", r));
        if (!canPresent)
            return fail("N/A: this GPU's Vulkan queue can't present to the test window");
        for (Slot &s : m_slots) {
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (const VkResult r = vkCreateSemaphore(m_ctx.device, &si, nullptr, &s.acquired); r != VK_SUCCESS)
                return fail(failure("vkCreateSemaphore", r));
        }
        std::string created;
#if defined(SYNBENCH_DEV_HOOKS)
        // Dev hook: "1" = no swapchain can be made; "later" = none after the first resume (L8's test).
        if (const char *hook = std::getenv("SYNBENCH_GPU_FAIL_SWAPCHAIN"); hook && (hook[0] == '1' || (std::strcmp(hook, "later") == 0 && m_everResumed)))
            created = "N/A: this GPU can't present to the test window on this display system";
#endif
        if (created.empty())
            created = m_swap.create(m_ctx, target.windowWidth, target.windowHeight);
        if (!created.empty()) {
            // N/A only on an API's first resume (nothing was ever presented on it): after that it is an Error (L8).
            if (m_everResumed && created.rfind("N/A: ", 0) == 0)
                created = "the swapchain couldn't be made again after the first workload (" + created.substr(5) + ")";
            return fail(created);
        }
        format = m_swap.format;
        m_presentModeName = presentModeName(m_swap.mode);
    }
    if (format != m_presentFormat) {
        if (std::string e = createPresentPipeline(format); !e.empty())
            return fail(e);
    }
    m_resumed = true;
    m_everResumed = true;
    if (std::string e = m_ctx.validationProblem(); !e.empty())
        return fail(e);
    return {};
}

std::string VulkanBackend::waitSlot(std::uint32_t index, std::vector<double> *gpuMs)
{
    Slot &s = m_slots[index];
    if (!s.submitted)
        return {};
    if (std::string e = m_ctx.waitFence(s.fence); !e.empty())
        return e;
    s.submitted = false;
    // VK-5: read only after the fence, masked to timestampValidBits.
    if (gpuMs && m_queries) {
        std::uint64_t q[2] = {};
        const VkResult r = vkGetQueryPoolResults(m_ctx.device, m_queries, 2 * index, 2, sizeof q, q, sizeof q[0], VK_QUERY_RESULT_64_BIT);
        if (r == VK_SUCCESS) {
            const std::uint64_t mask = m_ctx.timestampValidBits >= 64 ? ~0ull : (1ull << m_ctx.timestampValidBits) - 1;
            gpuMs->push_back(double((q[1] - q[0]) & mask) * m_ctx.timestampPeriodNs / 1e6);
        }
    }
    return {};
}

std::string VulkanBackend::waitAll()
{
    for (std::uint32_t k = 0; k < m_slots.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_slots.size()), nullptr); !e.empty())
            return e;
    }
    return {};
}

void VulkanBackend::writePresentConstants(Slot &slot, std::uint32_t windowW, std::uint32_t windowH, std::uint32_t mode, bool withOverlay)
{
    const PresentConstants c = presentConstants(mode, withOverlay ? m_overlay : std::string(), m_windowScale, windowW, windowH);
    std::memcpy(static_cast<std::uint8_t *>(slot.frameConstants.mapped) + kPassBase, &c, sizeof c);
}

bool VulkanBackend::tonemapped() const
{
    return m_work && m_work->tonemap;
}

Image &VulkanBackend::outputImage()
{
    if (m_work && m_work->workload == Workload::Particles)
        return m_ldrB;  // §6.4: the post chain ends in ldrB (FXAA), presented in Copy mode
    return tonemapped() ? m_sceneColor : m_ldrA;
}

void VulkanBackend::recordWorkload(VkCommandBuffer cmd, Slot &slot, double t)
{
    // No workload loaded (unreachable today): the clear workload below, never a fill path without its state.
    if (m_work) {
        switch (m_work->workload) {
        case Workload::Geometry:
        case Workload::DrawCalls: recordScene(cmd, slot, t); return;
        case Workload::Shading: recordShading(cmd, slot, t); return;
        case Workload::Particles: recordParticles(cmd, slot, t); return;
        case Workload::Quads:
        case Workload::Sprites: recordFill(cmd, slot, t); return;
        case Workload::Shapes:
        case Workload::Text:
        case Workload::Blits:
        case Workload::Gradients: break;
        }
    }
    m_lastDraws = 0;  // the clear workload draws nothing before the present pass
    // The previous frame's present pass sampled ldrA and a readback copied it:
    // their reads before this clear (WAR). Its old contents are discarded.
    m_ldrA.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_ldrA, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = m_ldrA.view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colour.clearValue.color.float32, kClearColour, sizeof kClearColour);
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {m_ldrA.width, m_ldrA.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);
}

void VulkanBackend::recordPresentPass(VkCommandBuffer cmd, Slot &slot, VkImageView view, std::uint32_t w, std::uint32_t h, VkPipeline pipeline)
{
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // the full-screen triangle writes every pixel
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &ri);
    // VK-3: the negative-height viewport (y up, as the D3D-convention matrices expect), depth 0..1.
    const VkViewport vp{0.0f, float(h), float(w), -float(h), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const std::uint32_t passOffset = std::uint32_t(kPassBase);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 0, 1, &slot.set0, 1, &passOffset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 2, 1, &m_presentSource, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

std::string VulkanBackend::frame(double t, std::vector<double> *gpuMs)
{
    if (!m_loaded || !m_resumed)
        return "frame() before load() and resume()";
    VkDevice d = m_ctx.device;
    const std::uint32_t si = std::uint32_t(m_frame % m_slots.size());
    Slot &s = m_slots[si];
    // §5.4: frame n reuses frame n-2's slot only after its fence (VK-2), and
    // collects that frame's GPU time (P8).
    if (std::string e = waitSlot(si, gpuMs); !e.empty())
        return e;
    const std::uint32_t outW = m_headless ? m_headlessTarget.width : m_swap.width;
    const std::uint32_t outH = m_headless ? m_headlessTarget.height : m_swap.height;
    writePresentConstants(s, outW, outH, tonemapped() ? kPresentTonemap : kPresentCopy, true);

    std::uint32_t imageIndex = 0;
    if (!m_headless) {
        if (std::string e = m_swap.acquire(m_ctx, s.acquired, &imageIndex); !e.empty())
            return e;
    }
    vkResetFences(d, 1, &s.fence);
    vkResetCommandPool(d, s.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    if (m_queries) {
        vkCmdResetQueryPool(s.cmd, m_queries, 2 * si, 2);  // VK-4
        vkCmdWriteTimestamp2(s.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_queries, 2 * si);
    }

    recordWorkload(s.cmd, s, t);
    transition(s.cmd, outputImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // The present pass (P3) into the swapchain image, or the headless target.
    if (m_headless) {
        // The previous frame's present pass wrote it (WAW) and a readback may have copied it.
        m_headlessTarget.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(s.cmd, m_headlessTarget, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        recordPresentPass(s.cmd, s, m_headlessTarget.view, outW, outH, m_presentPipeline);
    } else {
        // Waits for the acquire semaphore at COLOR_ATTACHMENT_OUTPUT (the submit below).
        imageBarrier(s.cmd, m_swap.images[imageIndex], m_swap.format, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        recordPresentPass(s.cmd, s, m_swap.views[imageIndex], outW, outH, m_presentPipeline);
        imageBarrier(s.cmd, m_swap.images[imageIndex], m_swap.format, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    }
    if (m_queries)
        vkCmdWriteTimestamp2(s.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queries, 2 * si + 1);
    if (const VkResult r = vkEndCommandBuffer(s.cmd); r != VK_SUCCESS)
        return failure("vkEndCommandBuffer", r);

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = s.cmd;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cbi;
    if (!m_headless) {
        wait.semaphore = s.acquired;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        // VK-1: the render-finished semaphore of this swapchain image, not of the slot.
        signal.semaphore = m_swap.renderFinished[imageIndex];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
    }
    if (const VkResult r = vkQueueSubmit2(m_ctx.queue, 1, &submit, s.fence); r != VK_SUCCESS)
        return failure("vkQueueSubmit2", r);
    s.submitted = true;
    ++m_frame;
    if (!m_headless) {
        if (std::string e = m_swap.present(m_ctx, m_swap.renderFinished[imageIndex], imageIndex); !e.empty())
            return e;
    }
    return m_ctx.validationProblem();
}

std::string VulkanBackend::drain(std::vector<double> *gpuMs)
{
    // The oldest pending frame first, so gpuMs stays in submission order.
    for (std::uint32_t k = 0; k < m_slots.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_slots.size()), gpuMs); !e.empty())
            return e;
    }
    return m_ctx.validationProblem();
}

std::string VulkanBackend::readImage(Image &img, const std::function<void(VkCommandBuffer)> &pre, std::vector<std::uint8_t> *rgba8)
{
    if (std::string e = waitAll(); !e.empty())
        return e;
    const VkDeviceSize bytes = VkDeviceSize(img.width) * img.height * 4;
    Buffer host;
    if (std::string e = makeBuffer(m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &host); !e.empty())
        return e;
    std::string e = m_ctx.oneTimeSubmit([&](VkCommandBuffer cmd) {
        if (pre)
            pre(cmd);
        transition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {img.width, img.height, 1};
        vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host.buffer, 1, &region);
        VkBufferMemoryBarrier2 hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = host.buffer;
        hb.size = VK_WHOLE_SIZE;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    if (e.empty()) {
        const auto *p = static_cast<const std::uint8_t *>(host.mapped);
        rgba8->assign(p, p + bytes);  // RGBA8_UNORM, row 0 = the top (the y-up viewport)
    }
    destroy(m_ctx, host);
    if (!e.empty())
        return e;
    return m_ctx.validationProblem();
}

std::string VulkanBackend::readback(double t, std::vector<std::uint8_t> *rgba8)
{
    if (!m_loaded)
        return "readback() before load()";
    // §5.7: the workload's output before the present pass, at render size.
    // Slot 0 is idle (readImage waits for all of them first), so it supplies the constants.
    Slot &s = m_slots[0];
    if (!tonemapped()) {
        return readImage(outputImage(), [&](VkCommandBuffer cmd) { recordWorkload(cmd, s, t); }, rgba8);
    }
    // A tone mapped workload (sceneColor is RGBA16F, linear): the present pass in
    // mode 0, no overlay, into an RGBA8 target at render size; that is what is read.
    if (std::string e = waitAll(); !e.empty())
        return e;
    Image &target = m_work->readTarget;
    writePresentConstants(s, target.width, target.height, kPresentTonemap, false);
    return readImage(
        target,
        [&](VkCommandBuffer cmd) {
            recordWorkload(cmd, s, t);
            transition(cmd, m_sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            target.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            recordPresentPass(cmd, s, target.view, target.width, target.height, m_work->readPipeline);
        },
        rgba8);
}

#if defined(SYNBENCH_DEV_HOOKS)
std::string VulkanBackend::readbackPresented(std::vector<std::uint8_t> *rgba8)
{
    if (!m_headless || !m_resumed || m_frame == 0)
        return "readbackPresented() needs a headless frame";
    return readImage(m_headlessTarget, nullptr, rgba8);
}
#endif

} // namespace sb::gpu::vk

namespace sb::gpu {
std::unique_ptr<IBackend> makeVulkanBackend()
{
    return std::make_unique<vk::VulkanBackend>();
}
} // namespace sb::gpu

// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan Geometry and Draw calls workloads (Phase 4a design §6.1, §6.2,
// task 11): HDR into sceneColor (RGBA16F) with a D32F depth buffer, one
// indexed draw per object. The per-object constants are one dynamic uniform
// buffer per frame slot (256-byte slots, a dynamic offset per draw); the CPU
// rewrites every slot each frame. Vertex and index buffers are bound when the
// mesh changes, the material descriptor set (set 2) when the material changes.
// The present pass then tone maps sceneColor (Present mode 0).

#include "tests/gpu/worker/vulkan/vk_workload_state.h"

#include "tests/gpu/worker/scene_constants.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {
#include "tests/gpu/worker/shaders/generated/drawcalls_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/drawcalls_vs.spv.inc"
#include "tests/gpu/worker/shaders/generated/geometry_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/geometry_vs.spv.inc"
// Task 12.
#include "tests/gpu/worker/shaders/generated/depth_only_vs.spv.inc"
#include "tests/gpu/worker/shaders/generated/ground_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/ground_vs.spv.inc"
#include "tests/gpu/worker/shaders/generated/particles_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/particles_vs.spv.inc"
#include "tests/gpu/worker/shaders/generated/post_bright_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/post_composite_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/post_down_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/post_fxaa_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/post_tonemap_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/post_up_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/present_vs.spv.inc"
#include "tests/gpu/worker/shaders/generated/shading_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/shading_vs.spv.inc"
#include "tests/gpu/worker/shaders/generated/sky_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/sky_vs.spv.inc"
} // namespace

namespace sb::gpu::vk {

namespace {

// The constant layouts and values are scene_constants.h's (shared with D3D12, task 19).
constexpr VkDeviceSize kObjectSlot = sc::kObjectSlot;
constexpr const float *kBackground = sc::kBackground;
static_assert(sc::kBlock == kFrameBlock && sc::kPresentOffset == kPassBase && sc::kLightsBytes == kPassRange
                  && sc::kFrameConstantsBytes == kFrameConstantsBytes,
              "Vulkan's frame-constants layout is scene_constants.h's");

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

std::string VulkanBackend::loadScene(const SceneData &scene)
{
    VkDevice d = m_ctx.device;
    WorkloadState &w = *m_work;
    w.tonemap = true;
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    w.draws = std::uint32_t(scene.instances.size());
    // VK-2 and §5.3: one object slot per draw, 256 bytes each.
    if (m_ctx.properties.limits.minUniformBufferOffsetAlignment > kObjectSlot)
        return "uniform alignment > 256";
    const bool drawCalls = scene.workload == Workload::DrawCalls;

    // The render targets (§5.3): sceneColor RGBA16F, sceneDepth D32F, and the RGBA8 readback target.
    if (std::string e = makeImage(m_ctx, m_init.renderWidth, m_init.renderHeight, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, &m_sceneColor);
        !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, m_init.renderWidth, m_init.renderHeight, VK_FORMAT_D32_SFLOAT,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, &m_sceneDepth);
        !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, m_init.renderWidth, m_init.renderHeight, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, &w.readTarget);
        !e.empty())
        return e;

    // The present pass samples sceneColor now.
    const VkDescriptorImageInfo src{VK_NULL_HANDLE, m_sceneColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet sw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    sw.dstSet = m_presentSource;
    sw.dstBinding = 0;
    sw.descriptorCount = 1;
    sw.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sw.pImageInfo = &src;
    vkUpdateDescriptorSets(d, 1, &sw, 0, nullptr);

    // The meshes: a vertex and an index buffer each (bound when the mesh changes).
    for (const Mesh &mesh : scene.meshes) {
        Buffer vb, ib;
        const VkDeviceSize vbytes = mesh.vertices.size() * sizeof(Vertex), ibytes = mesh.indices.size() * sizeof(std::uint32_t);
        if (std::string e = makeBuffer(m_ctx, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &vb);
            !e.empty())
            return e;
        w.vertexBuffers.push_back(vb);
        if (std::string e = uploadBuffer(m_ctx, w.vertexBuffers.back(), mesh.vertices.data(), std::size_t(vbytes)); !e.empty())
            return e;
        if (std::string e = makeBuffer(m_ctx, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &ib);
            !e.empty())
            return e;
        w.indexBuffers.push_back(ib);
        if (std::string e = uploadBuffer(m_ctx, w.indexBuffers.back(), mesh.indices.data(), std::size_t(ibytes)); !e.empty())
            return e;
        w.indexCounts.push_back(std::uint32_t(mesh.indices.size()));
    }

    // The materials (Draw calls: 16 textures, one set 2 each).
    for (const Texture &tex : scene.textures) {
        Image img;
        if (std::string e = uploadTexture(m_ctx, tex, &img); !e.empty())
            return e;
        w.materials.push_back(img);
    }
    for (const Image &img : w.materials) {
        VkDescriptorSet set{};
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = m_loadPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &m_setLayouts[2];
        if (const VkResult r = vkAllocateDescriptorSets(d, &dai, &set); r != VK_SUCCESS)
            return failure("vkAllocateDescriptorSets (material)", r);
        w.materialSets.push_back(set);
        const VkDescriptorImageInfo ii{VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set;
        x.dstBinding = 0;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        x.pImageInfo = &ii;
        vkUpdateDescriptorSets(d, 1, &x, 0, nullptr);
    }

    // The per-slot object constants and their set 1.
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        Slot &s = m_slots[i];
        if (std::string e = makeBuffer(m_ctx, std::max<VkDeviceSize>(w.draws, 1) * kObjectSlot, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                                       &s.objectConstants);
            !e.empty())
            return e;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = m_loadPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &m_setLayouts[1];
        if (const VkResult r = vkAllocateDescriptorSets(d, &dai, &w.objectSets[i]); r != VK_SUCCESS)
            return failure("vkAllocateDescriptorSets (objects)", r);
        const VkDescriptorBufferInfo bi{s.objectConstants.buffer, 0, kObjectSlot};
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = w.objectSets[i];
        x.dstBinding = 0;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        x.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d, 1, &x, 0, nullptr);
    }

    // The scene pipeline: vertex layout of Vertex (32 bytes), depth LESS with writes, no blending.
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    std::string e = drawCalls ? makeShader(d, kDrawcallsVsSpv, sizeof kDrawcallsVsSpv, &vs) : makeShader(d, kGeometryVsSpv, sizeof kGeometryVsSpv, &vs);
    if (e.empty())
        e = drawCalls ? makeShader(d, kDrawcallsPsSpv, sizeof kDrawcallsPsSpv, &ps) : makeShader(d, kGeometryPsSpv, sizeof kGeometryPsSpv, &ps);
    if (e.empty()) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "VSMain";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "PSMain";
        const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attrs[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)},
                                                            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)},
                                                            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)}};
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 3;
        vin.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;  // no culling: the same on every API (the design doesn't ask for it)
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        const VkFormat colourFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &colourFormat;
        ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
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
        if (const VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &w.pipeline); r != VK_SUCCESS) {
            w.pipeline = VK_NULL_HANDLE;
            e = failure("vkCreateGraphicsPipelines (scene)", r);
        }
    }
    if (vs)
        vkDestroyShaderModule(d, vs, nullptr);
    if (ps)
        vkDestroyShaderModule(d, ps, nullptr);
    if (!e.empty())
        return e;
    // The present pass into the RGBA8 readback target (§5.7: tone mapped, before the overlay).
    return buildPresentPipeline(VK_FORMAT_R8G8B8A8_UNORM, &w.readPipeline);
}

void VulkanBackend::unloadScene()
{
    if (!m_work)
        return;
    VkDevice d = m_ctx.device;
    WorkloadState &w = *m_work;
    if (w.pipeline)
        vkDestroyPipeline(d, w.pipeline, nullptr);
    if (w.readPipeline)
        vkDestroyPipeline(d, w.readPipeline, nullptr);
    w.pipeline = w.readPipeline = VK_NULL_HANDLE;
    for (Buffer &b : w.vertexBuffers)
        destroy(m_ctx, b);
    for (Buffer &b : w.indexBuffers)
        destroy(m_ctx, b);
    for (Image &i : w.materials)
        destroy(m_ctx, i);
    destroy(m_ctx, w.readTarget);
    // Task 12 (Shading, Particles + post).
    for (VkPipeline *p : {&w.shadowPipeline, &w.prepassPipeline, &w.skyPipeline, &w.particlePipeline, &w.brightPipeline,
                          &w.downPipeline, &w.upPipeline, &w.compositePipeline, &w.tonemapPipeline, &w.fxaaPipeline}) {
        if (*p)
            vkDestroyPipeline(d, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    destroy(m_ctx, w.particles);
    for (Slot &s : m_slots)
        destroy(m_ctx, s.objectConstants);  // VK-2: made in load(), freed here
}

void VulkanBackend::recordScene(VkCommandBuffer cmd, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t slotIndex = std::uint32_t(&slot - m_slots.data());
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;

    // The CPU side: this slot's Frame constants and every object slot (its fence
    // has passed, VK-2), the same bytes on every API (scene_constants.h).
    sc::writeFrame(w.scene, t, width, height, static_cast<std::uint8_t *>(slot.frameConstants.mapped));
    sc::writeObjects(w.scene, t, static_cast<std::uint8_t *>(slot.objectConstants.mapped));

    // The previous frame's present pass sampled sceneColor and a readback may
    // have copied it (WAR), and the depth buffer's last writes (WAW): both are
    // discarded, so the first use of each in the frame starts from UNDEFINED.
    m_sceneColor.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_sceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    m_sceneDepth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_sceneDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = m_sceneColor.view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colour.clearValue.color.float32, kBackground, sizeof sc::kBackground);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_sceneDepth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {width, height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    // VK-3: the negative-height viewport (y up, as the D3D-convention matrices expect), depth 0..1.
    const VkViewport vp{0.0f, float(height), float(width), -float(height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w.pipeline);
    const std::uint32_t passOffset = std::uint32_t(kPassBase);  // set 0 always takes one dynamic offset
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 0, 1, &slot.set0, 1, &passOffset);

    // §6.2 per draw: the mesh's buffers if they differ from the last draw's, the
    // material's set if it differs, the object's constants slot (always), then draw.
    std::uint32_t draws = 0;
    std::uint32_t lastMesh = UINT32_MAX, lastMaterial = UINT32_MAX;
    for (std::size_t i = 0; i < w.scene.instances.size(); ++i) {
        const Instance &inst = w.scene.instances[i];
        if (inst.mesh != lastMesh) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &w.vertexBuffers[inst.mesh].buffer, &zero);
            vkCmdBindIndexBuffer(cmd, w.indexBuffers[inst.mesh].buffer, 0, VK_INDEX_TYPE_UINT32);
            lastMesh = inst.mesh;
        }
        if (!w.materialSets.empty() && inst.material != lastMaterial) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 2, 1, &w.materialSets[inst.material], 0, nullptr);
            lastMaterial = inst.material;
        }
        const std::uint32_t objectOffset = std::uint32_t(i * kObjectSlot);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 1, 1, &w.objectSets[slotIndex], 1, &objectOffset);
        vkCmdDrawIndexed(cmd, w.indexCounts[inst.mesh], 1, 0, 0, 0);
        ++draws;
    }
    vkCmdEndRendering(cmd);
    m_lastDraws = draws;
}

// ---- Task 12: Shading (§6.3) and Particles + post-processing (§6.4) ----

namespace {

// The pass blocks of task 12 (scene_constants.h): Lights at 512, then 256-byte
// blocks from 4608 (DepthPass x 2, or Post x 14).
constexpr VkDeviceSize kLightsOffset = sc::kLightsOffset;
constexpr std::uint32_t kShadowBlock = sc::kShadowBlock, kPrepassBlock = sc::kPrepassBlock, kShadowSize = sc::kShadowSize;

std::uint32_t blockOffset(std::uint32_t block)
{
    return std::uint32_t(sc::blockOffset(block));
}

// A graphics pipeline on the shared layout (task 12's passes).
struct PipelineDesc {
    const std::uint32_t *vs = nullptr;
    std::size_t vsBytes = 0;
    const std::uint32_t *ps = nullptr;  // null: no fragment shader (depth only)
    std::size_t psBytes = 0;
    enum class Input { None, Position, Mesh, Particle } input = Input::None;
    VkFormat colour = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED;
    bool depthTest = false, depthWrite = false;
    VkCompareOp compare = VK_COMPARE_OP_LESS;
    bool additive = false;   // blend ONE, ONE
    bool depthBias = false;  // VK-7: depthBiasEnable + the dynamic state
};

std::string makePipeline(VkDevice d, VkPipelineLayout layout, const PipelineDesc &p, VkPipeline *out)
{
    *out = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    std::string e = makeShader(d, p.vs, p.vsBytes, &vs);
    if (e.empty() && p.ps)
        e = makeShader(d, p.ps, p.psBytes, &ps);
    if (e.empty()) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "VSMain";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "PSMain";
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)},
                                                      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)},
                                                      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)}};
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (p.input == PipelineDesc::Input::Particle) {
            binding = {0, sizeof(Particle), VK_VERTEX_INPUT_RATE_INSTANCE};
            attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16};
        }
        if (p.input != PipelineDesc::Input::None) {
            vin.vertexBindingDescriptionCount = 1;
            vin.pVertexBindingDescriptions = &binding;
            vin.vertexAttributeDescriptionCount = p.input == PipelineDesc::Input::Position ? 1u
                                                  : p.input == PipelineDesc::Input::Particle ? 2u
                                                                                             : 3u;
            vin.pVertexAttributeDescriptions = attrs;
        }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;  // task 11: both faces on every API
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.depthBiasEnable = p.depthBias ? VK_TRUE : VK_FALSE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = p.depthTest ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = p.depthWrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = p.compare;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (p.additive) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            att.srcAlphaBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = p.colour != VK_FORMAT_UNDEFINED ? 1u : 0u;
        cb.pAttachments = &att;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = p.depthBias ? 3u : 2u;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = p.colour != VK_FORMAT_UNDEFINED ? 1u : 0u;
        ri.pColorAttachmentFormats = &p.colour;
        ri.depthAttachmentFormat = p.depth;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &ri;
        gp.stageCount = p.ps ? 2u : 1u;
        gp.pStages = st;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dy;
        gp.layout = layout;
        if (const VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, out); r != VK_SUCCESS) {
            *out = VK_NULL_HANDLE;
            e = failure("vkCreateGraphicsPipelines", r);
        }
    }
    if (vs)
        vkDestroyShaderModule(d, vs, nullptr);
    if (ps)
        vkDestroyShaderModule(d, ps, nullptr);
    return e;
}

void writeImages(VkDevice d, VkDescriptorSet set, std::initializer_list<VkImageView> views)
{
    std::vector<VkDescriptorImageInfo> infos;
    for (VkImageView v : views)
        infos.push_back({VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    std::vector<VkWriteDescriptorSet> writes;
    for (std::uint32_t i = 0; i < infos.size(); ++i) {
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set;
        x.dstBinding = i;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        x.pImageInfo = &infos[i];
        writes.push_back(x);
    }
    vkUpdateDescriptorSets(d, std::uint32_t(writes.size()), writes.data(), 0, nullptr);
}

// The scene viewport (VK-3: negative height, y up; depth 0..1) and scissor.
void setViewport(VkCommandBuffer cmd, std::uint32_t w, std::uint32_t h)
{
    const VkViewport vp{0.0f, float(h), float(w), -float(h), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

// The stages that read a frame's targets last in the previous frame (the
// present pass and post passes sample, a readback copies): the source scope
// of a target's first write in a frame, which discards it (UNDEFINED).
constexpr VkPipelineStageFlags2 kPriorReaders = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
constexpr VkPipelineStageFlags2 kDepthTests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

} // namespace

// Allocates one set of `layout` from the load pool.
static std::string allocSet(VkDevice d, VkDescriptorPool pool, VkDescriptorSetLayout layout, VkDescriptorSet *out)
{
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &layout;
    if (const VkResult r = vkAllocateDescriptorSets(d, &dai, out); r != VK_SUCCESS)
        return failure("vkAllocateDescriptorSets", r);
    return {};
}

// Vertex/index buffers per mesh, the per-slot object constants and their set 1
// (as loadScene does for Geometry and Draw calls). A template only because
// WorkloadState is private to the backend.
template <class State>
static std::string loadMeshesAndObjects(Context &ctx, VkDescriptorPool pool, VkDescriptorSetLayout objectLayout,
                                        const SceneData &scene, State &w,
                                        std::array<Buffer *, 2> objectBuffers)
{
    for (const Mesh &mesh : scene.meshes) {
        Buffer vb, ib;
        const VkDeviceSize vbytes = mesh.vertices.size() * sizeof(Vertex), ibytes = mesh.indices.size() * sizeof(std::uint32_t);
        if (std::string e = makeBuffer(ctx, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &vb);
            !e.empty())
            return e;
        w.vertexBuffers.push_back(vb);
        if (std::string e = uploadBuffer(ctx, w.vertexBuffers.back(), mesh.vertices.data(), std::size_t(vbytes)); !e.empty())
            return e;
        if (std::string e = makeBuffer(ctx, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &ib);
            !e.empty())
            return e;
        w.indexBuffers.push_back(ib);
        if (std::string e = uploadBuffer(ctx, w.indexBuffers.back(), mesh.indices.data(), std::size_t(ibytes)); !e.empty())
            return e;
        w.indexCounts.push_back(std::uint32_t(mesh.indices.size()));
    }
    for (std::size_t i = 0; i < objectBuffers.size(); ++i) {
        Buffer &b = *objectBuffers[i];
        if (std::string e = makeBuffer(ctx, std::max<VkDeviceSize>(scene.instances.size(), 1) * kObjectSlot,
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &b);
            !e.empty())
            return e;
        if (std::string e = allocSet(ctx.device, pool, objectLayout, &w.objectSets[i]); !e.empty())
            return e;
        const VkDescriptorBufferInfo bi{b.buffer, 0, kObjectSlot};
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = w.objectSets[i];
        x.dstBinding = 0;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        x.pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx.device, 1, &x, 0, nullptr);
    }
    return {};
}

std::string VulkanBackend::loadShading(const SceneData &scene)
{
    VkDevice d = m_ctx.device;
    WorkloadState &w = *m_work;
    w.tonemap = true;
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    w.scene.lights = scene.lights;
    w.scene.lightMotion = scene.lightMotion;
    if (scene.lights.size() > kPointLights)
        return "more point lights than the Lights cbuffer holds";
    if (m_ctx.properties.limits.minUniformBufferOffsetAlignment > kObjectSlot)
        return "uniform alignment > 256";
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;

    // The render targets (§5.3): sceneColor, sceneDepth, the 2048^2 D32F shadow map, the readback target.
    if (std::string e = makeImage(m_ctx, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  1, &m_sceneColor);
        !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, W, H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, &m_sceneDepth); !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, kShadowSize, kShadowSize, VK_FORMAT_D32_SFLOAT,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, &m_shadow);
        !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  1, &w.readTarget);
        !e.empty())
        return e;
    writeImages(d, m_presentSource, {m_sceneColor.view});
    // Set 0 b2 = the shadow map, in both slots' set 0 (the device is idle in load()).
    for (Slot &s : m_slots) {
        const VkDescriptorImageInfo ii{VK_NULL_HANDLE, m_shadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = s.set0;
        x.dstBinding = 2;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        x.pImageInfo = &ii;
        vkUpdateDescriptorSets(d, 1, &x, 0, nullptr);
    }

    if (std::string e = loadMeshesAndObjects(m_ctx, m_loadPool, m_setLayouts[1], scene, w,
                                             {&m_slots[0].objectConstants, &m_slots[1].objectConstants});
        !e.empty())
        return e;
    // The four 1024^2 textures with full mip chains, one set 2 (b0-b3).
    for (const Texture &tex : scene.textures) {
        Image img;
        if (std::string e = uploadTexture(m_ctx, tex, &img); !e.empty())
            return e;
        w.materials.push_back(img);
    }
    if (w.materials.size() != 4)
        return "the Shading scene needs 4 textures";
    w.materialSets.resize(1);
    if (std::string e = allocSet(d, m_loadPool, m_setLayouts[2], &w.materialSets[0]); !e.empty())
        return e;
    writeImages(d, w.materialSets[0], {w.materials[0].view, w.materials[1].view, w.materials[2].view, w.materials[3].view});

    // The three passes' pipelines.
    PipelineDesc shadow;
    shadow.vs = kDepthOnlyVsSpv;
    shadow.vsBytes = sizeof kDepthOnlyVsSpv;
    shadow.input = PipelineDesc::Input::Position;
    shadow.depth = VK_FORMAT_D32_SFLOAT;
    shadow.depthTest = shadow.depthWrite = true;
    shadow.compare = VK_COMPARE_OP_LESS;
    shadow.depthBias = true;
    if (std::string e = makePipeline(d, m_presentLayout, shadow, &w.shadowPipeline); !e.empty())
        return e;
    PipelineDesc prepass = shadow;
    prepass.depthBias = false;
    if (std::string e = makePipeline(d, m_presentLayout, prepass, &w.prepassPipeline); !e.empty())
        return e;
    PipelineDesc shade;
    shade.vs = kShadingVsSpv;
    shade.vsBytes = sizeof kShadingVsSpv;
    shade.ps = kShadingPsSpv;
    shade.psBytes = sizeof kShadingPsSpv;
    shade.input = PipelineDesc::Input::Mesh;
    shade.colour = VK_FORMAT_R16G16B16A16_SFLOAT;
    shade.depth = VK_FORMAT_D32_SFLOAT;
    shade.depthTest = true;
    shade.depthWrite = false;
    shade.compare = VK_COMPARE_OP_EQUAL;
    if (std::string e = makePipeline(d, m_presentLayout, shade, &w.pipeline); !e.empty())
        return e;
    w.draws = 3 * std::uint32_t(scene.instances.size());
    return buildPresentPipeline(VK_FORMAT_R8G8B8A8_UNORM, &w.readPipeline);
}

void VulkanBackend::recordShading(VkCommandBuffer cmd, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t slotIndex = std::uint32_t(&slot - m_slots.data());
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;

    // The CPU side, after this slot's fence (VK-2): Frame (the Shading sun, its
    // shadow matrix, the light count in sunDir.w), the Lights and DepthPass
    // blocks, every object (scene_constants.h, the same bytes on every API).
    sc::writeFrame(w.scene, t, width, height, static_cast<std::uint8_t *>(slot.frameConstants.mapped));
    sc::writeObjects(w.scene, t, static_cast<std::uint8_t *>(slot.objectConstants.mapped));

    // The same draw loop in all three passes (§6.2's rule: buffers when the mesh changes).
    std::uint32_t draws = 0;
    const auto drawAll = [&] {
        std::uint32_t lastMesh = UINT32_MAX;
        for (std::size_t i = 0; i < w.scene.instances.size(); ++i) {
            const Instance &inst = w.scene.instances[i];
            if (inst.mesh != lastMesh) {
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &w.vertexBuffers[inst.mesh].buffer, &zero);
                vkCmdBindIndexBuffer(cmd, w.indexBuffers[inst.mesh].buffer, 0, VK_INDEX_TYPE_UINT32);
                lastMesh = inst.mesh;
            }
            const std::uint32_t objectOffset = std::uint32_t(i * kObjectSlot);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 1, 1, &w.objectSets[slotIndex], 1, &objectOffset);
            vkCmdDrawIndexed(cmd, w.indexCounts[inst.mesh], 1, 0, 0, 0);
            ++draws;
        }
    };
    const auto depthOnly = [&](Image &target, VkAttachmentStoreOp store) {
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = target.view;
        depth.imageLayout = target.layout;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = store;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {target.width, target.height}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        setViewport(cmd, target.width, target.height);
    };

    // 1. The shadow pass: depth only into the shadow map, slope bias 2.0 (the
    //    constant part is in depth_only.hlsl). The previous frame's shading
    //    pass sampled it (WAR); its contents are discarded.
    m_shadow.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_shadow, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
               kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    depthOnly(m_shadow, VK_ATTACHMENT_STORE_OP_STORE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w.shadowPipeline);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 2.0f);
    std::uint32_t passOffset = blockOffset(kShadowBlock);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 0, 1, &slot.set0, 1, &passOffset);
    drawAll();
    vkCmdEndRendering(cmd);
    // Shadow depth writes -> the shading pass's fragment-shader reads.
    transition(cmd, m_shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // 2. The depth pre-pass into sceneDepth (LESS, written). Last frame's
    //    shading pass tested it and its DONT_CARE store wrote it (WAW).
    m_sceneDepth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_sceneDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    depthOnly(m_sceneDepth, VK_ATTACHMENT_STORE_OP_STORE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w.prepassPipeline);
    passOffset = blockOffset(kPrepassBlock);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 0, 1, &slot.set0, 1, &passOffset);
    drawAll();
    vkCmdEndRendering(cmd);
    // The pre-pass's depth writes (and store) -> the shading pass's load and EQUAL
    // test. Two dynamic-rendering instances aren't ordered on their own, so this
    // barrier is needed (the layout stays DEPTH_ATTACHMENT_OPTIMAL).
    transition(cmd, m_sceneDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    // 3. The shading pass: EQUAL, no depth writes, into sceneColor (cleared).
    m_sceneColor.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_sceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kPriorReaders, VK_ACCESS_2_NONE,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = m_sceneColor.view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colour.clearValue.color.float32, kBackground, sizeof sc::kBackground);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_sceneDepth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {width, height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, width, height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w.pipeline);
    passOffset = std::uint32_t(kLightsOffset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 0, 1, &slot.set0, 1, &passOffset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 2, 1, &w.materialSets[0], 0, nullptr);
    drawAll();
    vkCmdEndRendering(cmd);
    m_lastDraws = draws;
}

std::string VulkanBackend::loadParticles(const SceneData &scene)
{
    VkDevice d = m_ctx.device;
    WorkloadState &w = *m_work;
    w.tonemap = false;  // the chain tone maps itself; the present pass copies ldrB
    w.scene.workload = scene.workload;
    w.scene.instances = scene.instances;
    w.scene.spins = scene.spins;
    const std::uint32_t W = m_init.renderWidth, H = m_init.renderHeight;
    const VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // The render targets (§5.3): sceneColor, sceneDepth (sampled by the fog),
    // bloom[0..5] at ceil(size / 2^(k+1)) (VK-6: never 0), hdrB, ldrB; ldrA is load()'s.
    if (std::string e = makeImage(m_ctx, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, target, 1, &m_sceneColor); !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, W, H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, &m_sceneDepth);
        !e.empty())
        return e;
    for (std::uint32_t k = 0; k < m_bloom.size(); ++k) {
        const std::uint32_t div = 2u << k;
        if (std::string e = makeImage(m_ctx, std::max(1u, (W + div - 1) / div), std::max(1u, (H + div - 1) / div),
                                      VK_FORMAT_R16G16B16A16_SFLOAT, target, 1, &m_bloom[k]);
            !e.empty())
            return e;
    }
    if (std::string e = makeImage(m_ctx, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, target, 1, &m_hdrB); !e.empty())
        return e;
    if (std::string e = makeImage(m_ctx, W, H, VK_FORMAT_R8G8B8A8_UNORM, target | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, &m_ldrB); !e.empty())
        return e;
    writeImages(d, m_presentSource, {m_ldrB.view});

    // The ground (geometry.hlsl, one object) and the particles (one instance each).
    if (std::string e = loadMeshesAndObjects(m_ctx, m_loadPool, m_setLayouts[1], scene, w,
                                             {&m_slots[0].objectConstants, &m_slots[1].objectConstants});
        !e.empty())
        return e;
    w.particleCount = std::uint32_t(scene.particles.size());
    if (std::string e = makeBuffer(m_ctx, std::max<VkDeviceSize>(scene.particles.size(), 1) * sizeof(Particle),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &w.particles);
        !e.empty())
        return e;
    if (std::string e = uploadBuffer(m_ctx, w.particles, scene.particles.data(), scene.particles.size() * sizeof(Particle)); !e.empty())
        return e;
    if (scene.textures.size() != 1)
        return "the Particles scene needs its disc texture";
    Image disc;
    if (std::string e = uploadTexture(m_ctx, scene.textures[0], &disc); !e.empty())
        return e;
    w.materials.push_back(disc);
    w.materialSets.resize(1);
    if (std::string e = allocSet(d, m_loadPool, m_setLayouts[2], &w.materialSets[0]); !e.empty())
        return e;
    writeImages(d, w.materialSets[0], {disc.view});

    // Set 2 of every post pass, in pass order.
    for (VkDescriptorSet &set : w.postSets) {
        if (std::string e = allocSet(d, m_loadPool, m_setLayouts[2], &set); !e.empty())
            return e;
    }
    std::size_t n = 0;
    writeImages(d, w.postSets[n++], {m_sceneColor.view});  // 1 bright
    for (std::size_t k = 1; k < m_bloom.size(); ++k)
        writeImages(d, w.postSets[n++], {m_bloom[k - 1].view});  // 2-6 down
    for (std::size_t k = m_bloom.size() - 1; k >= 1; --k)
        writeImages(d, w.postSets[n++], {m_bloom[k].view});  // 7-11 up: [k] -> [k-1]
    writeImages(d, w.postSets[n++], {m_sceneColor.view, m_bloom[0].view});  // 12 composite (no depth: D2)
    writeImages(d, w.postSets[n++], {m_hdrB.view});  // 13 tone map
    writeImages(d, w.postSets[n++], {m_ldrA.view});  // 14 FXAA

    // The pipelines: sky, ground, particles; the post passes on present's full-screen VS.
    PipelineDesc sky;
    sky.vs = kSkyVsSpv;
    sky.vsBytes = sizeof kSkyVsSpv;
    sky.ps = kSkyPsSpv;
    sky.psBytes = sizeof kSkyPsSpv;
    sky.colour = VK_FORMAT_R16G16B16A16_SFLOAT;
    sky.depth = VK_FORMAT_D32_SFLOAT;
    sky.depthTest = true;
    sky.compare = VK_COMPARE_OP_LESS_OR_EQUAL;  // at the far plane, against the cleared 1.0
    if (std::string e = makePipeline(d, m_presentLayout, sky, &w.skyPipeline); !e.empty())
        return e;
    PipelineDesc ground = sky;
    ground.vs = kGroundVsSpv;  // geometry.hlsl's lighting plus the height fog (D2)
    ground.vsBytes = sizeof kGroundVsSpv;
    ground.ps = kGroundPsSpv;
    ground.psBytes = sizeof kGroundPsSpv;
    ground.input = PipelineDesc::Input::Mesh;
    ground.depthWrite = true;
    ground.compare = VK_COMPARE_OP_LESS;
    if (std::string e = makePipeline(d, m_presentLayout, ground, &w.pipeline); !e.empty())
        return e;
    PipelineDesc parts = sky;
    parts.vs = kParticlesVsSpv;
    parts.vsBytes = sizeof kParticlesVsSpv;
    parts.ps = kParticlesPsSpv;
    parts.psBytes = sizeof kParticlesPsSpv;
    parts.input = PipelineDesc::Input::Particle;
    parts.compare = VK_COMPARE_OP_LESS_OR_EQUAL;  // tested, not written
    parts.additive = true;
    if (std::string e = makePipeline(d, m_presentLayout, parts, &w.particlePipeline); !e.empty())
        return e;
    const auto post = [&](const std::uint32_t *ps, std::size_t bytes, VkFormat format, bool additive, VkPipeline *out) {
        PipelineDesc p;
        p.vs = kPresentVsSpv;
        p.vsBytes = sizeof kPresentVsSpv;
        p.ps = ps;
        p.psBytes = bytes;
        p.colour = format;
        p.additive = additive;
        return makePipeline(d, m_presentLayout, p, out);
    };
    const VkFormat hdr = VK_FORMAT_R16G16B16A16_SFLOAT, ldr = VK_FORMAT_R8G8B8A8_UNORM;
    for (std::string e : {post(kPostBrightPsSpv, sizeof kPostBrightPsSpv, hdr, false, &w.brightPipeline),
                          post(kPostDownPsSpv, sizeof kPostDownPsSpv, hdr, false, &w.downPipeline),
                          post(kPostUpPsSpv, sizeof kPostUpPsSpv, hdr, true, &w.upPipeline),
                          post(kPostCompositePsSpv, sizeof kPostCompositePsSpv, hdr, false, &w.compositePipeline),
                          post(kPostTonemapPsSpv, sizeof kPostTonemapPsSpv, ldr, false, &w.tonemapPipeline),
                          post(kPostFxaaPsSpv, sizeof kPostFxaaPsSpv, ldr, false, &w.fxaaPipeline)}) {
        if (!e.empty())
            return e;
    }
    w.draws = 3 + 14;
    return {};
}

void VulkanBackend::postPass(VkCommandBuffer cmd, Slot &slot, Image &target, bool accumulate, bool toShaderRead, VkPipeline pipeline,
                             VkDescriptorSet sources, std::uint32_t block)
{
    if (accumulate) {
        // An upsample adds into the next larger mip (LOAD_OP_LOAD, blend ONE, ONE):
        // its own earlier write (the downsample pass that made it) must be visible to
        // the blend's read, and the downsample that sampled it must be done (WAR).
        transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    } else {
        // The first write this frame: discard; last frame's samplers/copies first (WAR).
        target.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kPriorReaders, VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = target.view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = accumulate ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // the triangle covers every pixel
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {target.width, target.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, target.width, target.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const std::uint32_t offset = blockOffset(block);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 0, 1, &slot.set0, 1, &offset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 2, 1, &sources, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (toShaderRead) {
        // This pass's writes -> the next passes' fragment-shader reads.
        transition(cmd, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}

void VulkanBackend::recordParticles(VkCommandBuffer cmd, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t slotIndex = std::uint32_t(&slot - m_slots.data());
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;

    // The CPU side, after this slot's fence (VK-2): Frame (the Geometry sun lights
    // the ground, the fog), the ground's object slot and the 14 Post blocks
    // (scene_constants.h, the same bytes on every API).
    sc::writeFrame(w.scene, t, width, height, static_cast<std::uint8_t *>(slot.frameConstants.mapped));
    sc::writeObjects(w.scene, t, static_cast<std::uint8_t *>(slot.objectConstants.mapped));

    // The scene pass: sky, ground, particles into sceneColor + sceneDepth. Last
    // frame's composite sampled both (WAR); their contents are discarded.
    m_sceneColor.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_sceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kPriorReaders, VK_ACCESS_2_NONE,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);  // additive blending reads
    // Last frame's scene pass tested and stored (DONT_CARE) the depth (WAW).
    m_sceneDepth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, m_sceneDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = m_sceneColor.view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colour.clearValue.color.float32, kBackground, sizeof sc::kBackground);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_sceneDepth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // nothing reads it after the scene pass (D2)
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {width, height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, width, height);
    const std::uint32_t passOffset = std::uint32_t(kPassBase);  // no pass block used; set 0 takes one offset
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 0, 1, &slot.set0, 1, &passOffset);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w.skyPipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w.pipeline);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &w.vertexBuffers[0].buffer, &zero);
    vkCmdBindIndexBuffer(cmd, w.indexBuffers[0].buffer, 0, VK_INDEX_TYPE_UINT32);
    const std::uint32_t objectOffset = 0;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 1, 1, &w.objectSets[slotIndex], 1, &objectOffset);
    vkCmdDrawIndexed(cmd, w.indexCounts[0], 1, 0, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w.particlePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentLayout, 2, 1, &w.materialSets[0], 0, nullptr);
    vkCmdBindVertexBuffers(cmd, 0, 1, &w.particles.buffer, &zero);
    vkCmdDraw(cmd, 6, w.particleCount, 0, 0);
    vkCmdEndRendering(cmd);
    // Scene colour writes -> the post passes' fragment-shader reads (the depth isn't read: D2).
    transition(cmd, m_sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // The 14 post passes (§6.4).
    std::uint32_t n = 0;
    postPass(cmd, slot, m_bloom[0], false, true, w.brightPipeline, w.postSets[n], n), ++n;  // 1
    for (std::size_t k = 1; k < m_bloom.size(); ++k)
        postPass(cmd, slot, m_bloom[k], false, true, w.downPipeline, w.postSets[n], n), ++n;  // 2-6
    for (std::size_t k = m_bloom.size() - 1; k >= 1; --k)
        postPass(cmd, slot, m_bloom[k - 1], true, true, w.upPipeline, w.postSets[n], n), ++n;  // 7-11
    postPass(cmd, slot, m_hdrB, false, true, w.compositePipeline, w.postSets[n], n), ++n;  // 12
    postPass(cmd, slot, m_ldrA, false, true, w.tonemapPipeline, w.postSets[n], n), ++n;    // 13
    // 14: FXAA into ldrB, left a colour attachment: frame() / readback() transition it (outputImage()).
    postPass(cmd, slot, m_ldrB, false, false, w.fxaaPipeline, w.postSets[n], n), ++n;
    m_lastDraws = 3 + n;
}

} // namespace sb::gpu::vk

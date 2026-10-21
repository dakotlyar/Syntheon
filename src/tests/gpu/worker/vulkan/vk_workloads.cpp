// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan Geometry and Draw calls workloads (Phase 4a design §6.1, §6.2,
// task 11): HDR into sceneColor (RGBA16F) with a D32F depth buffer, one
// indexed draw per object. The per-object constants are one dynamic uniform
// buffer per frame slot (256-byte slots, a dynamic offset per draw); the CPU
// rewrites every slot each frame. Vertex and index buffers are bound when the
// mesh changes, the material descriptor set (set 2) when the material changes.
// The present pass then tone maps sceneColor (Present mode 0).

#include "tests/gpu/worker/vulkan/vk_workload_state.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {
#include "tests/gpu/worker/shaders/generated/drawcalls_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/drawcalls_vs.spv.inc"
#include "tests/gpu/worker/shaders/generated/geometry_ps.spv.inc"
#include "tests/gpu/worker/shaders/generated/geometry_vs.spv.inc"
} // namespace

namespace sb::gpu::vk {

namespace {

// common.hlsli's cbuffer Frame (b0): 256 bytes, kFrameBlock.
struct FrameConstants {
    float viewProj[16], lightViewProj[16];
    float cameraPos[4], cameraRight[4], cameraUp[4];
    float sunDir[4], sunColour[4], skyAmbient[4], groundAmbient[4];
    float renderSize[4];
};
static_assert(sizeof(FrameConstants) == kFrameBlock, "the Frame cbuffer is 256 bytes");

// geometry.hlsl / drawcalls.hlsl's cbuffer Object (b1), at the start of each 256-byte slot.
struct ObjectConstants {
    float model[16];
    float colour[4];
};
constexpr VkDeviceSize kObjectSlot = 256;
static_assert(sizeof(ObjectConstants) <= kObjectSlot, "an object's constants fit its slot");

// §6.1: the sun, normalise(-0.4, -1, -0.3), colour (1, 0.96, 0.90) x 3; the
// hemisphere ambient, sky (0.25, 0.30, 0.40) and ground (0.08, 0.07, 0.06).
// The Draw calls scene uses the same.
constexpr float kBackground[4] = {0.02f, 0.02f, 0.03f, 1.0f};

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
    for (Slot &s : m_slots)
        destroy(m_ctx, s.objectConstants);  // VK-2: made in load(), freed here
}

void VulkanBackend::recordScene(VkCommandBuffer cmd, Slot &slot, double t)
{
    WorkloadState &w = *m_work;
    const std::uint32_t slotIndex = std::uint32_t(&slot - m_slots.data());
    const std::uint32_t width = m_sceneColor.width, height = m_sceneColor.height;

    // The CPU side: this slot's Frame constants and every object slot (its fence has passed, VK-2).
    const Camera cam = cameraAt(w.scene.workload, t, double(width) / double(height));
    FrameConstants fc{};
    const Mat4 viewProj = cam.proj * cam.view;
    std::memcpy(fc.viewProj, viewProj.m, sizeof fc.viewProj);
    const Mat4 identity = Mat4::identity();
    std::memcpy(fc.lightViewProj, identity.m, sizeof fc.lightViewProj);
    const float eye[4] = {cam.eye.x, cam.eye.y, cam.eye.z, float(t)};  // w = the animation time
    std::memcpy(fc.cameraPos, eye, sizeof eye);
    const float right[4] = {cam.view.m[0], cam.view.m[4], cam.view.m[8], 0.f};  // the view matrix's rows
    const float up[4] = {cam.view.m[1], cam.view.m[5], cam.view.m[9], 0.f};
    std::memcpy(fc.cameraRight, right, sizeof right);
    std::memcpy(fc.cameraUp, up, sizeof up);
    const Vec3 sun = normalize({-0.4f, -1.f, -0.3f});
    const float sunDir[4] = {sun.x, sun.y, sun.z, 0.f}, sunColour[4] = {3.f, 3.f * 0.96f, 3.f * 0.90f, 0.f};
    const float sky[4] = {0.25f, 0.30f, 0.40f, 0.f}, ground[4] = {0.08f, 0.07f, 0.06f, 0.f};
    std::memcpy(fc.sunDir, sunDir, sizeof sunDir);
    std::memcpy(fc.sunColour, sunColour, sizeof sunColour);
    std::memcpy(fc.skyAmbient, sky, sizeof sky);
    std::memcpy(fc.groundAmbient, ground, sizeof ground);
    const float size[4] = {float(width), float(height), 1.f / float(width), 1.f / float(height)};
    std::memcpy(fc.renderSize, size, sizeof size);
    std::memcpy(slot.frameConstants.mapped, &fc, sizeof fc);

    auto *objects = static_cast<std::uint8_t *>(slot.objectConstants.mapped);
    for (std::size_t i = 0; i < w.scene.instances.size(); ++i) {
        ObjectConstants oc{};
        const Mat4 model = instanceModelAt(w.scene, i, t);
        std::memcpy(oc.model, model.m, sizeof oc.model);
        const Vec4 &c = w.scene.instances[i].colour;
        const float colour[4] = {c.x, c.y, c.z, c.w};
        std::memcpy(oc.colour, colour, sizeof colour);
        std::memcpy(objects + i * kObjectSlot, &oc, sizeof oc);
    }

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
    std::memcpy(colour.clearValue.color.float32, kBackground, sizeof kBackground);
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

} // namespace sb::gpu::vk

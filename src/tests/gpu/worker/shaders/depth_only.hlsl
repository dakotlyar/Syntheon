// SPDX-License-Identifier: GPL-3.0-or-later
// Depth only (Phase 4a design §6.3): the Shading scene's shadow pass into the
// 2048^2 D32F shadow map and its depth pre-pass into sceneDepth, from one
// vertex shader. build.py compiles VSMain/PSMain only, so the shadow variant
// (the design's VSShadow) is VSMain with DepthPass.depthPass.x = 1: lightViewProj,
// plus the constant part of the depth bias, 1.5 x 2^-24 added to clip z (x w),
// identical on every API; the slope part is the rasteriser's (2.0).
// No fragment shader runs on Vulkan (the pipeline has none); PSMain exists
// for the D3D builds.
#include "common.hlsli"

SB_BIND_OBJECT cbuffer Object : register(b1)
{
    float4x4 model;
    float4 colour;
};

SB_BIND_PASS cbuffer DepthPass : register(b2)
{
    uint4 depthPass;  // x: 1 = the shadow pass (lightViewProj + bias), 0 = the pre-pass (viewProj)
};

struct VSIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    // precise: the pre-pass must give the shading pass's depth bit for bit (EQUAL test).
    precise float4 pos : SV_Position;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    precise float4 world = mul(model, float4(i.position, 1.0));
    if (depthPass.x != 0) {
        precise float4 p = mul(lightViewProj, world);
        p.z += 1.5 * (1.0 / 16777216.0) * p.w;
        o.pos = p;
    } else {
        o.pos = mul(viewProj, world);
    }
    return o;
}

void PSMain(VSOut i)
{
}

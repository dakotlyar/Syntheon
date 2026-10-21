// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by the Particles scene's post passes (Phase 4a design §6.4): the
// pass constants, the source sampling and the 13-tap downsample. Each pass
// draws present.hlsl's full-screen triangle (Vulkan uses present's vertex
// shader; VSMain here is the same, for the D3D builds).
#ifndef SB_GPU_POST_HLSLI
#define SB_GPU_POST_HLSLI

#include "common.hlsli"

SB_BIND_PASS cbuffer Post : register(b2)
{
    float4 sourceSize;  // t0: 1/width, 1/height, width, height
    float4 params;      // per pass (see each shader)
    uint4 postFlags;    // x = flipV (OpenGL: v flipped where a render target is sampled)
};

SB_BIND_S1 SamplerState linearClamp : register(s1);

FullscreenOut VSMain(uint id : SV_VertexID)
{
    return fullscreenTriangle(id);
}

// The pixel counted from the top-left on every API (OpenGL: mirrored by flipV).
uint2 inputPixel(FullscreenOut i)
{
    uint2 p = uint2(i.pos.xy);
    if (postFlags.x != 0)
        p.y = uint(renderSize.y) - 1u - p.y;
    return p;
}

float2 sourceUv(float2 uv)
{
    return postFlags.x != 0 ? float2(uv.x, 1.0 - uv.y) : uv;
}

// Jimenez's 13-tap downsample (Call of Duty: AW, SIGGRAPH 2014), at uv on a
// source of texel size `texel`; offsets in source texels.
float3 downsample13(Texture2D<float4> src, float2 uv, float2 texel)
{
    float3 a = src.SampleLevel(linearClamp, uv + texel * float2(-2.0, -2.0), 0.0).rgb;
    float3 b = src.SampleLevel(linearClamp, uv + texel * float2(0.0, -2.0), 0.0).rgb;
    float3 c = src.SampleLevel(linearClamp, uv + texel * float2(2.0, -2.0), 0.0).rgb;
    float3 d = src.SampleLevel(linearClamp, uv + texel * float2(-2.0, 0.0), 0.0).rgb;
    float3 e = src.SampleLevel(linearClamp, uv, 0.0).rgb;
    float3 f = src.SampleLevel(linearClamp, uv + texel * float2(2.0, 0.0), 0.0).rgb;
    float3 g = src.SampleLevel(linearClamp, uv + texel * float2(-2.0, 2.0), 0.0).rgb;
    float3 h = src.SampleLevel(linearClamp, uv + texel * float2(0.0, 2.0), 0.0).rgb;
    float3 i = src.SampleLevel(linearClamp, uv + texel * float2(2.0, 2.0), 0.0).rgb;
    float3 j = src.SampleLevel(linearClamp, uv + texel * float2(-1.0, -1.0), 0.0).rgb;
    float3 k = src.SampleLevel(linearClamp, uv + texel * float2(1.0, -1.0), 0.0).rgb;
    float3 l = src.SampleLevel(linearClamp, uv + texel * float2(-1.0, 1.0), 0.0).rgb;
    float3 m = src.SampleLevel(linearClamp, uv + texel * float2(1.0, 1.0), 0.0).rgb;
    return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}

#endif // SB_GPU_POST_HLSLI

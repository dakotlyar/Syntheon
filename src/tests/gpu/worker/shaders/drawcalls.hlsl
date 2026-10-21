// SPDX-License-Identifier: GPL-3.0-or-later
// Draw calls (Phase 4a design §6.2): 12,000 small objects, a draw each, with
// the Geometry lighting times a 64x64 material texture (t0, s0). The present
// pass tone maps the HDR result.
#include "lighting.hlsli"

SB_BIND_OBJECT cbuffer Object : register(b1)
{
    float4x4 model;
    float4 colour;  // rgb tint
};

SB_BIND_T0 Texture2D<float4> material : register(t0);
SB_BIND_S0 SamplerState linearWrap : register(s0);

struct VSIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 world = mul(model, float4(i.position, 1.0));
    o.worldPos = world.xyz;
    o.normal = mul((float3x3)model, i.normal);
    o.uv = i.uv;
    o.pos = mul(viewProj, world);
    return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
    float3 albedo = colour.rgb * material.Sample(linearWrap, i.uv).rgb;
    return float4(sunLighting(albedo, i.normal, i.worldPos), 1.0);
}

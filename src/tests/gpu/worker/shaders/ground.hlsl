// SPDX-License-Identifier: GPL-3.0-or-later
// The Particles scene's ground (Phase 4a design §6.4): the Geometry lighting
// (lighting.hlsli) with the albedo of its Object colour, then the height fog
// by the fragment's own position (fog.hlsli). Kept apart from geometry.hlsl so
// the Geometry scene's shader stays fog-free.
#include "lighting.hlsli"
#include "fog.hlsli"

SB_BIND_OBJECT cbuffer Object : register(b1)
{
    float4x4 model;
    float4 colour;  // rgb albedo
};

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
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 world = mul(model, float4(i.position, 1.0));
    o.worldPos = world.xyz;
    o.normal = mul((float3x3)model, i.normal);
    o.pos = mul(viewProj, world);
    return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
    float3 c = sunLighting(colour.rgb, i.normal, i.worldPos);
    return float4(lerp(c, kFogColour, heightFog(cameraPos.xyz, i.worldPos)), 1.0);
}

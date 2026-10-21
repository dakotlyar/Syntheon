// SPDX-License-Identifier: GPL-3.0-or-later
// Geometry (Phase 4a design §6.1): 36 spinning displaced icospheres, one
// indexed draw each, with the sun and hemisphere lighting and no textures.
// The present pass tone maps the HDR result.
#include "lighting.hlsli"

// One 256-byte slot per draw (a dynamic uniform buffer offset); only the
// first 80 bytes are read. Scaling must be uniform (the normal uses model's
// upper 3x3).
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
    return float4(sunLighting(colour.rgb, i.normal, i.worldPos), 1.0);
}

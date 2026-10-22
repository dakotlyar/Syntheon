// SPDX-License-Identifier: GPL-3.0-or-later
// Sprite batching, the fill-rate workload (Phase 4a design §6.6): 24 x 24
// logical px sprites (x the render scale) from a 256 x 256 atlas of 4 x 4
// cells of 64 x 64 shapes, alpha blended into ldrA. One instance per sprite,
// 16 bytes from the CPU every frame: {float x, y; uint rotation16 | atlasIndex
// << 16; uint rgba8} (x, y in 1920 x 1080 logical px, rgba8 = R | G << 8 |
// B << 16 | A << 24). Six vertices per instance from SV_VertexID.
#include "common.hlsli"

SB_BIND_T0 Texture2D<float4> atlas : register(t0);
SB_BIND_S1 SamplerState linearClamp : register(s1);

struct VSIn
{
    float2 pos : TEXCOORD0;
    uint packed : TEXCOORD1;
    uint colour : TEXCOORD2;
    uint vertex : SV_VertexID;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 colour : TEXCOORD1;
};

VSOut VSMain(VSIn i)
{
    static const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    const float2 c = corners[i.vertex];
    const float angle = float(i.packed & 0xFFFFu) * (6.28318530718 / 65536.0);
    const float cs = cos(angle), sn = sin(angle);
    const float2 local = (c - 0.5) * 24.0;  // logical px around the sprite's centre
    const float2 px = i.pos + float2(local.x * cs - local.y * sn, local.x * sn + local.y * cs);
    const uint cell = (i.packed >> 16) & 15u;
    VSOut o;
    o.pos = float4(px.x / 960.0 - 1.0, 1.0 - px.y / 540.0, 0.0, 1.0);  // the canvas is 1920 x 1080 logical px, any render size
    o.uv = (float2(float(cell & 3u), float(cell >> 2)) + c) * 0.25;
    o.colour = float4(float(i.colour & 255u), float((i.colour >> 8) & 255u), float((i.colour >> 16) & 255u), float(i.colour >> 24)) * (1.0 / 255.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
    const float4 texel = atlas.Sample(linearClamp, i.uv);
    return float4(texel.rgb * i.colour.rgb, texel.a * i.colour.a);
}

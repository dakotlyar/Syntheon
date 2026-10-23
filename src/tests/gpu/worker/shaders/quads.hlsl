// SPDX-License-Identifier: GPL-3.0-or-later
// Textured quads, the fill-rate workload (Phase 4a design §6.5): N full-screen
// layers as ONE instanced draw (N instances x 6 vertices, no vertex data).
// Layer i samples the 256 x 256 fill texture (no mips, linear, repeat: s3,
// the plain bilinear wrap sampler, checkpoint 5 L5) at
// uv * (2 + i mod 4) + offset_i(t), alpha x 1/16, blended SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA into ldrA. offset_i(t) is defined here (task 29):
//   offset_i(t) = 0.37 * (sin(0.6 t + 0.83 i), cos(0.5 t + 1.17 i))
// with t = Frame.cameraPos.w, so the layers drift past one another.
#include "common.hlsli"

SB_BIND_T0 Texture2D<float4> fillTexture : register(t0);
SB_BIND_S3 SamplerState bilinearRepeat : register(s3);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertex : SV_VertexID, uint layer : SV_InstanceID)
{
    // Two triangles over the whole target; c is (0, 0) at the top-left (D3D/Vulkan conventions).
    static const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    const float2 c = corners[vertex];
    const float t = cameraPos.w;
    const float k = float(layer);
    const float2 offset = 0.37 * float2(sin(0.6 * t + 0.83 * k), cos(0.5 * t + 1.17 * k));
    VSOut o;
    o.pos = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
    o.uv = c * float(2 + layer % 4) + offset;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
    const float4 colour = fillTexture.Sample(bilinearRepeat, i.uv);
    return float4(colour.rgb, colour.a * (1.0 / 16.0));
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Post passes 7-11 (Phase 4a design §6.4): the 3x3 tent upsample of bloom[k]
// (radius params.x = 1 source texel), blended additively (ONE, ONE) into the
// next larger mip bloom[k-1].
#include "post.hlsli"

SB_BIND_T0 Texture2D<float4> source : register(t0);

float4 PSMain(FullscreenOut i) : SV_Target0
{
    float2 uv = sourceUv(i.uv);
    float2 d = sourceSize.xy * params.x;
    float3 s = source.SampleLevel(linearClamp, uv + float2(-d.x, -d.y), 0.0).rgb;
    s += source.SampleLevel(linearClamp, uv + float2(0.0, -d.y), 0.0).rgb * 2.0;
    s += source.SampleLevel(linearClamp, uv + float2(d.x, -d.y), 0.0).rgb;
    s += source.SampleLevel(linearClamp, uv + float2(-d.x, 0.0), 0.0).rgb * 2.0;
    s += source.SampleLevel(linearClamp, uv, 0.0).rgb * 4.0;
    s += source.SampleLevel(linearClamp, uv + float2(d.x, 0.0), 0.0).rgb * 2.0;
    s += source.SampleLevel(linearClamp, uv + float2(-d.x, d.y), 0.0).rgb;
    s += source.SampleLevel(linearClamp, uv + float2(0.0, d.y), 0.0).rgb * 2.0;
    s += source.SampleLevel(linearClamp, uv + float2(d.x, d.y), 0.0).rgb;
    return float4(s * (1.0 / 16.0), 1.0);
}

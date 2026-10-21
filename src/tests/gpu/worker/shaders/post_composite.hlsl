// SPDX-License-Identifier: GPL-3.0-or-later
// Post pass 12 (Phase 4a design §6.4): sceneColor + params.x (0.04) x
// bloom[0] -> hdrB. The height fog is no longer applied here (D2, Zeb's
// window checks): particles write no depth, so a depth-based fog in post fogged
// them with the sky's far depth. Each surface fogs itself (fog.hlsli).
#include "post.hlsli"

SB_BIND_T0 Texture2D<float4> scene : register(t0);
SB_BIND_T1 Texture2D<float4> bloom : register(t1);

float4 PSMain(FullscreenOut i) : SV_Target0
{
    float2 uv = sourceUv(i.uv);
    float3 c = scene.SampleLevel(linearClamp, uv, 0.0).rgb + params.x * bloom.SampleLevel(linearClamp, uv, 0.0).rgb;
    return float4(c, 1.0);
}

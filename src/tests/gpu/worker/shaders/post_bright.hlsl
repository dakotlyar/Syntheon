// SPDX-License-Identifier: GPL-3.0-or-later
// Post pass 1 (Phase 4a design §6.4): the bright pass (threshold params.x =
// 1.0, soft knee params.y = 0.5) with the 13-tap downsample, sceneColor ->
// bloom[0] (half size).
#include "post.hlsli"

SB_BIND_T0 Texture2D<float4> source : register(t0);

float4 PSMain(FullscreenOut i) : SV_Target0
{
    float3 c = downsample13(source, sourceUv(i.uv), sourceSize.xy);
    float threshold = params.x, knee = params.y;
    float brightness = max(c.r, max(c.g, c.b));
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contribution = max(soft, brightness - threshold) / max(brightness, 1e-5);
    return float4(c * contribution, 1.0);
}

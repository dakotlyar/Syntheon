// SPDX-License-Identifier: GPL-3.0-or-later
// Post passes 2-6 (Phase 4a design §6.4): the 13-tap downsample, bloom[k-1] -> bloom[k].
#include "post.hlsli"

SB_BIND_T0 Texture2D<float4> source : register(t0);

float4 PSMain(FullscreenOut i) : SV_Target0
{
    return float4(downsample13(source, sourceUv(i.uv), sourceSize.xy), 1.0);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Post pass 13 (Phase 4a design §6.4): ACES fitted tone map, a vignette,
// film grain (an integer hash of the pixel and the frame time, amplitude
// params.x) and the sRGB encoding, hdrB -> ldrA.
#include "post.hlsli"

SB_BIND_T0 Texture2D<float4> source : register(t0);

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float4 PSMain(FullscreenOut i) : SV_Target0
{
    float2 uv = sourceUv(i.uv);
    float3 c = acesFitted(source.SampleLevel(linearClamp, uv, 0.0).rgb);
    float r = saturate(length(i.uv - 0.5) * 1.41421356);
    c *= 1.0 - 0.35 * r * r;
    uint2 p = inputPixel(i);
    uint frame = uint(cameraPos.w * 1000.0);
    float g = float(hash(p.x + hash(p.y + hash(frame))) & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
    c = saturate(c * (1.0 + params.x * g));
    return float4(srgbEncode(c), 1.0);
}

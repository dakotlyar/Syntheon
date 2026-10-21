// SPDX-License-Identifier: GPL-3.0-or-later
// The sun-and-hemisphere lighting of the Geometry and Draw calls scenes
// (Phase 4a design §6.1, §6.2): Lambert plus Blinn-Phong (power 32, strength
// 0.25) from one directional light, and a hemisphere ambient. Linear HDR out;
// the present pass tone maps. SM 5.0 compatible.
#ifndef SB_GPU_LIGHTING_HLSLI
#define SB_GPU_LIGHTING_HLSLI

#include "common.hlsli"

float3 sunLighting(float3 albedo, float3 normal, float3 worldPos)
{
    float3 n = normalize(normal);
    float3 l = -sunDir.xyz;  // sunDir is the direction the light travels
    float3 v = normalize(cameraPos.xyz - worldPos);
    float3 h = normalize(l + v);
    float ndotl = saturate(dot(n, l));
    float3 ambient = lerp(groundAmbient.rgb, skyAmbient.rgb, n.y * 0.5 + 0.5);
    float spec = pow(saturate(dot(n, h)), 32.0) * 0.25 * step(0.0, ndotl);
    return albedo * (ambient + sunColour.rgb * ndotl) + sunColour.rgb * spec * ndotl;
}

#endif // SB_GPU_LIGHTING_HLSLI

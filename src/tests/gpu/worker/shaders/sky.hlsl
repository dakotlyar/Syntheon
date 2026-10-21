// SPDX-License-Identifier: GPL-3.0-or-later
// The Particles scene's sky (Phase 4a design §6.4): one full-screen triangle
// at the far plane (depth 1), an HDR gradient from the horizon (0.30, 0.35,
// 0.45) to the zenith (0.05, 0.10, 0.25) by the view ray's height.
#include "common.hlsli"

// The view ray through a full-screen uv (60 degrees vertical, the scenes' projection).
float3 viewRay(float2 uv)
{
    float3 right = cameraRight.xyz, up = cameraUp.xyz;
    float3 forward = cross(up, right);
    float tanHalf = 0.57735027;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float aspect = renderSize.x * renderSize.w;
    return normalize(forward + right * (ndc.x * tanHalf * aspect) + up * (ndc.y * tanHalf));
}

FullscreenOut VSMain(uint id : SV_VertexID)
{
    FullscreenOut o = fullscreenTriangle(id);
    o.pos.z = o.pos.w;  // the far plane
    return o;
}

float4 PSMain(FullscreenOut i) : SV_Target0
{
    float h = saturate(viewRay(i.uv).y);
    return float4(lerp(float3(0.30, 0.35, 0.45), float3(0.05, 0.10, 0.25), h), 1.0);
}

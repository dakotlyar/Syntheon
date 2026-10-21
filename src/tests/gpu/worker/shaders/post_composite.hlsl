// SPDX-License-Identifier: GPL-3.0-or-later
// Post pass 12 (Phase 4a design §6.4): sceneColor + params.x (0.04) x
// bloom[0], then exponential height fog (density params.y, height falloff
// params.z, the horizon colour) along the view ray to the depth buffer's
// point, -> hdrB.
#include "post.hlsli"

SB_BIND_T0 Texture2D<float4> scene : register(t0);
SB_BIND_T1 Texture2D<float4> bloom : register(t1);
SB_BIND_T2 Texture2D<float> depth : register(t2);

float4 PSMain(FullscreenOut i) : SV_Target0
{
    float2 uv = sourceUv(i.uv);
    float3 c = scene.SampleLevel(linearClamp, uv, 0.0).rgb + params.x * bloom.SampleLevel(linearClamp, uv, 0.0).rgb;

    // The view ray (60 degrees vertical) and the distance to the depth buffer's
    // point: view depth = B / (ndc + A) for the scenes' RH 0..1 projection
    // (near 0.1, far 200).
    float3 right = cameraRight.xyz, up = cameraUp.xyz, forward = cross(up, right);
    float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
    float tanHalf = 0.57735027, aspect = renderSize.x * renderSize.w;
    float3 ray = forward + right * (ndc.x * tanHalf * aspect) + up * (ndc.y * tanHalf);
    float n = 0.1, f = 200.0;
    float a = f / (n - f), b = f * n / (n - f);
    // The depth at this fragment (Load: a depth format needn't filter); the target and the
    // depth buffer have the same size and orientation on every API, so no flipV here.
    int2 p = int2(i.pos.xy);
    float viewDepth = b / (depth.Load(int3(p, 0)) + a);
    float3 dir = normalize(ray);
    float dist = viewDepth * length(ray);  // ray's forward component is 1

    // Exponential height fog: density params.y x exp(-params.z x height), integrated along the ray.
    float falloff = params.z;
    float dy = dir.y * dist * falloff;
    float integral = params.y * exp(-falloff * cameraPos.y) * dist * (abs(dy) > 1e-4 ? (1.0 - exp(-dy)) / dy : 1.0);
    float fog = 1.0 - exp(-integral);
    c = lerp(c, float3(0.30, 0.35, 0.45), saturate(fog));
    return float4(c, 1.0);
}

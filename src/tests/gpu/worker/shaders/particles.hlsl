// SPDX-License-Identifier: GPL-3.0-or-later
// Particles (Phase 4a design §6.4): one fountain of camera-facing billboards,
// one instanced draw (6 vertices per particle), motion in closed form here (no
// simulation): tau = mod(t + phase, life), position = v dir tau + g tau^2 / 2.
// Additive (ONE, ONE) HDR colour HSV(hue, 0.9, 1) x 4 x (1 - tau / life),
// times the soft disc (t0), times the height fog's transmittance to the
// particle's centre (fog.hlsli; D2: the post chain doesn't fog particles,
// whose pixels hold the sky's depth). A particle below the ground has size 0.
#include "fog.hlsli"

SB_BIND_T0 Texture2D<float4> disc : register(t0);
SB_BIND_S1 SamplerState linearClamp : register(s1);

struct VSIn
{
    float4 a : TEXCOORD0;  // phase, life, speed, azimuth
    float4 b : TEXCOORD1;  // elevation, size, hue, pad
    uint vertex : SV_VertexID;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 colour : TEXCOORD1;
};

float3 hsvToRgb(float h, float s, float v)
{
    float3 k = saturate(abs(frac(h + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return v * lerp(float3(1.0, 1.0, 1.0), k, s);
}

VSOut VSMain(VSIn i)
{
    float life = i.a.y;
    float tau = fmod(cameraPos.w + i.a.x, life);
    float el = i.b.x, az = i.a.w;
    float3 dir = float3(cos(el) * cos(az), sin(el), cos(el) * sin(az));
    float3 p = dir * (i.a.z * tau) + float3(0.0, -9.81, 0.0) * (0.5 * tau * tau);
    float size = p.y < 0.0 ? 0.0 : i.b.y;
    // Two triangles: corners (-1,-1) (1,-1) (1,1), (-1,-1) (1,1) (-1,1).
    uint c = i.vertex % 6;
    float2 corner = float2((c == 1 || c == 2 || c == 4) ? 1.0 : -1.0, (c == 2 || c == 4 || c == 5) ? 1.0 : -1.0);
    float3 world = p + (cameraRight.xyz * corner.x + cameraUp.xyz * corner.y) * size;
    VSOut o;
    o.pos = mul(viewProj, float4(world, 1.0));
    o.uv = corner * float2(0.5, -0.5) + 0.5;
    o.colour = hsvToRgb(i.b.z, 0.9, 1.0) * (4.0 * (1.0 - tau / life)) * (1.0 - heightFog(cameraPos.xyz, p));
    return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
    return float4(i.colour * disc.SampleLevel(linearClamp, i.uv, 0.0).a, 1.0);
}

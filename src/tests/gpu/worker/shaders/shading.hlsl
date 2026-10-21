// SPDX-License-Identifier: GPL-3.0-or-later
// Shading (Phase 4a design §6.3): the room and its 64 objects with four
// 1024^2 textures (albedo, normal, roughness/metal, AO; trilinear, 8x
// anisotropic), the sun with a 2048^2 shadow map (3x3 taps of a comparison
// sampler) and 128 point lights evaluated brute force by every pixel. GGX,
// Smith-Schlick, Schlick Fresnel. Depth test EQUAL against the pre-pass, so
// each covered pixel is shaded once. Linear HDR out; the present pass tone maps.
#include "common.hlsli"

SB_BIND_OBJECT cbuffer Object : register(b1)
{
    float4x4 model;
    float4 colour;  // rgb tint, w = the UV scale
};

// 128 lights x 32 bytes = 4 KiB; the count in use is sunDir.w (content scale).
SB_BIND_PASS cbuffer Lights : register(b2)
{
    float4 lightPosRadius[128];        // xyz position at t, w radius
    float4 lightColourIntensity[128];  // rgb colour, w intensity
};

SB_BIND_T0 Texture2D<float4> albedoMap : register(t0);
SB_BIND_T1 Texture2D<float4> normalMap : register(t1);
SB_BIND_T2 Texture2D<float4> roughMetalMap : register(t2);
SB_BIND_T3 Texture2D<float4> aoMap : register(t3);
SB_BIND_SHADOW Texture2D<float> shadowMap : register(t4);
SB_BIND_S0 SamplerState linearWrap : register(s0);
SB_BIND_S2 SamplerComparisonState shadowCompare : register(s2);

struct VSIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    precise float4 pos : SV_Position;  // the same maths as depth_only.hlsl's pre-pass
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    precise float4 world = mul(model, float4(i.position, 1.0));
    o.worldPos = world.xyz;
    o.normal = mul((float3x3)model, i.normal);
    o.uv = i.uv * colour.w;
    o.pos = mul(viewProj, world);
    return o;
}

static const float kPi = 3.14159265;

float ggx(float ndoth, float a)
{
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float smithSchlick(float ndotv, float ndotl, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return (ndotv / (ndotv * (1.0 - k) + k)) * (ndotl / (ndotl * (1.0 - k) + k));
}

float3 schlick(float3 f0, float vdoth)
{
    float f = 1.0 - vdoth;
    float f2 = f * f;
    return f0 + (1.0 - f0) * (f2 * f2 * f);
}

// One light's GGX / Smith-Schlick / Schlick contribution, Lambert diffuse x (1 - F)(1 - metal).
float3 brdf(float3 n, float3 v, float3 l, float3 albedo, float roughness, float metal)
{
    float ndotl = saturate(dot(n, l));
    float ndotv = max(dot(n, v), 1e-4);
    float3 h = normalize(l + v);
    float ndoth = saturate(dot(n, h));
    float vdoth = saturate(dot(v, h));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 f = schlick(f0, vdoth);
    float a = roughness * roughness;
    float3 spec = ggx(ndoth, a) * smithSchlick(ndotv, ndotl, roughness) * f / max(4.0 * ndotv * ndotl, 1e-4);
    float3 diffuse = albedo / kPi * (1.0 - f) * (1.0 - metal);
    return (diffuse + spec) * ndotl;
}

// The sun's visibility: 3x3 taps of the comparison sampler (each a 2x2 PCF in hardware).
float sunShadow(float3 worldPos)
{
    float4 p = mul(lightViewProj, float4(worldPos, 1.0));
    float3 c = p.xyz / p.w;
    float2 uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    float texel = 1.0 / 2048.0;
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x)
            lit += shadowMap.SampleCmpLevelZero(shadowCompare, uv + float2(x, y) * texel, c.z);
    }
    return lit / 9.0;
}

float4 PSMain(VSOut i) : SV_Target0
{
    float3 albedo = albedoMap.Sample(linearWrap, i.uv).rgb * colour.rgb;
    float2 rm = roughMetalMap.Sample(linearWrap, i.uv).rg;
    float roughness = max(rm.r, 0.05);
    float metal = rm.g;
    float ao = aoMap.Sample(linearWrap, i.uv).r;

    // The normal map in a tangent frame from the derivatives (the meshes carry no tangents).
    float3 n0 = normalize(i.normal);
    float3 dpx = ddx(i.worldPos), dpy = ddy(i.worldPos);
    float2 dux = ddx(i.uv), duy = ddy(i.uv);
    float3 t = dpx * duy.y - dpy * dux.y;
    float3 b = dpy * dux.x - dpx * duy.x;
    float invMax = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    float3 tn = normalMap.Sample(linearWrap, i.uv).xyz * 2.0 - 1.0;
    float3 n = normalize(t * invMax * tn.x + b * invMax * tn.y + n0 * tn.z);

    float3 v = normalize(cameraPos.xyz - i.worldPos);
    float3 colourOut = albedo * ao * lerp(groundAmbient.rgb, skyAmbient.rgb, n.y * 0.5 + 0.5);
    colourOut += brdf(n, v, -sunDir.xyz, albedo, roughness, metal) * sunColour.rgb * sunShadow(i.worldPos);
    uint count = (uint)sunDir.w;
    for (uint k = 0; k < count; ++k) {
        float3 d = lightPosRadius[k].xyz - i.worldPos;
        float dist2 = dot(d, d);
        float dist = sqrt(dist2);
        float q = dist / lightPosRadius[k].w;
        float q2 = q * q;
        float window = saturate(1.0 - q2 * q2);
        float falloff = lightColourIntensity[k].w / (dist2 + 1.0) * window * window;
        colourOut += brdf(n, v, d / max(dist, 1e-4), albedo, roughness, metal) * lightColourIntensity[k].rgb * falloff;
    }
    return float4(colourOut, 1.0);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by every GPU-test shader (Phase 4a design §13.1). One HLSL source
// feeds every API: DXC -spirv (Vulkan, and through SPIRV-Cross GLSL 330 for
// OpenGL), DXC (DXIL, SM 6.0, D3D12) and FXC (DXBC, SM 5.0, D3D11), so this
// stays SM 5.0-compatible (pitfall SH-1): no wave intrinsics, no 16-bit
// types, no [[vk::...]] attributes except the bindings below.
#ifndef SB_GPU_COMMON_HLSLI
#define SB_GPU_COMMON_HLSLI

// The fixed binding table, the same slots in every API:
//
//   slot      HLSL                         Vulkan (set, binding)    GL
//   b0        cbuffer Frame                (0, 0)                   uniform block point 0
//   b1        cbuffer Object               (1, 0) dynamic           point 1 (glBindBufferRange)
//   b2        cbuffer Lights / Post /      (0, 1)                   point 2
//             Present (one per shader)
//   t0..t3    textures                     (2, 0..3)                units 0..3
//   t4        shadow map                   (0, 2)                   unit 4
//   t5        glyph atlas (present)        (0, 3)                   unit 5
//   s0        linear wrap, 8x anisotropic  (0, 4) immutable         sampler object on the texture's unit
//   s1        linear clamp                 (0, 5) immutable         "
//   s2        comparison (shadow)          (0, 6) immutable         "
//
// GLSL 330 has no binding= (pitfall SH-3): build.py turns each separate
// texture + sampler pair into a combined sampler named
// SPIRV_Cross_Combined<texture><sampler> and lists every block and sampler
// with its point or unit in generated/bindings.inc; the GL backend applies
// those after linking.
#if defined(__spirv__)
#define SB_VK_BINDING(b, s) [[vk::binding(b, s)]]
#else
#define SB_VK_BINDING(b, s)
#endif

#define SB_BIND_FRAME   SB_VK_BINDING(0, 0)
#define SB_BIND_OBJECT  SB_VK_BINDING(0, 1)
#define SB_BIND_PASS    SB_VK_BINDING(1, 0)
#define SB_BIND_T0      SB_VK_BINDING(0, 2)
#define SB_BIND_T1      SB_VK_BINDING(1, 2)
#define SB_BIND_T2      SB_VK_BINDING(2, 2)
#define SB_BIND_T3      SB_VK_BINDING(3, 2)
#define SB_BIND_SHADOW  SB_VK_BINDING(2, 0)
#define SB_BIND_ATLAS   SB_VK_BINDING(3, 0)
#define SB_BIND_S0      SB_VK_BINDING(4, 0)
#define SB_BIND_S1      SB_VK_BINDING(5, 0)
#define SB_BIND_S2      SB_VK_BINDING(6, 0)

// Per-frame constants (b0). Matrices are column-major (math.h's float[16])
// and are applied as mul(M, v). Every member is 16-byte aligned with
// explicit padding (pitfall SH-2: std140 packs a float after a float3
// differently). The compilers drop it from a shader that doesn't use it.
SB_BIND_FRAME cbuffer Frame : register(b0)
{
    float4x4 viewProj;       // world -> clip (D3D conventions, depth 0..1)
    float4x4 lightViewProj;  // world -> the shadow map's clip space (Shading)
    float4 cameraPos;        // xyz; w = animation time t, in seconds
    float4 cameraRight;      // xyz (billboards); w unused
    float4 cameraUp;         // xyz (billboards); w unused
    float4 sunDir;           // xyz = the direction the light travels (normalised); w = Shading's point-light count
    float4 sunColour;        // rgb, intensity included; w unused
    float4 skyAmbient;       // rgb; w unused
    float4 groundAmbient;    // rgb; w unused
    float4 renderSize;       // width, height, 1/width, 1/height (physical px)
};

// The full-screen triangle: no vertex buffer, three vertices from
// SV_VertexID. uv (0,0) is the top-left of the screen in D3D/Vulkan
// conventions; OpenGL flips v where it samples (the flipV constants).
struct FullscreenOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

FullscreenOut fullscreenTriangle(uint id)
{
    FullscreenOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// ACES filmic tone map, the fitted RRT + ODT of Stephen Hill (BakingLab,
// MIT), exposure 1.0. Linear scene-referred in, linear display-referred out.
float3 rrtAndOdtFit(float3 v)
{
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 acesFitted(float3 colour)
{
    // sRGB -> ACES AP1 with the RRT's saturation (rows of the matrix).
    const float3x3 acesInput = float3x3(
        0.59719, 0.35458, 0.04823,
        0.07600, 0.90834, 0.01566,
        0.02840, 0.13383, 0.83777);
    // ODT AP1 -> sRGB.
    const float3x3 acesOutput = float3x3(
         1.60475, -0.53108, -0.07367,
        -0.10208,  1.10813, -0.00605,
        -0.00327, -0.07276,  1.07602);
    colour = mul(acesInput, colour);
    colour = rrtAndOdtFit(colour);
    colour = mul(acesOutput, colour);
    return saturate(colour);
}

// Linear -> sRGB transfer function (IEC 61966-2-1). Every swapchain and
// LDR target is UNORM, so the encoding is the shader's on every API.
float3 srgbEncode(float3 linearColour)
{
    float3 c = saturate(linearColour);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r,
                  c.g <= 0.0031308 ? lo.g : hi.g,
                  c.b <= 0.0031308 ? lo.b : hi.b);
}

#endif // SB_GPU_COMMON_HLSLI

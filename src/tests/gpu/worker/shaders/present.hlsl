// SPDX-License-Identifier: GPL-3.0-or-later
// The present pass (Phase 4a design P3, §5.3, §5.9): one full-screen
// triangle that samples the workload's output (t0, linear clamp) into the
// swapchain image at window size, either tone mapped (mode 0, TonemapAces:
// sceneColor, ACES fitted, exposure 1.0, sRGB encode) or copied (mode 1,
// Copy: ldrA/ldrB, already display-encoded); then the FPS and scene overlay
// from the 5x7 glyph atlas (t5, read with Load: no filtering).
#include "common.hlsli"

SB_BIND_PASS cbuffer Present : register(b2)
{
    uint mode;          // 0 = TonemapAces, 1 = Copy
    uint flipV;         // 1 on OpenGL: sample v flipped, and mirror the overlay rows (below)
    int overlayScale;   // k, the size of one font pixel in window pixels
    int overlayChars;   // 0 = no overlay
    // The box (x, y, w, h) in window physical pixels, in the API's own
    // fragment coordinates: y counts down from the top on D3D and Vulkan.
    // With flipV (OpenGL, y counts up from the bottom) y is the box's bottom
    // edge, windowHeight - top - h, and the rows are mirrored inside the box;
    // that is the same pixel as §5.9's y = windowHeight - 1 - fragCoord.y.
    int4 overlayBox;
    uint4 overlayText[3];  // 4 ASCII characters per uint, little-endian, 0 after the end
};

SB_BIND_T0 Texture2D<float4> sourceImage : register(t0);
SB_BIND_ATLAS Texture2D<float4> glyphAtlas : register(t5);
SB_BIND_S1 SamplerState linearClamp : register(s1);

FullscreenOut VSMain(uint id : SV_VertexID)
{
    return fullscreenTriangle(id);
}

// The overlay's own colour for this pixel, or alpha 0 outside the box:
// the box is black at 60% opacity, ink is white and opaque.
float4 overlayAt(int2 pixel)
{
    int lx = pixel.x - overlayBox.x;
    int ly = pixel.y - overlayBox.y;
    if (overlayChars <= 0 || lx < 0 || ly < 0 || lx >= overlayBox.z || ly >= overlayBox.w)
        return float4(0.0, 0.0, 0.0, 0.0);
    if (flipV != 0)
        ly = overlayBox.w - 1 - ly;
    int k = max(overlayScale, 1);
    int pad = 3 * k;
    int fx = lx - pad;
    int fy = ly - pad;
    if (fx >= 0 && fy >= 0) {
        fx /= k;
        fy /= k;
        int c = fx / 6;
        int gx = fx - c * 6;
        if (c < overlayChars && gx < 5 && fy < 7) {
            uint word = overlayText[c >> 4][(c >> 2) & 3];
            uint code = (word >> (8 * (c & 3))) & 0xFF;
            if (code >= 32 && code <= 126) {
                int ax = int(code - 32) * 6 + gx;
                if (glyphAtlas.Load(int3(ax, fy, 0)).r > 0.5)
                    return float4(1.0, 1.0, 1.0, 1.0);
            }
        }
    }
    return float4(0.0, 0.0, 0.0, 0.6);
}

float4 PSMain(FullscreenOut input) : SV_Target0
{
    float2 uv = input.uv;
    if (flipV != 0)
        uv.y = 1.0 - uv.y;
    float3 colour = sourceImage.SampleLevel(linearClamp, uv, 0.0).rgb;
    if (mode == 0)
        colour = srgbEncode(acesFitted(colour));
    float4 overlay = overlayAt(int2(input.pos.xy));
    colour = lerp(colour, overlay.rgb, overlay.a);
    return float4(colour, 1.0);
}

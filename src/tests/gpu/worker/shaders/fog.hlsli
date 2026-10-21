// SPDX-License-Identifier: GPL-3.0-or-later
// The Particles scene's exponential height fog (Phase 4a design §6.4; D2,
// Zeb's window checks): applied by each surface that is drawn, by its own
// distance, never by the depth buffer in a post pass. The sky and the ground
// blend toward the fog colour; the additive particles are attenuated by the
// transmittance along their own view ray (the fog's in-scatter is already in
// the background behind them). Density in skyAmbient.w, height falloff in
// groundAmbient.w (0 in the other scenes, which don't include this file).
#ifndef SB_GPU_FOG_HLSLI
#define SB_GPU_FOG_HLSLI

#include "common.hlsli"

static const float3 kFogColour = float3(0.30, 0.35, 0.45);  // the sky's horizon

// The fog amount (0..1) between the eye and p: density x exp(-falloff x height),
// integrated along the ray.
float heightFog(float3 eye, float3 p)
{
    float density = skyAmbient.w, falloff = groundAmbient.w;
    float3 d = p - eye;
    float dist = length(d);
    float dy = d.y * falloff;
    float integral = density * exp(-falloff * eye.y) * dist * (abs(dy) > 1e-4 ? (1.0 - exp(-dy)) / dy : 1.0);
    return saturate(1.0 - exp(-integral));
}

#endif // SB_GPU_FOG_HLSLI

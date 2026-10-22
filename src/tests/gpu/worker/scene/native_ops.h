// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The native 2D op lists (Phase 4a design §7.1, task 25): shapes, text, image
// blits and gradients on a 1920 x 1080 LOGICAL canvas, generated once from
// fixed seeds. Qt raster and Direct2D draw exactly these ops (the paths never
// make content decisions), every frame, under a global translation of
// (8 sin(2 pi t / 4), 8 cos(2 pi t / 4)) logical px (nativeTranslation).
// Qt-free; colours are rgba8 packed as R | G << 8 | B << 16 | A << 24 (the
// sprites' packing), straight (not premultiplied) alpha.

#include "tests/gpu/worker/scene/scenes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sb::gpu {

enum class NativeKind : std::uint8_t { Shapes, Text, Blits, Gradients };
inline constexpr std::array<NativeKind, 4> kNativeKinds = {NativeKind::Shapes, NativeKind::Text, NativeKind::Blits, NativeKind::Gradients};

// "shapes" "text" "blits" "gradients" (the workload ids); the Workload of a kind and back.
const char *nativeId(NativeKind k);
Workload nativeWorkload(NativeKind k);
// nullopt for a non-native workload.
std::optional<NativeKind> nativeKindOf(Workload w);

// The canvas, in logical px, and §7.1's constants.
inline constexpr float kCanvasWidth = 1920.f, kCanvasHeight = 1080.f;
inline constexpr std::uint32_t kShapeOps = 2000, kTextStrings = 200, kTextChars = 40, kBlitOps = 1000, kGradientOps = 1000;
inline constexpr std::uint32_t kGradientDefinitions = 16;

struct ShapeOp {
    std::uint8_t type;       // 0 filled rotated rectangle, 1 filled ellipse, 2 stroked rounded rectangle (2 px, radius 8), 3 filled 10-vertex star (inner radius 0.45 R)
    float cx, cy, w, h;      // centre and size in logical px, 16..160 (a star is square: R = w / 2)
    float rotation;          // degrees, 0..360
    std::uint32_t rgba8;     // alpha 0.4..1.0
};
struct TextOp {
    float x, y;              // the string's left end and baseline, logical px
    std::uint32_t rgba8;     // opaque
    std::string text;        // 40 characters from A-Z a-z 0-9 .,;:!?-()
};
struct BlitOp {
    float cx, cy, scale, rotation, opacity;  // scale 0.25..1.0 of the 256 x 256 image, degrees, 0.5..1.0
};
struct GradientOp {
    std::uint8_t radial;     // 0: a linear-gradient rectangle (4 stops), 1: a radial-gradient ellipse (3 stops)
    float cx, cy, w, h;      // 32..256
    std::uint8_t gradient;   // which of the 16 definitions, 0..15
};

struct NativeOps {
    NativeKind kind = NativeKind::Shapes;
    std::vector<ShapeOp> shapes;
    std::vector<TextOp> texts;
    std::vector<BlitOp> blits;
    std::vector<GradientOp> gradients;
    Texture image;           // Blits: the 256 x 256 RGBA procedural image (blitImage)
    // Gradients: 16 definitions of 4 colour stops (rgba8, opaque); a linear gradient uses all four at
    // 0, 1/3, 2/3, 1, a radial one the first three at 0, 1/2, 1.
    std::array<std::array<std::uint32_t, 4>, 16> gradientStops{};
    std::uint32_t opsPerFrame = 0;  // shapes, blits, gradients: the op count; text: strings x 40 characters
};

// §7.1's counts, divided by contentScale (rounded up, at least 1; text divides the 200 strings).
NativeOps buildNativeOps(NativeKind kind, double contentScale = 1.0);
// FNV-1a 64 over the quantised ops, the image and the stops (golden: golden_hashes.inc).
std::uint64_t nativeHash(const NativeOps &ops);

// The global translation of frame time t, in logical px.
struct NativeShift { float x, y; };
NativeShift nativeTranslation(double t);

} // namespace sb::gpu

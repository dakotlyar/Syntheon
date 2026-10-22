// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/scene/native_ops.h"

#include "tests/gpu/worker/scene/prng.h"
#include "tests/gpu/worker/scene/textures.h"

#include <algorithm>
#include <cmath>

namespace sb::gpu {

namespace {

constexpr std::uint32_t kShapeSeed = 0x53484150u, kTextSeed = 0x54455854u, kBlitSeed = 0x424C4954u, kGradientSeed = 0x47524144u;
constexpr char kTextSet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,;:!?-()";
constexpr std::uint32_t kTextSetSize = sizeof kTextSet - 1;  // 71

// One draw per statement (the prng.h rule).
float uniform(Xoshiro256ss &rng, float lo, float hi)
{
    const std::uint64_t r = rng.next();
    return lo + (hi - lo) * (float(r >> 40) / 16777216.f);
}

std::uint32_t scaled(std::uint32_t n, double contentScale)
{
    const double s = contentScale > 0.0 ? contentScale : 1.0;
    return std::max<std::uint32_t>(1, std::uint32_t(std::ceil(double(n) / s)));
}

std::uint32_t pack(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

std::uint32_t randomColour(Xoshiro256ss &rng, float alphaLo, float alphaHi)
{
    const std::uint32_t r = std::uint32_t(rng.below(256));
    const std::uint32_t g = std::uint32_t(rng.below(256));
    const std::uint32_t b = std::uint32_t(rng.below(256));
    const float a = alphaHi > alphaLo ? uniform(rng, alphaLo, alphaHi) : alphaLo;
    return pack(r, g, b, std::uint32_t(a * 255.f + 0.5f));
}

struct Fnv {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void byte(std::uint8_t b)
    {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            byte(std::uint8_t(v >> (8 * i)));
    }
    void f(float x) { u32(std::uint32_t(std::int32_t(std::lround(double(x) * 4096.0)))); }
};

} // namespace

const char *nativeId(NativeKind k)
{
    switch (k) {
    case NativeKind::Shapes: return "shapes";
    case NativeKind::Text: return "text";
    case NativeKind::Blits: return "blits";
    case NativeKind::Gradients: return "gradients";
    }
    return "shapes";
}

Workload nativeWorkload(NativeKind k)
{
    switch (k) {
    case NativeKind::Shapes: return Workload::Shapes;
    case NativeKind::Text: return Workload::Text;
    case NativeKind::Blits: return Workload::Blits;
    case NativeKind::Gradients: return Workload::Gradients;
    }
    return Workload::Shapes;
}

std::optional<NativeKind> nativeKindOf(Workload w)
{
    switch (w) {
    case Workload::Shapes: return NativeKind::Shapes;
    case Workload::Text: return NativeKind::Text;
    case Workload::Blits: return NativeKind::Blits;
    case Workload::Gradients: return NativeKind::Gradients;
    default: return std::nullopt;
    }
}

NativeShift nativeTranslation(double t)
{
    constexpr double kTwoPi = 6.28318530717958647692;
    return {float(8.0 * std::sin(kTwoPi * t / 4.0)), float(8.0 * std::cos(kTwoPi * t / 4.0))};
}

NativeOps buildNativeOps(NativeKind kind, double contentScale)
{
    NativeOps ops;
    ops.kind = kind;
    switch (kind) {
    case NativeKind::Shapes: {
        Xoshiro256ss rng(kShapeSeed);
        const std::uint32_t n = scaled(kShapeOps, contentScale);
        for (std::uint32_t i = 0; i < n; ++i) {
            ShapeOp s{};
            s.type = std::uint8_t(i % 4);  // rectangle, ellipse, stroked rounded rectangle, star
            s.w = uniform(rng, 16.f, 160.f);
            const float h = uniform(rng, 16.f, 160.f);
            s.h = s.type == 3 ? s.w : h;  // a star is round: one size
            s.cx = uniform(rng, 0.f, kCanvasWidth);
            s.cy = uniform(rng, 0.f, kCanvasHeight);
            s.rotation = uniform(rng, 0.f, 360.f);
            s.rgba8 = randomColour(rng, 0.4f, 1.0f);
            ops.shapes.push_back(s);
        }
        ops.opsPerFrame = n;
        break;
    }
    case NativeKind::Text: {
        Xoshiro256ss rng(kTextSeed);
        const std::uint32_t n = scaled(kTextStrings, contentScale);
        for (std::uint32_t i = 0; i < n; ++i) {
            TextOp t;
            t.x = uniform(rng, 0.f, kCanvasWidth - 320.f);  // a 40-character string at 14 px is about 300 px wide
            t.y = uniform(rng, 16.f, kCanvasHeight - 4.f);
            t.rgba8 = randomColour(rng, 1.0f, 1.0f);
            t.text.reserve(kTextChars);
            for (std::uint32_t c = 0; c < kTextChars; ++c) {
                const std::uint64_t k = rng.below(kTextSetSize);
                t.text.push_back(kTextSet[k]);
            }
            ops.texts.push_back(std::move(t));
        }
        ops.opsPerFrame = n * kTextChars;
        break;
    }
    case NativeKind::Blits: {
        Xoshiro256ss rng(kBlitSeed);
        ops.image = blitImage(kBlitSeed);
        const std::uint32_t n = scaled(kBlitOps, contentScale);
        for (std::uint32_t i = 0; i < n; ++i) {
            BlitOp b{};
            b.cx = uniform(rng, 0.f, kCanvasWidth);
            b.cy = uniform(rng, 0.f, kCanvasHeight);
            b.scale = uniform(rng, 0.25f, 1.0f);
            b.rotation = uniform(rng, 0.f, 360.f);
            b.opacity = uniform(rng, 0.5f, 1.0f);
            ops.blits.push_back(b);
        }
        ops.opsPerFrame = n;
        break;
    }
    case NativeKind::Gradients: {
        Xoshiro256ss rng(kGradientSeed);
        for (std::array<std::uint32_t, 4> &stops : ops.gradientStops) {
            for (std::uint32_t &c : stops)
                c = randomColour(rng, 1.0f, 1.0f);
        }
        const std::uint32_t n = scaled(kGradientOps, contentScale);
        for (std::uint32_t i = 0; i < n; ++i) {
            GradientOp g{};
            g.radial = std::uint8_t(i % 2);  // even: linear rectangle, odd: radial ellipse
            g.w = uniform(rng, 32.f, 256.f);
            g.h = uniform(rng, 32.f, 256.f);
            g.cx = uniform(rng, 0.f, kCanvasWidth);
            g.cy = uniform(rng, 0.f, kCanvasHeight);
            g.gradient = std::uint8_t(rng.below(kGradientDefinitions));
            ops.gradients.push_back(g);
        }
        ops.opsPerFrame = n;
        break;
    }
    }
    return ops;
}

std::uint64_t nativeHash(const NativeOps &ops)
{
    Fnv h;
    h.u32(std::uint32_t(ops.kind));
    h.u32(ops.opsPerFrame);
    h.u32(std::uint32_t(ops.shapes.size()));
    for (const ShapeOp &s : ops.shapes) {
        h.byte(s.type);
        h.f(s.cx); h.f(s.cy); h.f(s.w); h.f(s.h); h.f(s.rotation);
        h.u32(s.rgba8);
    }
    h.u32(std::uint32_t(ops.texts.size()));
    for (const TextOp &t : ops.texts) {
        h.f(t.x); h.f(t.y);
        h.u32(t.rgba8);
        h.u32(std::uint32_t(t.text.size()));
        for (const char c : t.text)
            h.byte(std::uint8_t(c));
    }
    h.u32(std::uint32_t(ops.blits.size()));
    for (const BlitOp &b : ops.blits) {
        h.f(b.cx); h.f(b.cy); h.f(b.scale); h.f(b.rotation); h.f(b.opacity);
    }
    h.u32(std::uint32_t(ops.gradients.size()));
    for (const GradientOp &g : ops.gradients) {
        h.byte(g.radial);
        h.f(g.cx); h.f(g.cy); h.f(g.w); h.f(g.h);
        h.byte(g.gradient);
    }
    h.u32(ops.image.width);
    h.u32(ops.image.height);
    for (const std::uint8_t b : ops.image.rgba8)
        h.byte(b);
    for (const auto &stops : ops.gradientStops) {
        for (const std::uint32_t c : stops)
            h.u32(c);
    }
    return h.h;
}

} // namespace sb::gpu

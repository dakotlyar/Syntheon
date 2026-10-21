// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/scene/textures.h"

#include "tests/gpu/worker/scene/prng.h"

#include <algorithm>
#include <cmath>

namespace sb::gpu {

namespace {

std::uint8_t byteOf(float v)
{
    return std::uint8_t(int(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f));
}

float uniform(Xoshiro256ss &rng, float lo, float hi)
{
    const std::uint64_t r = rng.next();
    return lo + (hi - lo) * (float(r >> 40) / 16777216.f);
}

Texture blank(std::uint32_t w, std::uint32_t h)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.rgba8.assign(std::size_t(w) * h * 4, 0);
    return t;
}

void put(Texture &t, std::uint32_t x, std::uint32_t y, float r, float g, float b, float a = 1.f)
{
    std::uint8_t *p = &t.rgba8[(std::size_t(y) * t.width + x) * 4];
    p[0] = byteOf(r);
    p[1] = byteOf(g);
    p[2] = byteOf(b);
    p[3] = byteOf(a);
}

// 2D fbm in [-1, 1] at pixel (x, y) of a size x size texture, `cells` lattice cells across.
float noise2(std::uint32_t x, std::uint32_t y, std::uint32_t size, float cells, std::uint32_t seed, int octaves)
{
    const float fx = (float(x) + 0.5f) / float(size) * cells, fy = (float(y) + 0.5f) / float(size) * cells;
    return fbm(fx, fy, 0.5f, seed, octaves);
}

} // namespace

Texture noiseAlbedo(std::uint32_t seed, std::uint32_t size)
{
    Xoshiro256ss rng(seed);
    const float h0 = uniform(rng, 0.f, 1.f);
    const float h1 = uniform(rng, 0.f, 1.f);
    const Vec3 c0 = hsv(h0, 0.55f, 0.35f), c1 = hsv(h1, 0.35f, 0.9f);
    Texture t = blank(size, size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float n = noise2(x, y, size, 6.f, seed, 4) * 0.5f + 0.5f;
            put(t, x, y, lerpf(c0.x, c1.x, n), lerpf(c0.y, c1.y, n), lerpf(c0.z, c1.z, n));
        }
    }
    return t;
}

Texture noiseNormal(std::uint32_t seed, std::uint32_t size)
{
    std::vector<float> height(std::size_t(size) * size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x)
            height[std::size_t(y) * size + x] = noise2(x, y, size, 8.f, seed ^ 0x6E6F726Du, 4);
    }
    const auto at = [&](std::int64_t x, std::int64_t y) {
        const std::int64_t n = std::int64_t(size);
        return height[std::size_t(((y % n) + n) % n) * size + std::size_t(((x % n) + n) % n)];
    };
    Texture t = blank(size, size);
    for (std::int64_t y = 0; y < std::int64_t(size); ++y) {
        for (std::int64_t x = 0; x < std::int64_t(size); ++x) {
            const float dx = (at(x + 1, y - 1) + 2.f * at(x + 1, y) + at(x + 1, y + 1)) -
                             (at(x - 1, y - 1) + 2.f * at(x - 1, y) + at(x - 1, y + 1));
            const float dy = (at(x - 1, y + 1) + 2.f * at(x, y + 1) + at(x + 1, y + 1)) -
                             (at(x - 1, y - 1) + 2.f * at(x, y - 1) + at(x + 1, y - 1));
            const Vec3 n = normalize({-dx * 2.f, -dy * 2.f, 1.f});
            put(t, std::uint32_t(x), std::uint32_t(y), n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, n.z * 0.5f + 0.5f);
        }
    }
    return t;
}

Texture roughMetal(std::uint32_t seed, std::uint32_t size)
{
    Texture t = blank(size, size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float rough = 0.2f + 0.7f * (noise2(x, y, size, 5.f, seed ^ 0x726F7567u, 3) * 0.5f + 0.5f);
            const std::uint32_t band = y * 8u / size;  // 8 horizontal bands, every other one metal
            const float metal = (band & 1u) ? 1.f : 0.f;
            put(t, x, y, rough, metal, 0.f);
        }
    }
    return t;
}

Texture aoMap(std::uint32_t seed, std::uint32_t size)
{
    Texture t = blank(size, size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float ao = 0.5f + 0.5f * (noise2(x, y, size, 7.f, seed ^ 0x414F4F4Fu, 3) * 0.5f + 0.5f);
            put(t, x, y, ao, ao, ao);
        }
    }
    return t;
}

Texture checker(std::uint32_t seed, std::uint32_t size)
{
    Xoshiro256ss rng(seed * 2654435761u + 17u);
    const float h0 = uniform(rng, 0.f, 1.f);
    const float h1 = uniform(rng, 0.f, 1.f);
    const Vec3 a = hsv(h0, 0.7f, 0.95f), b = hsv(h1, 0.5f, 0.25f);
    const std::uint32_t cell = std::max(1u, size / 8u);
    const bool stripes = (seed & 1u) != 0;
    Texture t = blank(size, size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::uint32_t cx = x / cell, cy = y / cell;
            const bool on = stripes ? (cx & 1u) != 0 : ((cx + cy) & 1u) != 0;
            const Vec3 c = on ? a : b;
            put(t, x, y, c.x, c.y, c.z);
        }
    }
    return t;
}

Texture softDisc(std::uint32_t size)
{
    Texture t = blank(size, size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (float(x) + 0.5f) / float(size) * 2.f - 1.f, v = (float(y) + 0.5f) / float(size) * 2.f - 1.f;
            const float f = std::max(0.f, 1.f - (u * u + v * v));
            put(t, x, y, 1.f, 1.f, 1.f, f * f * f);
        }
    }
    return t;
}

Texture spriteAtlas(std::uint32_t seed)
{
    Texture t = blank(256, 256);
    for (std::uint32_t cell = 0; cell < 16; ++cell) {
        const std::uint32_t ox = (cell % 4) * 64, oy = (cell / 4) * 64;
        for (std::uint32_t y = 0; y < 64; ++y) {
            for (std::uint32_t x = 0; x < 64; ++x) {
                const float u = (float(x) + 0.5f) / 32.f - 1.f, v = (float(y) + 0.5f) / 32.f - 1.f;
                const float au = std::fabs(u), av = std::fabs(v), r2 = u * u + v * v;
                bool in = false;
                switch (cell) {
                case 0: in = r2 < 0.8f; break;
                case 1: in = std::max(au, av) < 0.75f; break;
                case 2: in = r2 > 0.35f && r2 < 0.8f; break;
                case 3: in = au + av < 1.f; break;
                case 4: in = (au < 0.25f || av < 0.25f) && std::max(au, av) < 0.9f; break;
                case 5: in = v > -0.8f && v < 0.8f && au < (0.8f - v) / 1.6f * 0.9f; break;
                case 6: in = r2 < 0.8f && v > 0.f; break;
                case 7: in = std::max(au, av) < 0.8f && std::max(au, av) > 0.4f; break;
                default: in = r2 < 0.9f && fbm(u * 2.f + 4.f, v * 2.f + 4.f, float(cell), seed, 2) > -0.15f; break;
                }
                if (in)
                    put(t, ox + x, oy + y, 1.f, 1.f, 1.f);
            }
        }
    }
    return t;
}

Texture fillTexture(std::uint32_t seed)
{
    Texture t = blank(256, 256);
    t.mips = false;  // §6.5: the quads texture has no mips (linear filter, repeat)
    for (std::uint32_t y = 0; y < 256; ++y) {
        for (std::uint32_t x = 0; x < 256; ++x) {
            const float n = noise2(x, y, 256, 8.f, seed, 3) * 0.5f + 0.5f;
            const float m = noise2(x, y, 256, 5.f, seed + 99u, 2) * 0.5f + 0.5f;
            put(t, x, y, n, m, 1.f - n * m);
        }
    }
    return t;
}

Texture blitImage(std::uint32_t seed)
{
    Texture t = blank(512, 512);
    for (std::uint32_t y = 0; y < 512; ++y) {
        for (std::uint32_t x = 0; x < 512; ++x) {
            const float n = noise2(x, y, 512, 10.f, seed, 4) * 0.5f + 0.5f;
            put(t, x, y, float(x) / 511.f, float(y) / 511.f, n);
        }
    }
    return t;
}

std::vector<Texture> mipChain(const Texture &level0)
{
    std::vector<Texture> chain{level0};
    while (chain.back().width > 1 || chain.back().height > 1) {
        const Texture &src = chain.back();
        Texture dst;
        dst.width = std::max(1u, src.width / 2);
        dst.height = std::max(1u, src.height / 2);
        dst.mips = src.mips;
        dst.rgba8.resize(std::size_t(dst.width) * dst.height * 4);
        for (std::uint32_t y = 0; y < dst.height; ++y) {
            for (std::uint32_t x = 0; x < dst.width; ++x) {
                const std::uint32_t x0 = std::min(2 * x, src.width - 1), x1 = std::min(2 * x + 1, src.width - 1);
                const std::uint32_t y0 = std::min(2 * y, src.height - 1), y1 = std::min(2 * y + 1, src.height - 1);
                for (std::uint32_t c = 0; c < 4; ++c) {
                    const auto px = [&](std::uint32_t xx, std::uint32_t yy) {
                        return std::uint32_t(src.rgba8[(std::size_t(yy) * src.width + xx) * 4 + c]);
                    };
                    dst.rgba8[(std::size_t(y) * dst.width + x) * 4 + c] =
                        std::uint8_t((px(x0, y0) + px(x1, y0) + px(x0, y1) + px(x1, y1) + 2u) / 4u);
                }
            }
        }
        chain.push_back(std::move(dst));
    }
    return chain;
}

} // namespace sb::gpu

// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU scene content (Phase 4a design §6): mesh and scene counts, the maths
// helpers, the cameras, mip chains and the content hash. No GPU, no window.
#include "tests/gpu/worker/scene/bitmap_font.h"
#include "tests/gpu/worker/scene/golden_hashes.inc"
#include "tests/gpu/worker/scene/mesh_gen.h"
#include "tests/gpu/worker/scene/overlay.h"
#include "tests/gpu/worker/scene/scenes.h"
#include "tests/gpu/worker/scene/textures.h"

#include <QTest>

#include <cmath>

using namespace sb::gpu;

class TestGpuScene : public QObject
{
    Q_OBJECT

private slots:
    void meshCounts()
    {
        const auto check = [](const Mesh &m, std::uint64_t tris, std::size_t verts) {
            QCOMPARE(triangleCount(m), tris);
            if (verts)
                QCOMPARE(m.vertices.size(), verts);
        };
        check(icosphere(0), 20, 12);
        check(icosphere(1), 80, 42);
        check(icosphere(4), 5120, 2562);
        check(icosphere(7), 327680, 163842);
        check(cube(), 12, 0);
        check(octahedron(), 8, 0);
        check(cylinder(24), 96, 0);
        check(cone(24), 48, 0);
        check(torus(16, 8, 1.f, 0.35f), 256, 0);
        check(uvSphere(16, 8), 224, 0);
        check(grid(64, 64, 40.f, 40.f), 8192, 0);
        check(room(), 10752, 0);
    }

    void everyIndexIsInRange()
    {
        for (const Mesh &m : {cube(), octahedron(), cylinder(24), cone(24), torus(16, 8, 1.f, 0.35f), uvSphere(16, 8), room(), icosphere(3)}) {
            for (const std::uint32_t i : m.indices)
                QVERIFY(i < m.vertices.size());
        }
    }

    void displacedStaysNearTheSphere()
    {
        const Mesh m = displaced(icosphere(4), 0x47454F4Du, 0.12f, 2.f);
        for (const Vertex &v : m.vertices) {
            const float r = std::sqrt(v.px * v.px + v.py * v.py + v.pz * v.pz);
            QVERIFY2(r >= 0.88f - 1e-4f && r <= 1.12f + 1e-4f, "radius out of range");
            const float n = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
            QVERIFY(std::abs(n - 1.f) < 1e-4f);
        }
    }

    void sceneTotals()
    {
        struct Row {
            Workload w;
            std::uint64_t tris;
            std::uint32_t draws;
        };
        for (const Row &r : {Row{Workload::Geometry, 11796480, 36}, Row{Workload::DrawCalls, 1566002, 12001},
                             Row{Workload::Shading, 916992, 195}, Row{Workload::Particles, 1048579, 17}}) {
            const SceneData s = buildScene(r.w);
            QVERIFY2(s.trianglesPerFrame == r.tris, workloadId(r.w));
            QVERIFY2(s.drawsPerFrame == r.draws, workloadId(r.w));
        }
        const SceneData q = buildScene(Workload::Quads);
        QCOMPARE(q.quadLayers, 128u);
        QCOMPARE(q.drawsPerFrame, 1u);
        const SceneData sp = buildScene(Workload::Sprites);
        QCOMPARE(sp.sprites.size(), std::size_t(100000));
        QCOMPARE(sp.drawsPerFrame, 25u);
    }

    void sceneContentDetails()
    {
        const SceneData shading = buildScene(Workload::Shading);
        QCOMPARE(shading.lights.size(), std::size_t(128));
        QCOMPARE(shading.instances.size(), std::size_t(65));
        QCOMPARE(shading.textures.size(), std::size_t(4));
        const SceneData draw = buildScene(Workload::DrawCalls);
        QCOMPARE(draw.textures.size(), std::size_t(16));
        QCOMPARE(draw.instances[0].mesh, 8u);  // the ground plane is drawn first
        const SceneData part = buildScene(Workload::Particles);
        QCOMPARE(part.particles.size(), std::size_t(524288));
        for (const Particle &p : part.particles) {
            QVERIFY(p.life >= 2.f && p.life <= 4.f && p.phase >= 0.f && p.phase < p.life + 1e-4f);
            QVERIFY(p.speed >= 6.f && p.speed <= 10.f && p.size >= 0.04f && p.size <= 0.12f);
        }
    }

    void scaledScenes()
    {
        const double s = 64;
        QCOMPARE(buildScene(Workload::Geometry, s).instances.size(), std::size_t(1));
        const SceneData d = buildScene(Workload::DrawCalls, s);
        QCOMPARE(d.instances.size(), std::size_t(1 + 188));
        QCOMPARE(d.drawsPerFrame, 189u);
        const SceneData sh = buildScene(Workload::Shading, s);
        QCOMPARE(sh.lights.size(), std::size_t(2));
        QCOMPARE(sh.drawsPerFrame, 6u);
        QCOMPARE(buildScene(Workload::Particles, s).particles.size(), std::size_t(8192));
        QCOMPARE(buildScene(Workload::Quads, s).quadLayers, 2u);
        const SceneData sp = buildScene(Workload::Sprites, s);
        QCOMPARE(sp.sprites.size(), std::size_t(1563));
        QCOMPARE(sp.drawsPerFrame, 1u);
    }

    void perspectiveMatrix()
    {
        const Mat4 p = perspective(float(3.14159265358979323846 / 3.0), 16.f / 9.f, 0.1f, 200.f);
        QVERIFY(std::abs(p.m[0] - 0.974279f) < 1e-5f);
        QVERIFY(std::abs(p.m[5] - 1.732051f) < 1e-5f);
        QVERIFY(std::abs(p.m[10] - -1.000500f) < 1e-5f);
        QVERIFY(std::abs(p.m[11] - -1.f) < 1e-5f);
        QVERIFY(std::abs(p.m[14] - -0.100050f) < 1e-5f);
    }

    void geometryCamera()
    {
        const Camera a = cameraAt(Workload::Geometry, 0, 16.0 / 9.0);
        QVERIFY(std::abs(a.eye.x - 14.f) < 1e-4f && std::abs(a.eye.y - 6.f) < 1e-4f && std::abs(a.eye.z) < 1e-4f);
        const Camera b = cameraAt(Workload::Geometry, 5, 16.0 / 9.0);
        QVERIFY(std::abs(b.eye.x) < 1e-4f && std::abs(b.eye.y - 6.f) < 1e-4f && std::abs(b.eye.z - 14.f) < 1e-4f);
        const Camera loop = cameraAt(Workload::Geometry, 20, 16.0 / 9.0);  // a 20 s loop
        QVERIFY(std::abs(loop.eye.x - 14.f) < 1e-3f && std::abs(loop.eye.z) < 1e-3f);
    }

    void lightsAndSpinsMove()
    {
        const SceneData s = buildScene(Workload::Shading, 64);
        const auto now = lightsAt(s, 0), later = lightsAt(s, 3);
        QCOMPARE(now.size(), s.lights.size());
        QVERIFY(now[0].position.x == s.lights[0].position.x);
        QVERIFY(now[0].position.x != later[0].position.x || now[0].position.z != later[0].position.z);
        const Mat4 a = instanceModelAt(s, 1, 0), b = instanceModelAt(s, 1, 2);
        bool differs = false;
        for (int i = 0; i < 16; ++i)
            differs = differs || a.m[i] != b.m[i];
        QVERIFY(differs);
        QVERIFY(instanceModelAt(s, 0, 7).m[0] == 1.f);  // the room never spins
    }

    void mipChains()
    {
        Texture t;
        t.width = t.height = 1024;
        t.rgba8.assign(std::size_t(1024) * 1024 * 4, 128);
        const auto chain = mipChain(t);
        QCOMPARE(chain.size(), std::size_t(11));
        QCOMPARE(chain.back().width, 1u);
        QCOMPARE(chain.back().rgba8[0], std::uint8_t(128));
        for (const auto &[size, levels] : {std::pair<std::uint32_t, int>{64, 7}, {256, 9}}) {
            Texture u;
            u.width = u.height = size;
            u.rgba8.assign(std::size_t(size) * size * 4, 0);
            QCOMPARE(int(mipChain(u).size()), levels);
        }
    }

    void texturesAreDeterministic()
    {
        QCOMPARE(checker(3, 64).rgba8, checker(3, 64).rgba8);
        QVERIFY(checker(3, 64).rgba8 != checker(4, 64).rgba8);
        const Texture disc = softDisc(64);
        QVERIFY(disc.rgba8[(32 * 64 + 32) * 4 + 3] > 200);  // soft: bright in the middle,
        QCOMPARE(disc.rgba8[3], std::uint8_t(0));           // clear in the corner
        const Texture atlas = spriteAtlas(1);
        QCOMPARE(atlas.width, 256u);
        QCOMPARE(atlas.rgba8.size(), std::size_t(256) * 256 * 4);
    }

    // The golden content hashes (golden_hashes.inc, Phase 4a task 9, M4): the
    // same on every compiler and OS, so the CI on Windows proves Windows =
    // Linux. A change of content changes them: regenerate the file with
    // synbench-gpu --print-hashes.
    void contentHashes()
    {
        static_assert(kGoldenHash.size() == kAllWorkloads.size());
        for (std::size_t i = 0; i < kAllWorkloads.size(); ++i) {
            const Workload w = kAllWorkloads[i];
            const std::uint64_t want = kGoldenHash[i];
            const SceneData a = buildScene(w), b = buildScene(w);
            const std::uint64_t h = contentHash(a);
            QCOMPARE(h, contentHash(b));  // the same seed builds the same scene
            if (h != want)
                QFAIL(qPrintable(QStringLiteral("%1: got 0x%2, golden 0x%3")
                                     .arg(QString::fromLatin1(workloadId(w)))
                                     .arg(h, 16, 16, QLatin1Char('0'))
                                     .arg(want, 16, 16, QLatin1Char('0'))));
        }
        // A different content scale is different content.
        QVERIFY(contentHash(buildScene(Workload::Quads)) != contentHash(buildScene(Workload::Quads, 2)));
        QVERIFY(contentHash(buildScene(Workload::Sprites, 64)) != contentHash(buildScene(Workload::Sprites, 32)));
    }

    void workloadIds()
    {
        for (const Workload w : kAllWorkloads)
            QCOMPARE(workloadFromId(workloadId(w)), std::optional<Workload>(w));
        QVERIFY(!workloadFromId("metal"));
    }

    // --- the bitmap font and the overlay (task 8a) ---

    static int popcount(unsigned v)
    {
        int n = 0;
        for (; v; v &= v - 1)
            ++n;
        return n;
    }
    static int glyphInk(char c)
    {
        int n = 0;
        for (int row = 0; row < kGlyphH; ++row)
            n += popcount(kGlyphRows[c - kFirstChar][row]);
        return n;
    }

    void fontGlyphs()
    {
        for (int row = 0; row < kGlyphH; ++row)
            QCOMPARE(int(kGlyphRows[0][row]), 0);  // the space
        for (int g = 0; g < kGlyphCount; ++g) {
            for (int row = 0; row < kGlyphH; ++row)
                QVERIFY(kGlyphRows[g][row] < 32);
        }
        std::vector<std::vector<int>> seen;
        for (const char c : std::string("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")) {
            QVERIFY2(glyphInk(c) > 0, "an empty glyph");
            std::vector<int> rows(kGlyphRows[c - kFirstChar], kGlyphRows[c - kFirstChar] + kGlyphH);
            for (const auto &other : seen)
                QVERIFY2(other != rows, "two glyphs are the same");
            seen.push_back(rows);
        }
    }

    void glyphAtlasShape()
    {
        const Texture a = glyphAtlas();
        QCOMPARE(a.width, 570u);
        QCOMPARE(a.height, 9u);
        std::size_t ink = 0, want = 0;
        for (std::size_t i = 3; i < a.rgba8.size(); i += 4)
            ink += a.rgba8[i] == 255 ? 1 : 0;
        for (int g = 0; g < kGlyphCount; ++g) {
            for (int row = 0; row < kGlyphH; ++row)
                want += std::size_t(popcount(kGlyphRows[g][row]));
        }
        QCOMPARE(ink, want);
    }

    void overlayTexts()
    {
        QCOMPARE(overlayText("Geometry", "Vulkan", 412), std::string("Geometry | Vulkan | 412 fps"));
        QCOMPARE(overlayText("Geometry", "Vulkan", std::nullopt), std::string("Geometry | Vulkan | warming up"));
        QCOMPARE(overlayText(std::string(60, 'x'), "Vulkan", 1).size(), std::size_t(40));
        QCOMPARE(overlayText("Caf\xC3\xA9", "Vulkan", 1), std::string("Caf?? | Vulkan | 1 fps"));
    }

    void overlayLayouts()
    {
        const OverlayLayout a = overlayLayout(27, 1.0);
        QCOMPARE(a.x, 8);
        QCOMPARE(a.y, 8);
        QCOMPARE(a.k, 2);
        QCOMPARE(a.pad, 6);
        QCOMPARE(a.w, 334);
        QCOMPARE(a.h, 26);
        const OverlayLayout b = overlayLayout(27, 1.5);
        QCOMPARE(b.x, 12);
        QCOMPARE(b.y, 12);
        QCOMPARE(b.k, 3);
        QCOMPARE(b.pad, 9);
        QCOMPARE(b.w, 501);
        QCOMPARE(b.h, 39);
    }

    void overlayPacking()
    {
        const auto p = packOverlay("ABCDE");
        QCOMPARE(p[0], 0x44434241u);
        QCOMPARE(p[1], 0x45u);
        QCOMPARE(p[2], 0u);
    }

    void overlayRaster()
    {
        OverlayLayout l;
        const std::vector<std::uint8_t> px = rasterOverlay("A", 1.0, &l);
        QCOMPARE(l.w, 22);
        QCOMPARE(l.h, 26);
        QCOMPARE(px.size(), std::size_t(22) * 26 * 4);
        std::size_t ink = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            if (px[i] == 255 && px[i + 1] == 255 && px[i + 2] == 255 && px[i + 3] == 255)
                ++ink;
            else
                QVERIFY(px[i] == 0 && px[i + 1] == 0 && px[i + 2] == 0 && px[i + 3] == 153);
        }
        QCOMPARE(ink, std::size_t(4 * glyphInk('A')));
    }
};

QTEST_GUILESS_MAIN(TestGpuScene)
#include "tst_gpu_scene.moc"

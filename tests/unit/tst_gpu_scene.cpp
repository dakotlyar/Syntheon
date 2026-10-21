// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU scene content (Phase 4a design §6): mesh and scene counts, the maths
// helpers, the cameras, mip chains and the content hash. No GPU, no window.
#include "tests/gpu/worker/scene/mesh_gen.h"
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

    // The golden hashes are added with the worker's --print-hashes (task 9);
    // until then they are printed for that task.
    void contentHashes()
    {
        for (const Workload w : kAllWorkloads) {
            const SceneData a = buildScene(w), b = buildScene(w);
            const std::uint64_t h = contentHash(a);
            QCOMPARE(h, contentHash(b));  // the same seed builds the same scene
            qInfo("contentHash %s = 0x%016llx", workloadId(w), static_cast<unsigned long long>(h));
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
};

QTEST_GUILESS_MAIN(TestGpuScene)
#include "tst_gpu_scene.moc"

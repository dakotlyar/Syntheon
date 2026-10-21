// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test's pure model (Phase 4a design §9, §10): ids and keys, frame
// statistics, best-of and the signature comparison.
#include "tests/gpu/model/best_of.h"
#include "tests/gpu/model/frame_stats.h"
#include "tests/gpu/model/gpu_identity.h"
#include "tests/gpu/model/ids.h"
#include "tests/gpu/model/render_size.h"

#include <QTest>

#include <cmath>

using namespace sb::gpu;

class TestGpuModel : public QObject
{
    Q_OBJECT

private slots:
    void keys()
    {
        QCOMPARE(sceneApiKey(ApiId::Vulkan, SceneId::Geometry, u"fps"), QStringLiteral("scenes3d.vulkan.geometry.fps"));
        QCOMPARE(sceneBestKey(SceneId::Shading, u"api"), QStringLiteral("scenes3d.shading.api"));
        QCOMPARE(apiSummaryKey(ApiId::Dx11, u"low"), QStringLiteral("scenes3d.dx11.low"));
        QCOMPARE(fillApiKey(ApiId::Dx12, FillId::Quads), QStringLiteral("fill2d.dx12"));
        QCOMPARE(fillApiKey(ApiId::Dx12, FillId::Sprites), QStringLiteral("fill2d.dx12.sprites"));
        QCOMPARE(fillApiFpsKey(ApiId::Dx12, FillId::Sprites), QStringLiteral("fill2d.dx12.sprites.fps"));
        QCOMPARE(fillBestKey(FillId::Quads), QStringLiteral("fill2d.quads"));
        QCOMPARE(fillBestKey(FillId::Quads, true), QStringLiteral("fill2d.quads.api"));
        QCOMPARE(nativePathMetricKey(NativePath::QtRaster, NativeWorkload::Text), QStringLiteral("native2d.qt.text"));
        QCOMPARE(nativeBestKey(NativeWorkload::Text), QStringLiteral("native2d.text"));
        QCOMPARE(nativeBestKey(NativeWorkload::Text, true), QStringLiteral("native2d.text.path"));
        QCOMPARE(apiStatusKey(u"d2d", u"status"), QStringLiteral("api.d2d.status"));
        QCOMPARE(seriesKey(u"vulkan", u"geometry"), QStringLiteral("ft.vulkan.geometry"));
        QCOMPARE(kScenes3d, QStringLiteral("scenes3d"));
    }

    void roundTrips()
    {
        for (const ApiId a : kApis)
            QCOMPARE(apiFromKey(apiKey(a)), std::optional<ApiId>(a));
        for (const SceneId s : kScenes)
            QCOMPARE(sceneFromKey(sceneKey(s)), std::optional<SceneId>(s));
        for (const FillId f : kFills)
            QCOMPARE(fillFromKey(fillKey(f)), std::optional<FillId>(f));
        for (const NativePath p : kNativePaths)
            QCOMPARE(nativePathFromKey(nativePathKey(p)), std::optional<NativePath>(p));
        for (const NativeWorkload w : kNativeWorkloads)
            QCOMPARE(nativeFromKey(nativeKey(w)), std::optional<NativeWorkload>(w));
        QVERIFY(!apiFromKey(u"metal"));
        QVERIFY(!sceneFromKey(u""));
    }

    void names()
    {
        QCOMPARE(apiName(ApiId::Dx12), QStringLiteral("DirectX 12"));
        QCOMPARE(apiShort(ApiId::Dx12), QStringLiteral("DX12"));
        QCOMPARE(sceneName(SceneId::Particles), QStringLiteral("Particles + post"));
        QCOMPARE(fillName(FillId::Quads), QStringLiteral("Textured quads"));
        QCOMPARE(nativePathName(NativePath::QtRaster), QStringLiteral("Qt raster"));
        QCOMPARE(nativeName(NativeWorkload::Blits), QStringLiteral("Image blits"));
    }

    void frameStatsWorkedExample()
    {
        const FrameStats s = frameStats({10, 10, 10, 10, 10, 10, 10, 10, 10, 20});
        QCOMPARE(s.frames, 10);
        QVERIFY(std::abs(s.seconds - 0.110) < 1e-9);
        QVERIFY(std::abs(s.fps - 90.9091) < 1e-4);
        QCOMPARE(s.p1, 10.0);
        QCOMPARE(s.p50, 10.0);
        QCOMPARE(s.p99, 20.0);
        QCOMPARE(s.low1, 50.0);
    }

    void frameStatsOneFrameAndNone()
    {
        const FrameStats one = frameStats({16});
        QCOMPARE(one.fps, 62.5);
        QCOMPARE(one.p1, 16.0);
        QCOMPARE(one.p50, 16.0);
        QCOMPARE(one.p99, 16.0);
        QCOMPARE(one.low1, 62.5);
        const FrameStats none = frameStats({});
        QCOMPARE(none.frames, 0);
        QCOMPARE(none.fps, 0.0);
    }

    void percentilesMedianGeometricMean()
    {
        std::vector<double> v;
        for (int i = 1; i <= 100; ++i)
            v.push_back(i);
        QCOMPARE(percentileNearestRank(v, 1), 1.0);
        QCOMPARE(percentileNearestRank(v, 50), 50.0);
        QCOMPARE(percentileNearestRank(v, 99), 99.0);
        QCOMPARE(percentileNearestRank(v, 100), 100.0);
        QCOMPARE(median({3, 1, 2}), 2.0);
        QCOMPARE(median({4, 1, 3, 2}), 2.5);
        QCOMPARE(median({}), 0.0);
        QVERIFY(std::abs(geometricMean({2, 8}) - 4.0) < 1e-12);
        QCOMPARE(geometricMean({2, 0}), 0.0);
        QCOMPARE(geometricMean({}), 0.0);
    }

    void buckets()
    {
        const QVector<QPointF> b = bucketSeries({10, 10, 10, 10, 10, 10, 10, 10, 10, 20});
        QCOMPARE(b.size(), 2);
        QVERIFY(std::abs(b[0].x() - 0.05) < 1e-12 && std::abs(b[0].y() - 10) < 1e-12);
        QVERIFY(std::abs(b[1].x() - 0.15) < 1e-12 && std::abs(b[1].y() - 20) < 1e-12);
        QVERIFY(bucketSeries({}).isEmpty());
    }

    void bestOfRules()
    {
        QCOMPARE(bestOf({{QStringLiteral("vulkan"), 410}, {QStringLiteral("dx12"), 412}})->source, QStringLiteral("dx12"));
        QCOMPARE(bestOf({{QStringLiteral("vulkan"), 410}, {QStringLiteral("dx12"), 412}})->value, 412.0);
        QCOMPARE(bestOf({{QStringLiteral("vulkan"), 412}, {QStringLiteral("dx12"), 412}})->source, QStringLiteral("vulkan"));
        QVERIFY(!bestOf({}));
        QVERIFY(!bestOf({{QStringLiteral("vulkan"), 0}}));
    }

    void signatures()
    {
        const QByteArray a(432, '\x40');
        const auto same = compareSignatures(a, a);
        QVERIFY(same);
        QCOMPARE(same->mean, 0.0);
        QCOMPARE(same->max, 0);
        QVERIFY(signaturesMatch(*same));
        QByteArray b = a;
        b[7] = char(0x40 + 40);
        const auto diff = compareSignatures(a, b);
        QVERIFY(diff);
        QVERIFY(std::abs(diff->mean - 40.0 / 432.0) < 1e-9);
        QCOMPARE(diff->max, 40);
        QVERIFY(!signaturesMatch(*diff));
        QVERIFY(!compareSignatures(a, QByteArray(431, '\x40')));
    }

    void planWindowTable_data()
    {
        QTest::addColumn<int>("availW");
        QTest::addColumn<int>("availH");
        QTest::addColumn<double>("scale");
        QTest::addColumn<QList<int>>("expect");  // windowLogical, windowPhysical, render (w, h each)
        QTest::addColumn<bool>("fit");
        QTest::newRow("tiamat") << 3440 << 1400 << 1.0 << QList<int>{1920, 1080, 1920, 1080, 1920, 1080} << false;
        QTest::newRow("1440p") << 2560 << 1400 << 1.0 << QList<int>{1920, 1080, 1920, 1080, 1920, 1080} << false;
        QTest::newRow("1080p monitor") << 1920 << 1032 << 1.0 << QList<int>{1664, 936, 1664, 936, 1920, 1080} << true;
        QTest::newRow("1600p at 150") << 1707 << 1019 << 1.5 << QList<int>{1640, 923, 2460, 1385, 2880, 1620} << true;
        QTest::newRow("1080p at 125") << 1536 << 824 << 1.25 << QList<int>{1294, 728, 1618, 910, 2400, 1350} << true;
        QTest::newRow("1200p at 150") << 1280 << 752 << 1.5 << QList<int>{1166, 656, 1749, 984, 2880, 1620} << true;
        QTest::newRow("1440p at 135") << 2560 << 1400 << 1.35 << QList<int>{1920, 1080, 2592, 1458, 2592, 1458} << false;
        QTest::newRow("ultrawide short") << 3000 << 1000 << 1.0 << QList<int>{1607, 904, 1607, 904, 1920, 1080} << true;
    }

    void planWindowTable()
    {
        QFETCH(int, availW);
        QFETCH(int, availH);
        QFETCH(double, scale);
        QFETCH(QList<int>, expect);
        QFETCH(bool, fit);
        const WindowPlan p = planWindow({availW, availH}, scale);
        QCOMPARE(p.windowLogical.width, expect[0]);
        QCOMPARE(p.windowLogical.height, expect[1]);
        QCOMPARE(p.windowPhysical.width, expect[2]);
        QCOMPARE(p.windowPhysical.height, expect[3]);
        QCOMPARE(p.render.width, expect[4]);
        QCOMPARE(p.render.height, expect[5]);
        QCOMPARE(p.fit, fit);
    }

    void sizeAndScaleText()
    {
        QCOMPARE(sizeText({1920, 1080}), QStringLiteral("1920\u00D71080"));
        QCOMPARE(scalePercent(1.35), 135);
        QCOMPARE(scalePercent(1.0), 100);
    }

    void gpuIds()
    {
        QCOMPARE(makeLinuxGpuId(0x10de, 0x2c05, QStringLiteral("0000:01:00.0")), QStringLiteral("10de:2c05@0000:01:00.0"));
        QCOMPARE(makeLinuxGpuId(0x10de, 0x2c05, {}), QStringLiteral("10de:2c05#0"));
        QCOMPARE(makeWindowsGpuId(0x1002, 0x73df, 1), QStringLiteral("1002:73df#1"));
    }

    void defaultGpu()
    {
        const auto gpu = [](GpuKind k, qint64 vram, const char *id) {
            GpuEntry g;
            g.kind = k;
            g.vramBytes = vram;
            g.id = QString::fromLatin1(id);
            return g;
        };
        constexpr qint64 MiB = 1024 * 1024, GiB = 1024 * MiB;
        QCOMPARE(defaultGpuIndex({gpu(GpuKind::Integrated, 512 * MiB, "a"), gpu(GpuKind::Discrete, 16 * GiB, "b")}), std::optional<int>(1));
        QCOMPARE(defaultGpuIndex({gpu(GpuKind::Discrete, 8 * GiB, "a"), gpu(GpuKind::Discrete, 12 * GiB, "b")}), std::optional<int>(1));
        QCOMPARE(defaultGpuIndex({gpu(GpuKind::Integrated, 0, "a"), gpu(GpuKind::Integrated, 0, "b")}), std::optional<int>(0));
        QVERIFY(!defaultGpuIndex({gpu(GpuKind::Software, 0, "s")}));
        QCOMPARE(defaultGpuIndex({gpu(GpuKind::Software, 0, "s")}, true), std::optional<int>(0));
        QVERIFY(!defaultGpuIndex({}));
        const QVector<GpuEntry> two{gpu(GpuKind::Integrated, 0, "a"), gpu(GpuKind::Discrete, 0, "b")};
        QCOMPARE(findGpu(two, QStringLiteral("b")), 1);
        QCOMPARE(findGpu(two, QStringLiteral("zz")), -1);
    }

    void openGlHelpers()
    {
        QCOMPARE(glVendorToPci(QStringLiteral("NVIDIA Corporation")), std::optional<quint32>(0x10de));
        QCOMPARE(glVendorToPci(QStringLiteral("AMD")), std::optional<quint32>(0x1002));
        QCOMPARE(glVendorToPci(QStringLiteral("ATI Technologies Inc.")), std::optional<quint32>(0x1002));
        QCOMPARE(glVendorToPci(QStringLiteral("Intel")), std::optional<quint32>(0x8086));
        QCOMPARE(glVendorToPci(QStringLiteral("Qualcomm")), std::optional<quint32>(0x5143));
        QVERIFY(!glVendorToPci(QStringLiteral("Mesa")));
        QVERIFY(!glVendorToPci(QStringLiteral("Microsoft Corporation")));
        const QString renderer = QStringLiteral("D3D12 (Qualcomm(R) Adreno(TM) X1-85 GPU)");
        QVERIFY(isGlOn12(QStringLiteral("Microsoft Corporation"), renderer));
        QVERIFY(!isGlOn12(QStringLiteral("Mesa"), renderer));
        QCOMPARE(glOn12DeviceName(renderer), QStringLiteral("Qualcomm(R) Adreno(TM) X1-85 GPU"));
        QVERIFY(isSoftwareGl(QStringLiteral("llvmpipe (LLVM 17.0.6, 256 bits)")));
        QVERIFY(isSoftwareGl(QStringLiteral("GDI Generic")));
        QVERIFY(!isSoftwareGl(QStringLiteral("NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2")));
    }

    void texts()
    {
        QCOMPARE(vramText(17094934528), QStringLiteral("16 GB"));
        QCOMPARE(vramText(536870912), QStringLiteral("512 MB"));
        QCOMPARE(kindText(GpuKind::Discrete), QStringLiteral("discrete"));
        QCOMPARE(kindText(GpuKind::Other), QStringLiteral("other"));
    }
};

QTEST_GUILESS_MAIN(TestGpuModel)
#include "tst_gpu_model.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test's pure model (Phase 4a design §9, §10): ids and keys, frame
// statistics, best-of and the signature comparison.
#include "tests/gpu/model/best_of.h"
#include "tests/gpu/model/frame_stats.h"
#include "tests/gpu/model/ids.h"

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
};

QTEST_GUILESS_MAIN(TestGpuModel)
#include "tst_gpu_model.moc"

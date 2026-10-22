// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test's pure model (Phase 4a design §9, §10): ids and keys, frame
// statistics, best-of and the signature comparison.
#include "tests/gpu/model/best_of.h"
#include "tests/gpu/model/drm_display.h"
#include "tests/gpu/model/frame_stats.h"
#include "tests/gpu/model/gl_env.h"
#include "tests/gpu/model/gpu_identity.h"
#include "tests/gpu/model/linux_gpus.h"
#include "tests/gpu/model/ids.h"
#include "tests/gpu/model/protocol.h"
#include "tests/gpu/model/render_size.h"
#include "tests/gpu/model/windows_gpus.h"

#include <QTest>

#include <chrono>
#include <cmath>
#include <cstring>

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

    // AP-5 L7: a screen too small for a 640x360 window is "too small", not a tiny window.
    void tooSmallScreens()
    {
        QVERIFY(!planWindow({704, 456}, 1.0).tooSmall);  // exactly 640x360 after the margins
        QCOMPARE(planWindow({704, 456}, 1.0).windowLogical.width, 640);
        QVERIFY(planWindow({703, 456}, 1.0).tooSmall);   // aw 639: width-limited 639x359
        QVERIFY(planWindow({704, 455}, 1.0).tooSmall);   // ah 359
        QVERIFY(planWindow({100, 100}, 1.0).tooSmall);
        QVERIFY(planWindow({0, 0}, 1.0).tooSmall);
        QVERIFY(planWindow({1280, 400}, 2.0).tooSmall);
        QVERIFY(!planWindow({1920, 1032}, 1.0).tooSmall);
        QVERIFY(!planWindow({3440, 1400}, 1.0).tooSmall);
    }

    // --- the Linux GPU list (task 13) ---

    static QString fixtureRoot(const char *sub) { return QString::fromUtf8(SB_FIXTURES) + QString::fromUtf8(sub); }

    static QVector<VkDeviceRecord> tiamatVulkan()
    {
        VkDeviceRecord nv;
        nv.name = QStringLiteral("NVIDIA GeForce RTX 5070 Ti");
        nv.vendorId = 0x10de;
        nv.deviceId = 0x2c05;
        nv.type = QStringLiteral("discrete");
        nv.apiVersion = QStringLiteral("1.4.351");
        nv.driverName = QStringLiteral("NVIDIA");
        nv.driverInfo = QStringLiteral("615.78.08");
        nv.deviceLocalBytes = 17094934528;
        nv.pciSlot = QStringLiteral("0000:01:00.0");
        VkDeviceRecord amd;
        amd.name = QStringLiteral("AMD Radeon 610M (RADV RAPHAEL_MENDOCINO)");
        amd.vendorId = 0x1002;
        amd.deviceId = 0x13c0;
        amd.type = QStringLiteral("integrated");
        amd.apiVersion = QStringLiteral("1.4.354");
        amd.driverName = QStringLiteral("radv");
        amd.driverInfo = QStringLiteral("Mesa 26.2.4");
        amd.deviceLocalBytes = 536870912;
        amd.pciSlot = QStringLiteral("0000:79:00.0");
        return {nv, amd};  // the loader's order: not the DRM order
    }

    void drmCardsFromTheTiamatFixture()
    {
        const QVector<DrmCard> cards = readDrmCards(fixtureRoot("/linux/tiamat"));
        QCOMPARE(cards.size(), 2);
        QCOMPARE(cards[0], (DrmCard{QStringLiteral("0000:79:00.0"), 0x1002, 0x13c0, QStringLiteral("amdgpu"), false}));  // DP-9 is connected but disabled
        QCOMPARE(cards[1], (DrmCard{QStringLiteral("0000:01:00.0"), 0x10de, 0x2c05, QStringLiteral("nvidia"), true}));  // DP-2 and DP-3 connected and enabled
        QVERIFY(readDrmCards(fixtureRoot("/linux/nowhere")).isEmpty());
    }

    void mergeTiamat()
    {
        const QVector<GpuEntry> gpus = mergeLinuxGpus(tiamatVulkan(), readDrmCards(fixtureRoot("/linux/tiamat")));
        QCOMPARE(gpus.size(), 2);
        QCOMPARE(gpus[0].id, QStringLiteral("1002:13c0@0000:79:00.0"));
        QCOMPARE(gpus[0].kind, GpuKind::Integrated);
        QCOMPARE(gpus[0].kernelDriver, QStringLiteral("amdgpu"));
        QCOMPARE(gpus[1].id, QStringLiteral("10de:2c05@0000:01:00.0"));
        QCOMPARE(gpus[1].kind, GpuKind::Discrete);
        QCOMPARE(gpus[1].name, QStringLiteral("NVIDIA GeForce RTX 5070 Ti"));
        QCOMPARE(gpus[1].vramBytes, qint64(17094934528));
        const ApiAvailability &v = gpus[1].apis.value(QStringLiteral("vulkan"));
        QVERIFY(v.ok);
        QCOMPARE(v.version, QStringLiteral("Vulkan 1.4.351"));
        QCOMPARE(v.driver, QStringLiteral("NVIDIA 615.78.08"));
        QCOMPARE(gpus[1].apis.value(QStringLiteral("dx12")).reason, QStringLiteral("DirectX is Windows only"));
        QCOMPARE(gpus[1].apis.value(QStringLiteral("dx11")).reason, QStringLiteral("DirectX is Windows only"));
        QCOMPARE(gpus[1].apis.value(QStringLiteral("d2d")).reason, QStringLiteral("Direct2D is Windows only"));
        QCOMPARE(defaultGpuIndex(gpus), std::optional<int>(1));  // the NVIDIA
    }

    void mergeLavapipeAndOldDrivers()
    {
        QVector<VkDeviceRecord> vk = tiamatVulkan();
        VkDeviceRecord lvp;
        lvp.name = QStringLiteral("llvmpipe (LLVM 21.1.0, 256 bits)");
        lvp.vendorId = 0x10005;
        lvp.type = QStringLiteral("cpu");
        lvp.apiVersion = QStringLiteral("1.4.318");
        vk.append(lvp);
        QVector<GpuEntry> gpus = mergeLinuxGpus(vk, readDrmCards(fixtureRoot("/linux/tiamat")));
        QCOMPARE(gpus.size(), 3);
        QCOMPARE(gpus[2].kind, GpuKind::Software);
        QCOMPARE(gpus[2].id, QStringLiteral("10005:0000#0"));
        QCOMPARE(defaultGpuIndex(gpus), std::optional<int>(1));  // software is excluded by default
        QCOMPARE(defaultGpuIndex(gpus, true), std::optional<int>(1));
        // Two devices without a slot: the n-th keeps its place (P11).
        vk.append(lvp);
        QCOMPARE(mergeLinuxGpus(vk, {}).last().id, QStringLiteral("10005:0000#1"));
        // A 1.2 driver.
        vk[0].apiVersion = QStringLiteral("1.2.198");
        const QVector<GpuEntry> old = mergeLinuxGpus(vk, readDrmCards(fixtureRoot("/linux/tiamat")));
        const ApiAvailability &a = old[1].apis.value(QStringLiteral("vulkan"));
        QVERIFY(!a.ok);
        QCOMPARE(a.reason, QStringLiteral("Needs Vulkan 1.3 (the driver has 1.2.198)"));
        // A card without a Vulkan device.
        const QVector<GpuEntry> none = mergeLinuxGpus({}, readDrmCards(fixtureRoot("/linux/tiamat")));
        QCOMPARE(none.size(), 2);
        QCOMPARE(none[0].apis.value(QStringLiteral("vulkan")).reason, QStringLiteral("This GPU has no Vulkan driver"));
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

    void protocolRoundTrips()
    {
        namespace pr = sb::gpu::protocol;
        GpuEntry g;
        g.id = QStringLiteral("10de:2c05@0000:01:00.0");
        g.name = QStringLiteral("NVIDIA GeForce RTX 5070 Ti");
        g.vendorId = 4318;
        g.deviceId = 11269;
        g.kind = GpuKind::Discrete;
        g.vramBytes = 17094934528;
        g.pciSlot = QStringLiteral("0000:01:00.0");
        g.kernelDriver = QStringLiteral("nvidia");
        g.apis.insert(QStringLiteral("vulkan"), {true, QStringLiteral("Vulkan 1.4.351"), QStringLiteral("NVIDIA 615.78.08"), {}, {}});
        g.apis.insert(QStringLiteral("dx12"), {false, {}, {}, {}, QStringLiteral("DirectX is Windows only")});
        pr::GlEvent gl;
        gl.ok = true;
        gl.version = QStringLiteral("OpenGL 4.6");
        gl.renderer = QStringLiteral("NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2");
        gl.vendor = QStringLiteral("NVIDIA Corporation");
        gl.vendorId = 4318;
        pr::ReadyEvent ready;
        ready.api = QStringLiteral("vulkan");
        ready.device = QStringLiteral("NVIDIA GeForce RTX 5070 Ti");
        ready.version = QStringLiteral("Vulkan 1.4.351");
        ready.driver = QStringLiteral("NVIDIA 615.78.08");
        ready.present = QStringLiteral("IMMEDIATE");
        ready.screen = QStringLiteral("DP-3");
        ready.renderW = 1920;
        ready.renderH = 1080;
        ready.windowW = 1920;
        ready.windowH = 1080;
        ready.scale = 1.25;
        ready.gpuTimestamps = true;
        pr::ResultEvent res;
        res.workload = QStringLiteral("geometry");
        res.frames = 8231;
        res.seconds = 20.002;
        res.ftP1 = 2.21;
        res.ftP50 = 2.40;
        res.ftP99 = 3.08;
        res.fps = 411.5;
        res.gpuMs = 2.31;
        res.cpuMs = 0.41;
        res.series = {QPointF(0.05, 2.41), QPointF(0.15, 2.39)};
        res.screen = QStringLiteral("DP-2");
        res.renderW = 1920;
        res.renderH = 1080;
        res.windowW = 1920;
        res.windowH = 1080;
        res.scale = 1.0;
        for (int i = 0; i < 432; ++i)
            res.signature.append(char(i * 7));
        const QVector<pr::Event> events{pr::GpuEvent{g}, gl, ready, pr::StageEvent{QStringLiteral("geometry"), QStringLiteral("measure"), 12}, res,
                                        pr::ErrorEvent{QStringLiteral("shading"), QStringLiteral("the GPU driver reset"), false},
                                        pr::ErrorEvent{{}, QStringLiteral("No Vulkan driver is installed"), true}, pr::DoneEvent{},
                                        pr::IdleEvent{}};
        for (const pr::Event &e : events) {
            const QByteArray line = pr::encode(e);
            QVERIFY(!line.contains('\n'));
            QVERIFY(line.size() < pr::kMaxLine);
            QString err;
            const auto back = pr::parseLine(line, &err);
            QVERIFY2(back, qPrintable(err));
            QCOMPARE(back->index(), e.index());
            QCOMPARE(pr::encode(*back), line);  // an exact round trip, doubles included
        }
        // Spot checks of the decoded values (1e-9 on the doubles).
        const auto r = std::get<pr::ResultEvent>(*pr::parseLine(pr::encode(res), nullptr));
        QVERIFY(std::abs(r.fps - 411.5) < 1e-9 && std::abs(r.ftP99 - 3.08) < 1e-9 && std::abs(*r.gpuMs - 2.31) < 1e-9);
        QCOMPARE(r.signature, res.signature);
        QCOMPARE(r.screen, QStringLiteral("DP-2"));
        QCOMPARE(r.windowW, 1920);
        QVERIFY(!r.fit);
        QCOMPARE(r.series.size(), 2);
        QVERIFY(std::abs(r.series[1].y() - 2.39) < 1e-9);
        const auto gg = std::get<pr::GpuEvent>(*pr::parseLine(pr::encode(pr::GpuEvent{g}), nullptr));
        QCOMPARE(gg.gpu, g);
        const auto rr = std::get<pr::ReadyEvent>(*pr::parseLine(pr::encode(ready), nullptr));
        QVERIFY(std::abs(rr.scale - 1.25) < 1e-9 && rr.gpuTimestamps && !rr.fit && rr.screen == QStringLiteral("DP-3"));
        // A result without GPU timestamps has no gpuMs.
        res.gpuMs.reset();
        QVERIFY(!std::get<pr::ResultEvent>(*pr::parseLine(pr::encode(res), nullptr)).gpuMs);
        // The optional present mode (task 10): sent only when known, and round-tripped.
        QVERIFY(!pr::encode(res).contains("\"present\""));
        res.present = QStringLiteral("MAILBOX");
        QCOMPARE(std::get<pr::ResultEvent>(*pr::parseLine(pr::encode(res), nullptr)).present, QStringLiteral("MAILBOX"));
        QString presentError;
        QVERIFY(!pr::parseLine(pr::encode(res).replace("\"MAILBOX\"", "7"), &presentError));
        QCOMPARE(presentError, QStringLiteral("result: bad present"));
    }

    void protocolErrors()
    {
        namespace pr = sb::gpu::protocol;
        QString err;
        QVERIFY(!pr::parseLine("{}", &err));
        QCOMPARE(err, QStringLiteral("no ev"));
        QVERIFY(!pr::parseLine("{\"ev\":\"x\"}", &err));
        QCOMPARE(err, QStringLiteral("unknown event 'x'"));
        QVERIFY(!pr::parseLine("{\"ev\":\"result\",\"workload\":\"g\",\"frames\":1,\"seconds\":1,\"ftMs\":{\"p1\":1,\"p50\":1,\"p99\":1},\"cpuMs\":1,\"sig\":\"\"}", &err));
        QCOMPARE(err, QStringLiteral("result: missing fps"));
        QVERIFY(!pr::parseLine("{\"ev\":\"result\",\"workload\":\"g\",\"frames\":1,\"seconds\":1,\"fps\":1,\"ftMs\":{\"p1\":1,\"p50\":1,\"p99\":1},\"cpuMs\":1,\"sig\":\"zz\"}", &err));
        QCOMPARE(err, QStringLiteral("result: bad sig"));
        QVERIFY(!pr::parseLine("not json", &err));
        // Extra keys are ignored.
        const auto done = pr::parseLine("{\"ev\":\"done\",\"extra\":[1,2]}", &err);
        QVERIFY(done && std::holds_alternative<pr::DoneEvent>(*done));
        const auto idle = pr::parseLine("{\"ev\":\"idle\"}", &err);
        QVERIFY(idle && std::holds_alternative<pr::IdleEvent>(*idle));
        QCOMPARE(pr::encode(pr::IdleEvent{}), QByteArray("{\"ev\":\"idle\"}"));
        const auto stage = pr::parseLine("{\"ev\":\"stage\",\"workload\":\"g\",\"phase\":\"warmup\",\"seconds\":3,\"x\":true}", &err);
        QVERIFY(stage && std::get<pr::StageEvent>(*stage).seconds == 3);
        // 864 hex characters make 432 bytes.
        const QByteArray sig(864, 'a');
        const QByteArray line = "{\"ev\":\"result\",\"workload\":\"g\",\"frames\":1,\"seconds\":1,\"fps\":1,\"ftMs\":{\"p1\":1,\"p50\":1,\"p99\":1},\"cpuMs\":1,\"sig\":\"" + sig + "\"}";
        const auto r = pr::parseLine(line, &err);
        QVERIFY2(r, qPrintable(err));
        QCOMPARE(std::get<pr::ResultEvent>(*r).signature.size(), 432);
        // Wrong types are malformed lines, not zeros (AP-5 L6).
        const auto result = [](const char *fps, const char *frames) {
            return QByteArray("{\"ev\":\"result\",\"workload\":\"g\",\"frames\":") + frames + ",\"seconds\":1,\"fps\":" + fps +
                   ",\"ftMs\":{\"p1\":1,\"p50\":1,\"p99\":1},\"cpuMs\":1,\"sig\":\"\"}";
        };
        QVERIFY(pr::parseLine(result("1", "1"), &err));
        QVERIFY(!pr::parseLine(result("\"fast\"", "1"), &err));
        QCOMPARE(err, QStringLiteral("result: bad fps"));
        QVERIFY(!pr::parseLine(result("1", "1.5"), &err));
        QCOMPARE(err, QStringLiteral("result: bad frames"));
        QVERIFY(!pr::parseLine(result("1", "-3"), &err));
        QVERIFY(!pr::parseLine(result("1", "99999999999"), &err));
        QVERIFY(!pr::parseLine("{\"ev\":\"stage\",\"workload\":\"g\",\"phase\":\"warmup\",\"seconds\":\"3\"}", &err));
        QVERIFY(!pr::parseLine("{\"ev\":\"gpu\",\"id\":5,\"name\":\"x\"}", &err));
        QVERIFY(!pr::parseLine("{\"ev\":\"gl\",\"ok\":\"yes\"}", &err));
        QVERIFY(!pr::parseLine("{\"ev\":\"ready\",\"api\":\"vulkan\",\"device\":\"d\",\"renderW\":1920,\"renderH\":1080,\"windowW\":1920}", &err));
        QCOMPARE(err, QStringLiteral("ready: missing windowH"));
        QVERIFY(!pr::parseLine(QByteArray(pr::kMaxLine + 1, ' '), &err));
    }

    // ---- task 23: the OpenGL environment, its verification and the Windows list ----

    void driPrime()
    {
        QCOMPARE(driPrimeValue(QStringLiteral("0000:79:00.0")), QStringLiteral("pci-0000_79_00_0"));
    }

    void glEnvironments()
    {
        const QVector<GpuEntry> gpus = mergeLinuxGpus(tiamatVulkan(), readDrmCards(fixtureRoot("/linux/tiamat")));
        const auto exists = [](const QString &) { return true; };
        const QProcessEnvironment amd = glEnvironmentFor(gpus[0], false, exists);  // the 610M, amdgpu
        QCOMPARE(amd.value(QStringLiteral("DRI_PRIME")), QStringLiteral("pci-0000_79_00_0"));
        QCOMPARE(amd.value(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME")), QStringLiteral("mesa"));
        QCOMPARE(amd.value(QStringLiteral("__EGL_VENDOR_LIBRARY_FILENAMES")), QStringLiteral("/usr/share/glvnd/egl_vendor.d/50_mesa.json"));
        const QProcessEnvironment nv = glEnvironmentFor(gpus[1], false, exists);  // the 5070 Ti, nvidia
        QCOMPARE(nv.value(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD")), QStringLiteral("1"));
        QCOMPARE(nv.value(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME")), QStringLiteral("nvidia"));
        QCOMPARE(nv.value(QStringLiteral("__EGL_VENDOR_LIBRARY_FILENAMES")), QStringLiteral("/usr/share/glvnd/egl_vendor.d/10_nvidia.json"));
        QVERIFY(!nv.contains(QStringLiteral("DRI_PRIME")));
        // The JSON only when it exists.
        const QProcessEnvironment noJson = glEnvironmentFor(gpus[0], false, [](const QString &) { return false; });
        QVERIFY(!noJson.contains(QStringLiteral("__EGL_VENDOR_LIBRARY_FILENAMES")));
        QVERIFY(noJson.contains(QStringLiteral("DRI_PRIME")));
        // A Mesa software device (no PCI slot): the GLX vendor only.
        GpuEntry lvp;
        lvp.apis.insert(QStringLiteral("vulkan"), ApiAvailability{true, QStringLiteral("Vulkan 1.4"), QStringLiteral("llvmpipe Mesa 26.2.4"), {}, {}, {}});
        const QProcessEnvironment soft = glEnvironmentFor(lvp, false, exists);
        QVERIFY(!soft.contains(QStringLiteral("DRI_PRIME")));
        QCOMPARE(soft.value(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME")), QStringLiteral("mesa"));
        // Windows: nothing (SynBench never writes the GPU preference).
        QVERIFY(glEnvironmentFor(gpus[1], true, exists).isEmpty());
    }

    void verifyGlCases()
    {
        namespace pr = sb::gpu::protocol;
        const QVector<GpuEntry> gpus = mergeLinuxGpus(tiamatVulkan(), readDrmCards(fixtureRoot("/linux/tiamat")));
        const GpuEntry &nv = gpus[1];
        const auto glEvent = [](const char *version, const char *renderer, const char *vendor, bool ok = true) {
            pr::GlEvent g;
            g.ok = ok;
            g.version = QLatin1StringView(version);
            g.renderer = QLatin1StringView(renderer);
            g.vendor = QLatin1StringView(vendor);
            return g;
        };
        const QString rtx = QStringLiteral("NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2");
        // The wrong GPU.
        ApiAvailability a = verifyGl(glEvent("OpenGL 4.6", "AMD Radeon Graphics (radeonsi)", "AMD"), nv, gpus, false);
        QVERIFY(!a.ok);
        QCOMPARE(a.reason, QStringLiteral("OpenGL runs on AMD Radeon Graphics (radeonsi) here, not on this GPU"));
        // The right one.
        a = verifyGl(glEvent("OpenGL 4.6", "NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2", "NVIDIA Corporation"), nv, gpus, false);
        QVERIFY(a.ok);
        QCOMPARE(a.version, QStringLiteral("OpenGL 4.6"));
        QCOMPARE(a.driver, rtx);
        // Two Intel GPUs: can't tell which.
        GpuEntry intelA, intelB;
        intelA.vendorId = intelB.vendorId = 0x8086;
        intelA.kind = intelB.kind = GpuKind::Integrated;
        a = verifyGl(glEvent("OpenGL 4.6", "Mesa Intel(R) Graphics", "Intel"), intelA, {intelA, intelB}, false);
        QVERIFY(!a.ok);
        QCOMPARE(a.reason, QStringLiteral("Can't tell which GPU OpenGL uses (Intel)"));
        // An unknown vendor.
        a = verifyGl(glEvent("OpenGL 4.6", "Something", "Acme"), nv, gpus, false);
        QCOMPARE(a.reason, QStringLiteral("Can't tell which GPU OpenGL uses (Acme)"));
        // GLon12 whose device is the chosen DXGI adapter.
        GpuEntry dx;
        dx.name = QStringLiteral("NVIDIA GeForce RTX 5070 Ti");
        dx.vendorId = 0x10de;
        a = verifyGl(glEvent("OpenGL 4.6", "D3D12 (NVIDIA GeForce RTX 5070 Ti)", "Microsoft Corporation"), dx, {dx}, false);
        QVERIFY(a.ok);
        QCOMPARE(a.note, QStringLiteral("via D3D12 compatibility layer"));
        a = verifyGl(glEvent("OpenGL 4.6", "D3D12 (Intel(R) UHD Graphics)", "Microsoft Corporation"), dx, {dx}, false);
        QVERIFY(!a.ok);
        // Too old (the context says 1.1.0).
        a = verifyGl(glEvent("1.1.0", "GDI Generic", "Microsoft Corporation", false), dx, {dx}, false);
        QCOMPARE(a.reason, QStringLiteral("Needs OpenGL 3.3 (the driver has 1.1)"));
        // Linux ARM: not tested, whatever the answer.
        a = verifyGl(glEvent("OpenGL 4.6", "NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2", "NVIDIA Corporation"), nv, gpus, true);
        QCOMPARE(a.reason, QStringLiteral("OpenGL isn't tested on Linux ARM"));
        // Windows on ARM: GDI Generic is the missing compatibility pack.
        a = verifyGl(glEvent("1.1.0", "GDI Generic", "Microsoft Corporation", false), dx, {dx}, false, true);
        QCOMPARE(a.reason, QStringLiteral("OpenGL needs Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack"));
        QCOMPARE(a.hint, QStringLiteral("https://apps.microsoft.com/detail/9nqpsl29bfff"));
        // A software renderer is only for a software GPU.
        a = verifyGl(glEvent("OpenGL 4.5", "llvmpipe (LLVM 21.1.0, 256 bits)", "Mesa"), nv, gpus, false);
        QVERIFY(!a.ok);
        GpuEntry soft;
        soft.kind = GpuKind::Software;
        QVERIFY(verifyGl(glEvent("OpenGL 4.5", "llvmpipe (LLVM 21.1.0, 256 bits)", "Mesa"), soft, {soft}, false).ok);
    }

    // The Windows list: ids numbered per vendor:device, kinds, Vulkan by LUID.
    void windowsList()
    {
        DxgiAdapterRecord nv, intel, warp, nv2;
        nv.name = QStringLiteral("NVIDIA GeForce RTX 5070 Ti");
        nv.vendorId = 0x10de;
        nv.deviceId = 0x2c05;
        nv.dedicatedBytes = qint64(16) << 30;
        nv.luid[0] = 1;
        nv.dx12.ok = nv.dx11.ok = nv.d2d.ok = true;
        nv.dx12.version = QStringLiteral("DirectX 12 (feature level 12_2)");
        nv2 = nv;
        nv2.luid[0] = 2;
        intel.name = QStringLiteral("Intel(R) UHD Graphics");
        intel.vendorId = 0x8086;
        intel.deviceId = 0x4680;
        intel.dedicatedBytes = qint64(128) << 20;
        intel.luid[0] = 3;
        intel.dx12.reason = QStringLiteral("Needs shader model 6.0");
        warp.name = QStringLiteral("Microsoft Basic Render Driver");
        warp.vendorId = 0x1414;
        warp.deviceId = 0x008c;
        warp.software = true;
        warp.luid[0] = 9;
        VkDeviceRecord vk;
        vk.name = QStringLiteral("NVIDIA GeForce RTX 5070 Ti");
        vk.apiVersion = QStringLiteral("1.4.351");
        vk.luidValid = true;
        vk.luid[0] = 2;  // the second adapter of the two identical cards
        const QVector<GpuEntry> gpus = mergeWindowsGpus({nv, nv2, intel, warp}, {vk});
        QCOMPARE(gpus.size(), 4);
        QCOMPARE(gpus[0].id, QStringLiteral("10de:2c05#0"));
        QCOMPARE(gpus[1].id, QStringLiteral("10de:2c05#1"));
        QCOMPARE(gpus[2].id, QStringLiteral("8086:4680#0"));
        QCOMPARE(gpus[3].id, QStringLiteral("1414:008c#0"));
        QCOMPARE(gpus[0].kind, GpuKind::Discrete);
        QCOMPARE(gpus[2].kind, GpuKind::Integrated);
        QCOMPARE(gpus[3].kind, GpuKind::Software);
        QVERIFY(!gpus[0].apis.value(QStringLiteral("vulkan")).ok);
        QCOMPARE(gpus[0].apis.value(QStringLiteral("vulkan")).reason, QStringLiteral("This GPU has no Vulkan driver"));
        QVERIFY(gpus[1].apis.value(QStringLiteral("vulkan")).ok);  // matched by LUID, not by name
        QVERIFY(gpus[0].apis.value(QStringLiteral("dx12")).ok);
        QCOMPARE(gpus[2].apis.value(QStringLiteral("dx12")).reason, QStringLiteral("Needs shader model 6.0"));
        QCOMPARE(defaultGpuIndex(gpus), std::optional<int>(0));
    }

    // ---- checkpoint 3 fix round ----

    // H3 (D22): a probe that gets no answer in time stores no entry, and nothing after it is probed.
    void glProbeTimeoutStoresNothing()
    {
        namespace pr = sb::gpu::protocol;
        GpuEntry a, b, c;
        a.id = QStringLiteral("a");
        b.id = QStringLiteral("b");
        c.id = QStringLiteral("c");
        a.vendorId = 0x10de;
        b.vendorId = 0x1002;
        c.vendorId = 0x8086;
        QVector<GpuEntry> list{a, b, c};
        QStringList probed;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        probeOpenGlOver(list, {0, 1, 2}, deadline, false, false, [&](const GpuEntry &g, std::chrono::milliseconds) -> std::optional<pr::GlEvent> {
            probed << g.id;
            if (g.id == QLatin1StringView("b"))
                return std::nullopt;  // ran out of time
            pr::GlEvent gl;
            gl.ok = true;
            gl.version = QStringLiteral("OpenGL 4.6");
            gl.renderer = QStringLiteral("NVIDIA GeForce");
            gl.vendor = QStringLiteral("NVIDIA Corporation");
            return gl;
        });
        QCOMPARE(probed, (QStringList{QStringLiteral("a"), QStringLiteral("b")}));  // c was never probed
        QVERIFY(list[0].apis.value(QStringLiteral("opengl")).ok);
        QVERIFY(!list[1].apis.contains(QStringLiteral("opengl")));  // no entry: its worker checks it at the start
        QVERIFY(!list[2].apis.contains(QStringLiteral("opengl")));
        // An expired deadline probes nothing; Linux ARM stores its fixed reason without a probe.
        QVector<GpuEntry> late{a};
        probeOpenGlOver(late, {0}, std::chrono::steady_clock::now() - std::chrono::seconds(1), false, false,
                        [&](const GpuEntry &, std::chrono::milliseconds) -> std::optional<pr::GlEvent> { return pr::GlEvent{}; });
        QVERIFY(late[0].apis.isEmpty());
        QVector<GpuEntry> arm{a};
        probeOpenGlOver(arm, {0}, deadline, true, false, [&](const GpuEntry &, std::chrono::milliseconds) -> std::optional<pr::GlEvent> {
            [] { QFAIL("probed on Linux ARM"); }();
            return std::nullopt;
        });
        QCOMPARE(arm[0].apis.value(QStringLiteral("opengl")).reason, QStringLiteral("OpenGL isn't tested on Linux ARM"));
    }

    // M2: the LUID is carried (probe list -> protocol -> the worker's arguments) so every API selects the same adapter.
    void luidIsCarried()
    {
        namespace pr = sb::gpu::protocol;
        DxgiAdapterRecord r;
        r.name = QStringLiteral("GPU");
        r.vendorId = 0x10de;
        r.deviceId = 0x2c05;
        r.dedicatedBytes = qint64(16) << 30;
        const quint8 luid[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
        std::memcpy(r.luid, luid, 8);
        const QVector<GpuEntry> gpus = mergeWindowsGpus({r}, {});
        QCOMPARE(gpus[0].luid, QStringLiteral("0123456789abcdef"));
        const auto e = pr::parseLine(pr::encode(pr::GpuEvent{gpus[0]}), nullptr);
        QVERIFY(e);
        QCOMPARE(std::get<pr::GpuEvent>(*e).gpu.luid, QStringLiteral("0123456789abcdef"));
        std::array<quint8, 8> back{};
        QVERIFY(parseLuidHex(gpus[0].luid, &back));
        QCOMPARE(std::memcmp(back.data(), luid, 8), 0);
        QVERIFY(!parseLuidHex(QStringLiteral("0123"), &back));
        QVERIFY(!parseLuidHex(QStringLiteral("zzzzzzzzzzzzzzzz"), &back));
        QCOMPARE(luidHex(luid), QStringLiteral("0123456789abcdef"));
    }

    // L5: UMA decides (or, without the answer, shared > dedicated on AMD and Intel); else the 512 MiB rule.
    void windowsKinds()
    {
        const auto kind = [](quint32 vendor, qint64 dedicated, qint64 shared, bool umaKnown, bool uma) {
            DxgiAdapterRecord r;
            r.vendorId = vendor;
            r.dedicatedBytes = dedicated;
            r.sharedBytes = shared;
            r.umaKnown = umaKnown;
            r.uma = uma;
            return mergeWindowsGpus({r}, {})[0].kind;
        };
        const qint64 GiB = qint64(1) << 30;
        QCOMPARE(kind(0x1002, 4 * GiB, 16 * GiB, true, true), GpuKind::Integrated);    // an APU with a big carve-out
        QCOMPARE(kind(0x1002, 8 * GiB, 16 * GiB, true, false), GpuKind::Discrete);     // a discrete Radeon: UMA says no
        QCOMPARE(kind(0x1002, 512 * 1024, 16 * GiB, false, false), GpuKind::Integrated);  // fallback: shared > dedicated
        QCOMPARE(kind(0x8086, 128 * 1024 * 1024, 16 * GiB, false, false), GpuKind::Integrated);
        // WARP reports UMA but is software: classified first, by the flag or by its ids.
        const auto warp = [](bool flag) {
            DxgiAdapterRecord r;
            r.vendorId = 0x1414;
            r.deviceId = 0x008c;
            r.software = flag;
            r.umaKnown = true;
            r.uma = true;
            return mergeWindowsGpus({r}, {})[0].kind;
        };
        QCOMPARE(warp(true), GpuKind::Software);
        QCOMPARE(warp(false), GpuKind::Software);
        QCOMPARE(kind(0x10de, 6 * GiB, 16 * GiB, false, false), GpuKind::Discrete);   // NVIDIA: the 512 MiB rule
        QCOMPARE(kind(0x10de, 256 * 1024 * 1024, 16 * GiB, false, false), GpuKind::Integrated);
    }

    // L3 and L6: a software GPU gets Mesa's software GL; the vendor table is case-insensitive.
    void softwareGlAndVendorTable()
    {
        GpuEntry lvp;
        lvp.kind = GpuKind::Software;
        lvp.apis.insert(QStringLiteral("vulkan"), ApiAvailability{true, QStringLiteral("Vulkan 1.4"), QStringLiteral("llvmpipe Mesa 26.2.4"), {}, {}, {}});
        QCOMPARE(glEnvironmentFor(lvp, false, [](const QString &) { return false; }).value(QStringLiteral("LIBGL_ALWAYS_SOFTWARE")), QStringLiteral("1"));
        QCOMPARE(glVendorToPci(QStringLiteral("nvidia corporation")), std::optional<quint32>(0x10de));
        QCOMPARE(glVendorToPci(QStringLiteral("advanced micro devices, inc.")), std::optional<quint32>(0x1002));
        QVERIFY(isSoftwareGl(QStringLiteral("LLVMPIPE (LLVM 21)")));
        QVERIFY(!isSoftwareGl(QStringLiteral("NVIDIA GeForce RTX 5070 Ti")));
    }

    // Dave's Wayland OpenGL rule: a GPU that drives no enabled display output is N/A on a Wayland session.
    void waylandGlNeedsADisplayOutput()
    {
        namespace pr = sb::gpu::protocol;
        const auto glFor = [](const char *vendor, const char *renderer) {
            pr::GlEvent g;
            g.ok = true;
            g.version = QStringLiteral("OpenGL 4.6");
            g.vendor = QLatin1StringView(vendor);
            g.renderer = QLatin1StringView(renderer);
            return g;
        };
        const QString reason = QStringLiteral("OpenGL's test window can't be shown from this GPU on this Wayland session (it doesn't drive a display)");
        // Tiamat: the 5070 Ti drives DP-2/DP-3; the 610M's only connected connector is disabled.
        const QVector<GpuEntry> tiamat = mergeLinuxGpus(tiamatVulkan(), readDrmCards(fixtureRoot("/linux/tiamat")));
        QCOMPARE(tiamat[0].drivesDisplay, std::optional<bool>(false));
        QCOMPARE(tiamat[1].drivesDisplay, std::optional<bool>(true));
        const pr::GlEvent amdGl = glFor("AMD", "AMD Radeon Graphics (radeonsi, raphael_mendocino)");
        const pr::GlEvent nvGl = glFor("NVIDIA Corporation", "NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2");
        ApiAvailability a = verifyGl(amdGl, tiamat[0], tiamat, false, false, true);
        QVERIFY(!a.ok);
        QCOMPARE(a.reason, reason);
        QVERIFY(verifyGl(nvGl, tiamat[1], tiamat, false, false, true).ok);
        // X11 (or any non-Wayland session): the rule doesn't apply.
        QVERIFY(verifyGl(amdGl, tiamat[0], tiamat, false, false, false).ok);
        // A laptop: the iGPU drives the panel, the dGPU nothing.
        const QVector<DrmCard> cards = readDrmCards(fixtureRoot("/linux/laptop"));
        QCOMPARE(cards.size(), 2);
        const QVector<GpuEntry> laptop = mergeLinuxGpus({}, cards);
        QCOMPARE(laptop[0].drivesDisplay, std::optional<bool>(true));   // Intel, eDP-1 connected and enabled
        QCOMPARE(laptop[1].drivesDisplay, std::optional<bool>(false));  // NVIDIA, HDMI disconnected
        QVERIFY(verifyGl(glFor("Intel", "Mesa Intel(R) Graphics"), laptop[0], laptop, false, false, true).ok);
        QCOMPARE(verifyGl(nvGl, laptop[1], laptop, false, false, true).reason, reason);
        QVERIFY(verifyGl(nvGl, laptop[1], laptop, false, false, false).ok);
        // Unknown (no DRM card: a Vulkan-only device) and software GPUs are never N/A by this rule.
        GpuEntry unknown = tiamat[0];
        unknown.drivesDisplay.reset();
        QVERIFY(verifyGl(amdGl, unknown, {unknown}, false, false, true).ok);
        // The worker's own check reads the same sysfs by PCI slot.
        const std::filesystem::path tiamatRoot = fixtureRoot("/linux/tiamat").toStdString();
        QCOMPARE(drmGpuDrivesDisplay(tiamatRoot, "0000:79:00.0"), std::optional<bool>(false));
        QCOMPARE(drmGpuDrivesDisplay(tiamatRoot, "0000:01:00.0"), std::optional<bool>(true));
        QVERIFY(!drmGpuDrivesDisplay(tiamatRoot, "0000:99:00.0").has_value());
        // The flag survives the protocol.
        const auto e = pr::parseLine(pr::encode(pr::GpuEvent{tiamat[0]}), nullptr);
        QVERIFY(e);
        QCOMPARE(std::get<pr::GpuEvent>(*e).gpu.drivesDisplay, std::optional<bool>(false));
    }

    // D20: the reference is the API that matches the most others, ties to the earlier one.
    void judge()
    {
        const auto sig = [](char c) { return QByteArray(432, c); };
        const QByteArray a = sig(0x10), b = sig(0x60);  // 80/255 apart: no match
        const auto verdict = [&](QVector<QByteArray> v) { return judgeSignatures(v); };
        SignatureVerdict v = verdict({a, a, b, a});
        QCOMPARE(v.reference, 0);
        QCOMPARE(v.outliers, (QVector<int>{2}));
        v = verdict({a, b});
        QCOMPARE(v.reference, 0);
        QCOMPARE(v.outliers, (QVector<int>{1}));
        v = verdict({a, b, b, b});
        QCOMPARE(v.reference, 1);
        QCOMPARE(v.outliers, (QVector<int>{0}));
        v = verdict({a});
        QCOMPARE(v.reference, 0);
        QVERIFY(v.outliers.isEmpty());
        v = verdict({a, QByteArray(), a});
        QCOMPARE(v.reference, 0);
        QVERIFY(v.outliers.isEmpty());
        v = verdict({QByteArray(), QByteArray()});
        QCOMPARE(v.reference, -1);
        // Within the tolerance is a match (mean 4, max 4).
        v = verdict({sig(0x10), sig(0x14)});
        QVERIFY(v.outliers.isEmpty());
        v = verdict({a, b});
        QVERIFY(v.diffToReference[1].mean > 70.0);
    }

    void planText()
    {
        namespace pr = sb::gpu::protocol;
        const QString text = QStringLiteral("geometry:5000:20000,drawcalls:3000:20000");
        QString err;
        const auto plan = pr::parsePlan(text, &err);
        QVERIFY2(plan, qPrintable(err));
        QCOMPARE(plan->size(), 2);
        QCOMPARE((*plan)[1], (pr::PlanItem{QStringLiteral("drawcalls"), 3000, 20000}));
        QCOMPARE(pr::planText(*plan), text);
        QVERIFY(!pr::parsePlan(QStringLiteral("geometry:5000"), &err));
        QCOMPARE(err, QStringLiteral("bad plan item 'geometry:5000'"));
        const auto empty = pr::parsePlan(QString(), &err);
        QVERIFY(empty && empty->isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestGpuModel)
#include "tst_gpu_model.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test's pure model (Phase 4a design §9, §10): ids and keys, frame
// statistics, best-of and the signature comparison.
#include "tests/gpu/model/best_of.h"
#include "tests/gpu/model/frame_stats.h"
#include "tests/gpu/model/gpu_identity.h"
#include "tests/gpu/model/ids.h"
#include "tests/gpu/model/protocol.h"
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
        QCOMPARE(r.series.size(), 2);
        QVERIFY(std::abs(r.series[1].y() - 2.39) < 1e-9);
        const auto gg = std::get<pr::GpuEvent>(*pr::parseLine(pr::encode(pr::GpuEvent{g}), nullptr));
        QCOMPARE(gg.gpu, g);
        const auto rr = std::get<pr::ReadyEvent>(*pr::parseLine(pr::encode(ready), nullptr));
        QVERIFY(std::abs(rr.scale - 1.25) < 1e-9 && rr.gpuTimestamps && !rr.fit && rr.screen == QStringLiteral("DP-3"));
        // A result without GPU timestamps has no gpuMs.
        res.gpuMs.reset();
        QVERIFY(!std::get<pr::ResultEvent>(*pr::parseLine(pr::encode(res), nullptr)).gpuMs);
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

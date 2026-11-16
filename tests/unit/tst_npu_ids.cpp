// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU test's pure ids/keys and units (Phase 4c design §9, §14.1): every
// stable metric key, the workload/vendor round-trips, the N/A-reasons taxonomy
// (§3.3, declared in detect.h), and the `img/s` / `tok/s` canonical-unit
// additions.
#include "core/units.h"
#include "tests/npu/detect/detect.h"
#include "tests/npu/ids.h"

#include <QTest>

using namespace sb::npu;

class TestNpuIds : public QObject
{
    Q_OBJECT

private slots:
    void subTestIds()
    {
        QCOMPARE(workloadId(Workload::Classify), QStringLiteral("classify"));
        QCOMPARE(workloadId(Workload::Detect), QStringLiteral("detect"));
        QCOMPARE(workloadId(Workload::Llm), QStringLiteral("llm"));
        QCOMPARE(workloadLabel(Workload::Classify), QStringLiteral("Classify"));
        QCOMPARE(workloadLabel(Workload::Detect), QStringLiteral("Detect"));
        QCOMPARE(workloadLabel(Workload::Llm), QStringLiteral("LLM"));
    }

    void headlineKeys()
    {
        QCOMPARE(key(Workload::Classify), QStringLiteral("classify.imgs"));
        QCOMPARE(key(Workload::Detect), QStringLiteral("detect.imgs"));
        QCOMPARE(key(Workload::Llm), QStringLiteral("llm.tps"));
    }

    void metricKeys()
    {
        QCOMPARE(key(Workload::Classify, Field::Imgs), QStringLiteral("classify.imgs"));
        QCOMPARE(key(Workload::Classify, Field::Img0), QStringLiteral("classify.img0"));
        QCOMPARE(key(Workload::Classify, Field::Img1), QStringLiteral("classify.img1"));
        QCOMPARE(key(Workload::Classify, Field::Img2), QStringLiteral("classify.img2"));
        QCOMPARE(key(Workload::Detect, Field::Imgs), QStringLiteral("detect.imgs"));
        QCOMPARE(key(Workload::Detect, Field::Img0), QStringLiteral("detect.img0"));
        QCOMPARE(key(Workload::Llm, Field::Tps), QStringLiteral("llm.tps"));
        QCOMPARE(key(Workload::Llm, Field::Ttft), QStringLiteral("llm.ttft"));
        QCOMPARE(key(Workload::Llm, Field::Tps0), QStringLiteral("llm.tps0"));
        QCOMPARE(key(Workload::Llm, Field::Tps1), QStringLiteral("llm.tps1"));
        QCOMPARE(key(Workload::Llm, Field::Tps2), QStringLiteral("llm.tps2"));
    }

    void labelsAndUnits()
    {
        QCOMPARE(label(Workload::Classify, Field::Imgs), QStringLiteral("Classification throughput"));
        QCOMPARE(label(Workload::Detect, Field::Imgs), QStringLiteral("Detection throughput"));
        QCOMPARE(label(Workload::Llm, Field::Tps), QStringLiteral("Generation throughput"));
        QCOMPARE(label(Workload::Llm, Field::Ttft), QStringLiteral("Time to first token"));
        QCOMPARE(label(Workload::Classify, Field::Img0), QStringLiteral("per-window images/s (raw)"));
        QCOMPARE(label(Workload::Llm, Field::Tps0), QStringLiteral("per-window tokens/s (raw)"));

        QCOMPARE(unit(Workload::Classify, Field::Imgs), QStringLiteral("img/s"));
        QCOMPARE(unit(Workload::Detect, Field::Imgs), QStringLiteral("img/s"));
        QCOMPARE(unit(Workload::Llm, Field::Tps), QStringLiteral("tok/s"));
        QCOMPARE(unit(Workload::Llm, Field::Ttft), QStringLiteral("ms"));
        QCOMPARE(unit(Workload::Classify, Field::Img0), QStringLiteral("img/s"));
        QCOMPARE(unit(Workload::Llm, Field::Tps0), QStringLiteral("tok/s"));

        QVERIFY(!lowerIsBetter(Workload::Classify, Field::Imgs));
        QVERIFY(!lowerIsBetter(Workload::Llm, Field::Tps));
        QVERIFY(lowerIsBetter(Workload::Llm, Field::Ttft));
        QVERIFY(!lowerIsBetter(Workload::Detect, Field::Img0));
    }

    void neutralKeys()
    {
        QCOMPARE(kDeviceVendor, QStringLiteral("device.vendor"));
        QCOMPARE(kDeviceName, QStringLiteral("device.name"));
        QCOMPARE(kDeviceDriver, QStringLiteral("device.driver"));
        QCOMPARE(kDeviceRuntime, QStringLiteral("device.runtime"));
        QCOMPARE(kEngineName, QStringLiteral("engine.name"));
        QCOMPARE(kModelClassify, QStringLiteral("model.classify"));
        QCOMPARE(kModelDetect, QStringLiteral("model.detect"));
        QCOMPARE(kModelLlm, QStringLiteral("model.llm"));
        QCOMPARE(kModelsPackVersion, QStringLiteral("models.pack_version"));
        QCOMPARE(kStabilitySpread, QStringLiteral("stability.spread"));
        QCOMPARE(kPowerSource, QStringLiteral("power.source"));
    }

    void vendors()
    {
        QCOMPARE(vendorId(NpuVendor::Intel), QStringLiteral("intel"));
        QCOMPARE(vendorId(NpuVendor::Amd), QStringLiteral("amd"));
        QCOMPARE(vendorId(NpuVendor::Qualcomm), QStringLiteral("qualcomm"));
        QCOMPARE(vendorId(NpuVendor::Null), QStringLiteral("null"));
        QCOMPARE(vendorName(NpuVendor::Intel), QStringLiteral("Intel"));
        QCOMPARE(vendorName(NpuVendor::Amd), QStringLiteral("AMD"));
        QCOMPARE(vendorName(NpuVendor::Qualcomm), QStringLiteral("Qualcomm"));
        QCOMPARE(vendorName(NpuVendor::Null), QStringLiteral("Null"));
    }

    void roundTrips()
    {
        for (Workload w : kWorkloads)
            QCOMPARE(workloadFromId(QStringView(workloadId(w))), std::optional<Workload>(w));
        QVERIFY(!workloadFromId(u"nope"));

        for (NpuVendor v : {NpuVendor::Intel, NpuVendor::Amd, NpuVendor::Qualcomm})
            QCOMPARE(vendorFromId(QStringView(vendorId(v))), std::optional<NpuVendor>(v));
        QVERIFY(!vendorFromId(u"nvidia"));
        QVERIFY(!vendorFromId(u"null"));   // "null" is the internal sentinel, not a pack tag
    }

    void canonicalUnits()
    {
        QVERIFY(sb::Units::canonical().contains(QStringLiteral("img/s")));
        QVERIFY(sb::Units::canonical().contains(QStringLiteral("tok/s")));
    }

    // The N/A-reasons taxonomy (§3.3, §14.1): key + user string, with the
    // %1/%2/%3 placeholders.
    void taxonomy()
    {
        QCOMPARE(reasonKey(NaReason::None), QString());
        QCOMPARE(reasonKey(NaReason::NoNpu), QStringLiteral("no_npu"));
        QCOMPARE(reasonKey(NaReason::DriverMissing), QStringLiteral("driver_missing"));
        QCOMPARE(reasonKey(NaReason::RuntimeMissing), QStringLiteral("runtime_missing"));
        QCOMPARE(reasonKey(NaReason::ModelMissing), QStringLiteral("model_missing"));
        QCOMPARE(reasonKey(NaReason::UnsupportedVendor), QStringLiteral("unsupported_vendor"));
        QCOMPARE(reasonKey(NaReason::RuntimeLoadFailed), QStringLiteral("runtime_load_failed"));

        QCOMPARE(reasonString(NaReason::NoNpu), QStringLiteral("No supported NPU found on this machine"));
        QCOMPARE(reasonString(NaReason::DriverMissing, {QStringLiteral("Intel")}),
                 QStringLiteral("an Intel NPU is present but its driver isn't installed"));
        QCOMPARE(reasonString(NaReason::RuntimeMissing, {QStringLiteral("AMD")}),
                 QStringLiteral("the AMD runtime isn't installed"));
        QCOMPARE(reasonString(NaReason::ModelMissing, {QStringLiteral("AMD")}),
                 QStringLiteral("the AMD model pack isn't downloaded (offline or blocked)"));
        QCOMPARE(reasonString(NaReason::UnsupportedVendor, {QStringLiteral("Qualcomm"), QStringLiteral("Linux")}),
                 QStringLiteral("Qualcomm isn't supported on Linux"));
        QCOMPARE(reasonString(NaReason::RuntimeLoadFailed, {QStringLiteral("AMD"), QString(), QStringLiteral("symbol lookup failed")}),
                 QStringLiteral("the AMD runtime failed to load: symbol lookup failed"));
        // The 2-arg {vendor, error} call must also carry the error (L1), and an
        // arg's own "%2"/"%3" text must not be re-substituted.
        QCOMPARE(reasonString(NaReason::RuntimeLoadFailed, {QStringLiteral("AMD"), QStringLiteral("LoadLibrary error 0x7E (%3)")}),
                 QStringLiteral("the AMD runtime failed to load: LoadLibrary error 0x7E (%3)"));
    }
};

QTEST_GUILESS_MAIN(TestNpuIds)
#include "tst_npu_ids.moc"

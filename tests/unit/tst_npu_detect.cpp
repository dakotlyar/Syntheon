// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU vendor + runtime detection (Phase 4c design §3, §14.1, §15): the
// snapshot's `npus[]` parsed against fixture snapshots, the primary-vendor
// selection (Intel → AMD → Qualcomm), the platform gates (Qualcomm is Windows-
// on-ARM only, N4; Intel is Windows-only, D1a), and the runtime probes run
// against a synthetic `root` so the present/absent outcomes don't depend on
// the dev machine's real packages. The per-OS runtime probes are guarded so
// this test runs on whatever platform the build targets.
#include "tests/npu/detect/detect.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QTemporaryDir>

using namespace sb;
using namespace sb::npu;

namespace {

// A snapshot carrying the given NPU entries (empty -> no npus).
SystemSnapshot snapshotWith(QList<QJsonObject> npus)
{
    SystemSnapshot s;
    QJsonArray arr;
    for (const QJsonObject &n : npus)
        arr.append(n);
    s.data.insert(QStringLiteral("npus"), arr);
    return s;
}

// A generic NPU entry from a friendly name, manufacturer string and driver
// (empty fields are omitted, mirroring what a real collector reports).
QJsonObject npuEntry(const QString &name, const QString &vendor, const QString &driver)
{
    QJsonObject o;
    if (!name.isEmpty())
        o.insert(QStringLiteral("name"), name);
    if (!vendor.isEmpty())
        o.insert(QStringLiteral("vendor"), vendor);
    if (!driver.isEmpty())
        o.insert(QStringLiteral("driver"), driver);
    return o;
}

QJsonObject intel()
{
    return npuEntry(QStringLiteral("Intel NPU"), QStringLiteral("Intel"), QStringLiteral("intel_vpu"));
}

QJsonObject amd(const QString &driver = QStringLiteral("amdxdna"))
{
    return npuEntry(QStringLiteral("AMD XDNA NPU"), QStringLiteral("AMD"), driver);
}

QJsonObject qualcomm()
{
    return npuEntry(QStringLiteral("Qualcomm Hexagon NPU"), QStringLiteral("Qualcomm"), QStringLiteral("qnn"));
}

// Create the file at `path` (making parent dirs), used to lay out a synthetic
// runtime tree under a QTemporaryDir.
void touch(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.close();
}

} // namespace

class TestNpuDetect : public QObject
{
    Q_OBJECT

private slots:
    void emptyIsNoNpu()
    {
        const NpuDetect d = detect(snapshotWith({}));
        QCOMPARE(d.vendor, NpuVendor::Null);
        QCOMPARE(d.reason, NaReason::NoNpu);
    }

    // A recognised vendor whose entry has no bound driver → DriverMissing (not
    // Null): the AMD name-only entry from §3.2/AP-5 H2.
    void amdNameOnlyIsDriverMissing()
    {
        const NpuDetect d = detect(snapshotWith({npuEntry(QStringLiteral("AMD XDNA NPU"), QString(), QString())}));
        QCOMPARE(d.vendor, NpuVendor::Amd);
        QCOMPARE(d.reason, NaReason::DriverMissing);
    }

    // Real snapshot strings (Windows SPDRP_MFG/friendly-name and the WoA ACPI
    // device) must still resolve to the right vendor via name/driver, not the
    // exact short vendor string (§3.2, AP-5 H2).
    void realSnapshotVendorMatching()
    {
        // WoA Snapdragon X: vendor "Qualcomm Technologies, Inc.", name carries
        // "Qualcomm", ACPI device (no PCI VEN_).
        const NpuDetect q = detect(snapshotWith({npuEntry(QStringLiteral("Qualcomm(R) Hexagon(TM) NPU"),
                                                        QStringLiteral("Qualcomm Technologies, Inc."),
                                                        QStringLiteral("qcnspmcdm"))}));
        QCOMPARE(q.vendor, NpuVendor::Qualcomm);

        // AMD: manufacturer string may be "Advanced Micro Devices, Inc.", the
        // name + amdxdna driver are the signal.
        const NpuDetect a = detect(snapshotWith({npuEntry(QStringLiteral("AMD XDNA NPU"),
                                                      QStringLiteral("Advanced Micro Devices, Inc."),
                                                      QStringLiteral("amdxdna"))}));
        QCOMPARE(a.vendor, NpuVendor::Amd);

        // Intel: the intel_vpu driver (vended name may differ).
        const NpuDetect i = detect(snapshotWith({npuEntry(QStringLiteral("Intel AI Boost"),
                                                      QStringLiteral("Intel Corporation"),
                                                      QStringLiteral("intel_vpu"))}));
        QCOMPARE(i.vendor, NpuVendor::Intel);
    }

    void firstVendorWins()
    {
        // Both Intel and AMD present: Intel is picked (N3).
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({amd(), intel()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Intel);
    }

    void qualcommUnsupportedOffArm()
    {
        // The dev/CI box is x86_64 (or an ARM64 build of a non-Windows OS),
        // where Qualcomm QNN has no runtime (N4): UnsupportedVendor everywhere.
#if defined(Q_OS_WIN) && (defined(Q_PROCESSOR_ARM_64) || defined(_M_ARM64))
        QSKIP("WoA ARM64 build: Qualcomm is supported here");
#endif
        const NpuDetect d = detect(snapshotWith({qualcomm()}));
        QCOMPARE(d.vendor, NpuVendor::Qualcomm);
        QCOMPARE(d.reason, NaReason::UnsupportedVendor);
    }

    // ---- Linux (D1a): AMD XDNA only; Intel is UnsupportedVendor ----
#if defined(Q_OS_LINUX)
    void linuxIntelUnsupported()
    {
        const NpuDetect d = detect(snapshotWith({intel()}));
        QCOMPARE(d.vendor, NpuVendor::Intel);
        QCOMPARE(d.reason, NaReason::UnsupportedVendor);
    }

    void linuxAmdRuntimeMissing()
    {
        QTemporaryDir root;   // no /opt/xilinx/xrt under it
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({amd()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Amd);
        QCOMPARE(d.reason, NaReason::RuntimeMissing);
    }

    void linuxAmdRuntimePresent()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("opt/xilinx/xrt"))));
        const NpuDetect d = detect(snapshotWith({amd()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Amd);
        QCOMPARE(d.reason, NaReason::None);
        QVERIFY(d.runtimePath.startsWith(root.path()));
    }
#endif

    // ---- Windows §3.2 runtime fixtures ----
#if defined(Q_OS_WIN)
    void winIntelRuntimeMissing()
    {
        QTemporaryDir root;   // empty: no OpenVINO anywhere under it
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({intel()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Intel);
        QCOMPARE(d.reason, NaReason::RuntimeMissing);
    }

    void winIntelRuntimePresent()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString d = root.filePath(QStringLiteral("Program Files/Intel/openvino/runtime/bin/intel64/Release"));
        touch(d + QStringLiteral("/openvino.dll"));
        touch(d + QStringLiteral("/openvino_intel_npu_plugin.dll"));
        const NpuDetect r = detect(snapshotWith({intel()}), root.path());
        QCOMPARE(r.vendor, NpuVendor::Intel);
        QCOMPARE(r.reason, NaReason::None);
        QVERIFY(r.runtimePath.startsWith(root.path()));
    }

    void winAmdRuntimeMissing()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({amd()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Amd);
        QCOMPARE(d.reason, NaReason::RuntimeMissing);
    }

    void winAmdRuntimePresent()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString d = root.filePath(QStringLiteral("Program Files/RyzenAI/ryzen-ai-1.9.0/deployment"));
        touch(d + QStringLiteral("/onnxruntime.dll"));
        touch(d + QStringLiteral("/vitisai_ep.dll"));
        const NpuDetect r = detect(snapshotWith({amd()}), root.path());
        QCOMPARE(r.vendor, NpuVendor::Amd);
        QCOMPARE(r.reason, NaReason::None);
        QVERIFY(r.runtimePath.startsWith(root.path()));
    }

#if defined(Q_PROCESSOR_ARM_64) || defined(_M_ARM64)
    void winQualcommRuntimeMissing()
    {
        QTemporaryDir root;   // no QnnHtp.dll anywhere under it
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({qualcomm()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Qualcomm);
        QCOMPARE(d.reason, NaReason::RuntimeMissing);
    }

    void winQualcommRuntimePresent()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString d = root.filePath(QStringLiteral("Qualcomm/AIStack/QAIRT/2.45.0"));
        touch(d + QStringLiteral("/lib/aarch64-windows-msvc/QnnHtp.dll"));
        const NpuDetect r = detect(snapshotWith({qualcomm()}), root.path());
        QCOMPARE(r.vendor, NpuVendor::Qualcomm);
        QCOMPARE(r.reason, NaReason::None);
        QVERIFY(r.runtimePath.startsWith(root.path()));
    }
#endif  // ARM64
#endif  // Q_OS_WIN
};

QTEST_GUILESS_MAIN(TestNpuDetect)
#include "tst_npu_detect.moc"

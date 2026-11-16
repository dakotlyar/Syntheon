// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU vendor + runtime detection (Phase 4c design §3, §14.1, §15): the
// snapshot's `npus[]` parsed against fixture snapshots, the primary-vendor
// selection (Intel → AMD → Qualcomm), the platform gate (Qualcomm is Windows-
// on-ARM only, N4), and the runtime probes run against a synthetic `root` so
// the present/absent outcomes don't depend on the dev machine's real packages.
// Intel runs on both Windows and Linux (D1b, build-only/unvalidated on Linux).
// The per-OS runtime probes are guarded so this test runs on whatever platform
// the build targets.
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

    // Real snapshot strings (Windows SPDRP_MFG/friendly-name + PCI vendor, the
    // WoA ACPI device, and empty-field entries per §15) must still resolve to
    // the right vendor via vendor/driver/name, not the exact short vendor
    // string or a Linux-only driver (§3.2, AP-5 H2/M2).
    void realSnapshotVendorMatching()
    {
        // Knowhere Windows Ryzen AI: vendor "AMD" via pciVendorName(0x1022),
        // IpuMcdmDriver service, factory friendly-name (no "AMD XDNA" text).
        const NpuDetect knowhere = detect(snapshotWith({npuEntry(
            QStringLiteral("NPU Compute Accelerator Device"),
            QStringLiteral("AMD"),
            QStringLiteral("IpuMcdmDriver"))}));
        QCOMPARE(knowhere.vendor, NpuVendor::Amd);

        // Nivedellir Windows: real Intel AI Boost snapshot.
        const NpuDetect nivedellir = detect(snapshotWith({npuEntry(
            QStringLiteral("Intel(R) AI Boost"),
            QStringLiteral("Intel"),
            QStringLiteral("npu"))}));
        QCOMPARE(nivedellir.vendor, NpuVendor::Intel);

        // Dragonfire WoA Snapdragon X: manufacturer "Qualcomm Technologies,
        // Inc." with no PCI VEN_, name/driver carry the signal.
        const NpuDetect dragonfire = detect(snapshotWith({npuEntry(
            QStringLiteral("Snapdragon(R) X Elite - X1E78100 - Qualcomm(R) Hexagon(TM) NPU"),
            QStringLiteral("Qualcomm Technologies, Inc."),
            QStringLiteral("qcnspmcdm"))}));
        QCOMPARE(dragonfire.vendor, NpuVendor::Qualcomm);

        // Design §15: a Qualcomm entry with only the vendor string (empty name
        // and driver) must still match, not fall through to Null.
        const NpuDetect woavendoronly = detect(snapshotWith({npuEntry(
            QString(), QStringLiteral("Qualcomm"), QString())}));
        QCOMPARE(woavendoronly.vendor, NpuVendor::Qualcomm);

        // AMD with no bound driver (driverless PCI\VEN_1022) → Amd, and the
        // empty driver resolves to DriverMissing, not Null.
        const NpuDetect amdNoDriver = detect(snapshotWith({npuEntry(
            QStringLiteral("NPU Compute Accelerator Device"),
            QStringLiteral("AMD"),
            QString())}));
        QCOMPARE(amdNoDriver.vendor, NpuVendor::Amd);
        QCOMPARE(amdNoDriver.reason, NaReason::DriverMissing);
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

    // ---- Linux (D1b): Intel (OpenVINO .so, build-only/unvalidated) + AMD XDNA ----
#if defined(Q_OS_LINUX)
    void linuxIntelRuntimeMissing()
    {
        QTemporaryDir root;   // no OpenVINO libs anywhere under it
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({intel()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Intel);
        QCOMPARE(d.reason, NaReason::RuntimeMissing);
    }

    void linuxIntelRuntimePresent()
    {
        // The /opt/intel tarball layout: flat unversioned libopenvino.so +
        // libopenvino_c.so + the NPU plugin beside them.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString d = root.filePath(QStringLiteral("opt/intel/openvino/runtime/lib/intel64"));
        touch(d + QStringLiteral("/libopenvino.so"));
        touch(d + QStringLiteral("/libopenvino_c.so"));
        touch(d + QStringLiteral("/libopenvino_intel_npu_plugin.so"));
        const NpuDetect r = detect(snapshotWith({intel()}), root.path());
        QCOMPARE(r.vendor, NpuVendor::Intel);
        QCOMPARE(r.reason, NaReason::None);
        QVERIFY(r.runtimePath.startsWith(root.path()));
    }

    void linuxIntelPackagedRuntimePresent()
    {
        // APT/DEB & RPM ship versioned SONAME files (the unversioned .so is the
        // -dev namelink) and the NPU plugin under openvino-<ver>/, not flat — the
        // probe and engine must match that layout, not only the tarball (M1).
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString d = root.filePath(QStringLiteral("usr/lib/x86_64-linux-gnu"));
        touch(d + QStringLiteral("/libopenvino.so.2024.5.0"));
        touch(d + QStringLiteral("/libopenvino_c.so.2024.5.0"));
        touch(d + QStringLiteral("/openvino-2024.5.0/libopenvino_intel_npu_plugin.so"));
        const NpuDetect r = detect(snapshotWith({intel()}), root.path());
        QCOMPARE(r.vendor, NpuVendor::Intel);
        QCOMPARE(r.reason, NaReason::None);
        QVERIFY(r.runtimePath.startsWith(root.path()));
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
        // The probe checks the exact file the engine dlopens (L-XRT), so an empty
        // /opt/xilinx/xrt dir alone must NOT report Ready.
        touch(root.filePath(QStringLiteral("opt/xilinx/xrt/lib/libxrt_coreutil.so")));
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
        // A sibling "Intel Corporation" (created by standard Intel/Killer/IPF
        // drivers) sorts above "Intel" under reversed name order; the probe
        // must still resolve the exact Intel runtime dir's DLLs (AP-5 CP1).
        touch(root.filePath(QStringLiteral("Program Files/Intel Corporation/placeholder")));
        const QString d = root.filePath(QStringLiteral("Program Files/Intel/openvino/runtime/bin/intel64/Release"));
        touch(d + QStringLiteral("/openvino.dll"));
        touch(d + QStringLiteral("/openvino_c.dll"));
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
        // "logs"/"uninstall" siblings sort above the numeric "2.45.0" in
        // reversed name order; the empty-prefix scan must loop past them to the
        // version dir that actually holds the DLL.
        touch(root.filePath(QStringLiteral("Qualcomm/AIStack/QAIRT/logs/placeholder")));
        touch(root.filePath(QStringLiteral("Qualcomm/AIStack/QAIRT/uninstall/placeholder")));
        const QString d = root.filePath(QStringLiteral("Qualcomm/AIStack/QAIRT/2.45.0"));
        touch(d + QStringLiteral("/lib/aarch64-windows-msvc/QnnHtp.dll"));
        touch(d + QStringLiteral("/lib/aarch64-windows-msvc/QnnSystem.dll"));
        const NpuDetect r = detect(snapshotWith({qualcomm()}), root.path());
        QCOMPARE(r.vendor, NpuVendor::Qualcomm);
        QCOMPARE(r.reason, NaReason::None);
        QVERIFY(r.runtimePath.startsWith(root.path()));
        // The engine loads both DLLs by exact path from runtimePath, so detect
        // must hand back the DLL dir, not the SDK root (H1).
        QVERIFY(r.runtimePath.endsWith(QLatin1StringView("/lib/aarch64-windows-msvc")));
    }
#endif  // ARM64
#endif  // Q_OS_WIN
};

QTEST_GUILESS_MAIN(TestNpuDetect)
#include "tst_npu_detect.moc"

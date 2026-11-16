// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU vendor + runtime detection (Phase 4c design §3, §14.1, §15): the
// snapshot's `npus[]` parsed against fixture snapshots, the primary-vendor
// selection (Intel → AMD → Qualcomm), the platform gate (Qualcomm is Windows-
// on-ARM only), and the runtime probes run against a synthetic `root` so the
// present/absent outcomes don't depend on the dev machine's real packages.
#include "tests/npu/detect/detect.h"

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

QJsonObject intel(const QString &driver = QStringLiteral("intel_vpu"))
{
    return QJsonObject{{QStringLiteral("name"), QStringLiteral("Intel NPU")},
                       {QStringLiteral("vendor"), QStringLiteral("Intel")},
                       {QStringLiteral("driver"), driver},
                       {QStringLiteral("driver_version"), QStringLiteral("1.0")}};
}

QJsonObject amd(const QString &driver = QStringLiteral("amdxdna"))
{
    return QJsonObject{{QStringLiteral("name"), QStringLiteral("AMD XDNA NPU")},
                       {QStringLiteral("driver"), driver},
                       {QStringLiteral("driver_version"), QStringLiteral("1.0")}};
}

QJsonObject qualcomm()
{
    return QJsonObject{{QStringLiteral("name"), QStringLiteral("Qualcomm Hexagon NPU")},
                       {QStringLiteral("vendor"), QStringLiteral("Qualcomm")},
                       {QStringLiteral("driver"), QStringLiteral("qnn")}};
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

    void intelRuntimeMissing()
    {
        QTemporaryDir root;   // empty: no OpenVINO anywhere under it
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({intel()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Intel);
        QCOMPARE(d.reason, NaReason::RuntimeMissing);
    }

    void intelRuntimePresent()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString lib = root.filePath(QStringLiteral("usr/lib"));
        QVERIFY(QDir().mkpath(lib));
        QFile f1(lib + QStringLiteral("/libopenvino.so"));
        QFile f2(lib + QStringLiteral("/openvino_intel_npu_plugin.so"));
        QVERIFY(f1.open(QIODevice::WriteOnly));
        QVERIFY(f2.open(QIODevice::WriteOnly));
        const NpuDetect d = detect(snapshotWith({intel()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Intel);
        QCOMPARE(d.reason, NaReason::None);
        QVERIFY(d.runtimePath.startsWith(root.path()));
    }

    void amdLinuxRuntimeMissing()
    {
        QTemporaryDir root;   // no /opt/xilinx/xrt
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({amd()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Amd);
        QCOMPARE(d.reason, NaReason::RuntimeMissing);
    }

    void amdLinuxRuntimePresent()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("opt/xilinx/xrt"))));
        const NpuDetect d = detect(snapshotWith({amd()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Amd);
        QCOMPARE(d.reason, NaReason::None);
        QVERIFY(d.runtimePath.startsWith(root.path()));
    }

    void qualcommUnsupportedOnX64()
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

    void firstVendorWins()
    {
        // Both Intel and AMD present: Intel is picked (N3).
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const NpuDetect d = detect(snapshotWith({amd(), intel()}), root.path());
        QCOMPARE(d.vendor, NpuVendor::Intel);
    }
};

QTEST_GUILESS_MAIN(TestNpuDetect)
#include "tst_npu_detect.moc"

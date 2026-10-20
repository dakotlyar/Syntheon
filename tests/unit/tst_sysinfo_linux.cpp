// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/collector.h"
#include "sysinfo/linux/sysfs_readers.h"

#include "fixture_tree.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace sb;
using namespace sb::sysinfo::linux_sysfs;

namespace {
// Materialised (decoded) copies of the fixture trees; see fixture_tree.h.
QTemporaryDir *g_trees = nullptr;
QString kKnowhere, kArm, kIntel;
} // namespace

// The Linux tree readers over fixture trees (real: Knowhere; synthetic: an
// ARM big.LITTLE board and an Intel hybrid CPU). Linux-only: the fixtures use
// symlinks, as sysfs does.
class TestSysinfoLinux : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        g_trees = new QTemporaryDir;
        for (const char *name : {"knowhere", "arm64-biglittle", "intel-hybrid"}) {
            QVERIFY(sb::testing::materializeFixture(QStringLiteral(SB_FIXTURES "/linux/") + QLatin1StringView(name),
                                                    g_trees->filePath(QLatin1StringView(name))));
        }
        kKnowhere = g_trees->filePath(QStringLiteral("knowhere"));
        kArm = g_trees->filePath(QStringLiteral("arm64-biglittle"));
        kIntel = g_trees->filePath(QStringLiteral("intel-hybrid"));
        // The real sysfs names came back.
        QVERIFY(QFileInfo(kKnowhere + QStringLiteral("/sys/dev/block/259:7")).isSymLink());
        QVERIFY(QFileInfo::exists(kKnowhere + QStringLiteral("/sys/class/power_supply/ucsi-source-psy-USBC000:001/online")));
    }

    void cleanupTestCase() { delete g_trees; }

    void decodeNames()
    {
        QCOMPARE(sb::testing::decodeFixtureName(QStringLiteral("ucsi-source-psy-USBC000%3A001")),
                 QStringLiteral("ucsi-source-psy-USBC000:001"));
        QCOMPARE(sb::testing::decodeFixturePath(QStringLiteral("../pci0000%3A00/100%25")),
                 QStringLiteral("../pci0000:00/100%"));
    }

    // The USB-C (ucsi) supplies have scope=Device: they must never count as
    // the machine's AC power, even when online.
    void ucsiSupplyExcludedFromAc()
    {
        QTemporaryDir copy;
        QVERIFY(sb::testing::materializeFixture(QStringLiteral(SB_FIXTURES "/linux/knowhere"), copy.path()));
        const QString ps = copy.path() + QStringLiteral("/sys/class/power_supply/");
        const auto write = [](const QString &file, const QByteArray &value) {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(value);
        };
        write(ps + QStringLiteral("ADP0/online"), "0\n");
        write(ps + QStringLiteral("ucsi-source-psy-USBC000:001/online"), "1\n");
        QCOMPARE(readBattery(copy.path()).acOnline, false);
        write(ps + QStringLiteral("ADP0/online"), "1\n");
        QCOMPARE(readBattery(copy.path()).acOnline, true);
    }

    void knowhereMachine()
    {
        const MachineInfo m = readMachine(kKnowhere);
        QCOMPARE(m.maker, QStringLiteral("ASUS"));
        QCOMPARE(m.model, QStringLiteral("TUF Gaming A14 FA401EA"));
        QCOMPARE(m.family, QStringLiteral("TUF Gaming A14"));
        QCOMPARE(m.biosVersion, QStringLiteral("FA401EA.304"));
        QCOMPARE(m.biosDate, QStringLiteral("01/28/2026"));
    }

    void knowhereCpu()
    {
        const CpuInfo c = readCpu(kKnowhere);
        QCOMPARE(c.name, QStringLiteral("AMD RYZEN AI MAX+ 392 w/ Radeon 8060S"));
        QCOMPARE(c.vendor, QStringLiteral("AMD"));
        QCOMPARE(c.arch, QStringLiteral("x86_64"));
        QCOMPARE(c.packages, 1);
        QCOMPARE(c.cores, 12);
        QCOMPARE(c.threads, 24);
        QCOMPARE(c.performanceCores, 0);  // not hybrid
        QCOMPARE(c.caches.l3, qint64(64) << 20);  // 2 CCDs x 32 MiB
        QCOMPARE(c.caches.l2, qint64(12) << 20);  // 12 x 1 MiB
        QCOMPARE(c.caches.l1d, qint64(12) * 48 * 1024);
        QCOMPARE(c.maxMhz, 5057);
        QCOMPARE(c.baseMhz, 0);  // amd-pstate doesn't publish one
        QVERIFY(c.features.contains(QStringLiteral("avx512f")));
    }

    void knowhereMemoryAndOs()
    {
        QVERIFY(readMemTotal(kKnowhere) > (qint64(30) << 30));
        const OsInfo os = readOsRelease(kKnowhere);
        QCOMPARE(os.name, QStringLiteral("CachyOS"));
        QCOMPARE(os.build, QStringLiteral("rolling"));
    }

    void knowhereGpuAndNpu()
    {
        const auto gpus = readGpus(kKnowhere);
        QCOMPARE(gpus.size(), 1);
        QCOMPARE(gpus[0].vendorId, 0x1002u);
        QCOMPARE(gpus[0].deviceId, 0x1586u);
        QCOMPARE(gpus[0].vendor, QStringLiteral("AMD"));
        QCOMPARE(gpus[0].driver, QStringLiteral("amdgpu"));
        QCOMPARE(gpus[0].vramBytes, qint64(512) << 20);

        const auto npus = readNpus(kKnowhere);
        QCOMPARE(npus.size(), 1);
        QCOMPARE(npus[0].driver, QStringLiteral("amdxdna"));
        QCOMPARE(npus[0].name, QStringLiteral("AMD XDNA NPU"));
        QCOMPARE(npus[0].vendorId, 0x1022u);
        QCOMPARE(npus[0].deviceId, 0x17f0u);
    }

    void knowhereBatterySkipsTheMouse()
    {
        const BatteryInfo b = readBattery(kKnowhere);
        QVERIFY(b.present);
        QVERIFY(b.acOnline);
        QCOMPARE(b.designWh, 73.0);
        QCOMPARE(b.fullWh, 72.028);
        QVERIFY(qAbs(b.healthPct - 98.67) < 0.01);
        QVERIFY(qAbs(b.chargePct - 99.0) < 0.05);  // energy_now / energy_full, not the mouse's 76 %
        QCOMPARE(b.cycleCount, 0);
    }

    void knowhereBlockDevices()
    {
        const BlockDeviceInfo root = readBlockDevice(kKnowhere, 259, 7);  // nvme1n1p2, "/"
        QCOMPARE(root.model, QStringLiteral("CT2000P3PSSD8"));
        QCOMPARE(root.bus, QStringLiteral("NVMe"));
        QCOMPARE(root.kind, QStringLiteral("SSD"));
        QCOMPARE(readBlockDevice(kKnowhere, 259, 0).model, QStringLiteral("WD_BLACK SN770 2TB"));
        const BlockDeviceInfo zram = readBlockDevice(kKnowhere, 253, 0);
        QCOMPARE(zram.bus, QStringLiteral("Virtual"));
        QVERIFY(zram.kind.isEmpty());
        QVERIFY(readBlockDevice(kKnowhere, 8, 0).model.isEmpty());  // no such device
    }

    void armBigLittle()
    {
        const CpuInfo c = readCpu(kArm);
        QCOMPARE(c.arch, QStringLiteral("arm64"));
        QCOMPARE(c.vendor, QStringLiteral("ARM"));
        QCOMPARE(c.coreTypes, (QStringList{QStringLiteral("Cortex-A55"), QStringLiteral("Cortex-A78"),
                                           QStringLiteral("Cortex-X1")}));
        QCOMPARE(c.name, QStringLiteral("ARM Cortex-A55 + Cortex-A78 + Cortex-X1"));
        QCOMPARE(c.cores, 8);
        QCOMPARE(c.threads, 8);
        QCOMPARE(c.performanceCores, 1);  // top capacity tier
        QCOMPARE(c.efficiencyCores, 7);
        QCOMPARE(c.caches.l3, qint64(4) << 20);
        QCOMPARE(c.caches.l2, qint64(4) << 20);
        QCOMPARE(c.maxMhz, 3000);
        QVERIFY(c.features.contains(QStringLiteral("dotprod")));

        QCOMPARE(readMachine(kArm).model, QStringLiteral("Example ARM64 Board Rev 1.0"));  // device tree
        QCOMPARE(readMemTotal(kArm), qint64(7864320) * 1024);
    }

    void armChargeBasedBattery()
    {
        const BatteryInfo b = readBattery(kArm);
        QVERIFY(b.present);
        QVERIFY(b.acOnline);  // USB charger
        QVERIFY(qAbs(b.fullWh - 15.4) < 1e-9);
        QVERIFY(qAbs(b.designWh - 19.25) < 1e-9);
        QVERIFY(qAbs(b.healthPct - 80.0) < 1e-9);
        QVERIFY(qAbs(b.chargePct - 50.0) < 1e-9);
        QCOMPARE(b.cycleCount, -1);
    }

    void intelHybrid()
    {
        const CpuInfo c = readCpu(kIntel);
        QCOMPARE(c.vendor, QStringLiteral("Intel"));
        QCOMPARE(c.threads, 4);
        QCOMPARE(c.cores, 3);
        QCOMPARE(c.performanceCores, 1);
        QCOMPARE(c.efficiencyCores, 2);
        QCOMPARE(c.baseMhz, 2100);
        QCOMPARE(c.maxMhz, 4700);
        QVERIFY(c.features.contains(QStringLiteral("avxvnni")));
        QVERIFY(!c.features.contains(QStringLiteral("avx512f")));
        QVERIFY(!readBattery(kIntel).present);
    }

    // Phase 3 D11: without the helper, the modules come from udev's copy
    // in run/udev/data/+dmi:id (Tiamat's capture; Knowhere's from its
    // dmidecode); a tree without it (intel-hybrid) changes nothing.
    void udevMemoryFallback()
    {
        QTemporaryDir tiamat;
        QVERIFY(sb::testing::materializeFixture(QStringLiteral(SB_FIXTURES "/linux/tiamat"), tiamat.path()));
        QVERIFY(QFileInfo::exists(tiamat.path() + QStringLiteral("/run/udev/data/+dmi:id")));
        SystemInfo info;
        QVERIFY(sb::sysinfo::applyUdevMemory(info, tiamat.path()));
        QVERIFY(info.memory.modulesKnown);
        QCOMPARE(info.memory.modulesSource, QStringLiteral("udev"));
        QCOMPARE(info.memory.modules.size(), 2);
        QCOMPARE(info.memory.modules[0].configuredSpeedMts, 6000);
        QCOMPARE(info.memory.modules[0].rank, 2);
        QCOMPARE(info.memory.channels, 2);
        SystemInfo knowhere;
        QVERIFY(sb::sysinfo::applyUdevMemory(knowhere, kKnowhere));
        QCOMPARE(knowhere.memory.modules.size(), 8);
        QCOMPARE(knowhere.memory.modules[0].configuredSpeedMts, 8000);
        QCOMPARE(knowhere.memory.modules[0].dataWidth, 32);
        QCOMPARE(knowhere.memory.channels, 8);
        SystemInfo none;
        QVERIFY(!sb::sysinfo::applyUdevMemory(none, kIntel));
        QVERIFY(!none.memory.modulesKnown);
        QVERIFY(none.memory.modulesSource.isEmpty());
    }

    // P23, the way the app reads it (applyUdevMemory on a file tree): udev's
    // copy holds serial numbers and asset tags; none reaches the snapshot.
    void udevNeverReadsSerials()
    {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("run/udev/data")));
        QFile f(root.filePath(QStringLiteral("run/udev/data/+dmi:id")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("I:123\nE:ID_VENDOR=Example\nE:ID_CHASSIS_ASSET_TAG=ASSET-SHOULD-NOT-APPEAR\n"
                "E:MEMORY_DEVICE_0_SIZE=8589934592\nE:MEMORY_DEVICE_0_TYPE=LPDDR5\n"
                "E:MEMORY_DEVICE_0_SERIAL_NUMBER=SERIAL-SHOULD-NOT-APPEAR\n"
                "E:MEMORY_DEVICE_0_ASSET_TAG=ASSET-SHOULD-NOT-APPEAR\n"
                "E:MEMORY_DEVICE_0_PART_NUMBER=K3KL3L30EM\nE:MEMORY_DEVICE_0_LOCATOR=DIMM 0\n"
                "E:MEMORY_DEVICE_0_DATA_WIDTH=32\nE:MEMORY_ARRAY_NUM_DEVICES=1\n"
                "E:MEMORY_DEVICE_4_LOCATOR=DIMMA1\n");  // no size: an empty slot, skipped
        f.close();
        SystemInfo info;
        QVERIFY(sb::sysinfo::applyUdevMemory(info, root.path()));
        QCOMPARE(info.memory.modules.size(), 1);
        QCOMPARE(info.memory.modules[0].dataWidth, 32);
        const QByteArray json = QJsonDocument(info.toJson()).toJson();
        QVERIFY(!json.contains("SERIAL"));
        QVERIFY(!json.contains("ASSET"));
        QVERIFY(json.contains("K3KL3L30EM"));
        QVERIFY(json.contains("\"modules_source\": \"udev\""));
    }
};

QTEST_GUILESS_MAIN(TestSysinfoLinux)
#include "tst_sysinfo_linux.moc"

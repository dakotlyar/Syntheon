// SPDX-License-Identifier: GPL-3.0-or-later
#include "smbios_builder.h"
#include "sysinfo/collector.h"
#include "sysinfo/parsers/smbios.h"
#include "sysinfo/parsers/text_parsers.h"

#include <QFile>
#include <QJsonDocument>
#include <QTest>

using namespace sb;
using namespace sb::sysinfo;

namespace {

QString fixture(const QString &rel)
{
    QFile f(QStringLiteral(SB_FIXTURES "/") + rel);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

class TestSysinfoParsers : public QObject
{
    Q_OBJECT

private slots:
    void cpuinfoX86Knowhere()
    {
        const CpuInfoText c = parseCpuInfo(fixture(QStringLiteral("linux/knowhere/proc/cpuinfo")));
        QVERIFY(!c.isArm());
        QCOMPARE(c.logicalCpus, 24);
        QCOMPARE(c.vendorId, QStringLiteral("AuthenticAMD"));
        QCOMPARE(c.modelName, QStringLiteral("AMD RYZEN AI MAX+ 392 w/ Radeon 8060S"));
        const QStringList f = normalizeX86Flags(c.flags);
        for (const char *expected : {"sse2", "sse3", "sse4.2", "avx2", "fma", "sha", "avx512f", "avx512vnni",
                                     "avx512bf16", "avxvnni"})
            QVERIFY2(f.contains(QLatin1StringView(expected)), expected);
        QVERIFY(!f.contains(QStringLiteral("amx")));
        QCOMPARE(f.first(), QStringLiteral("sse2"));  // canonical order
    }

    void cpuinfoArmRaspberryPi5()
    {
        const CpuInfoText c = parseCpuInfo(fixture(QStringLiteral("linux/cpuinfo-raspberry-pi-5.txt")));
        QVERIFY(c.isArm());
        QCOMPARE(c.logicalCpus, 4);
        QVERIFY(c.modelName.isEmpty());  // ARM: no "model name"
        QCOMPARE(c.armCores.size(), 4);
        QCOMPARE(c.armCores[0], (ArmCoreId{0x41, 0x4, 0xd0b, 1}));
        QCOMPARE(armCoreName(0x41, 0xd0b), QStringLiteral("Cortex-A76"));
        QCOMPARE(normalizeArmFeatures(c.flags), (QStringList{QStringLiteral("neon"), QStringLiteral("fp16"),
                                                              QStringLiteral("dotprod"), QStringLiteral("crc32"),
                                                              QStringLiteral("aes"), QStringLiteral("sha2"),
                                                              QStringLiteral("lse")}));
    }

    void midrAndNames()
    {
        const auto id = parseMidr(QStringLiteral("0x00000000410fd0b1\n"));
        QVERIFY(id);
        QCOMPARE(*id, (ArmCoreId{0x41, 0x0, 0xd0b, 1}));
        QVERIFY(!parseMidr(QStringLiteral("garbage")));
        QCOMPARE(armCoreName(0x51, 0x001), QStringLiteral("Oryon"));
        QCOMPARE(armVendorName(0x51), QStringLiteral("Qualcomm"));
        QCOMPARE(armCoreName(0xc0, 0xac3), QStringLiteral("AmpereOne"));
        QCOMPARE(armCoreName(0x41, 0xfff), QStringLiteral("ARM part 0xfff"));
    }

    void meminfoAndOsRelease()
    {
        QCOMPARE(parseMemTotal(fixture(QStringLiteral("linux/knowhere/proc/meminfo"))) > qint64(30) << 30, true);
        QCOMPARE(parseMemTotal(QStringLiteral("MemFree: 5 kB\nMemTotal:   1024 kB\n")), qint64(1024 * 1024));
        QCOMPARE(parseMemTotal(QString()), 0);

        const auto kv = parseOsRelease(fixture(QStringLiteral("linux/knowhere/etc/os-release")));
        QCOMPARE(kv.value(QStringLiteral("PRETTY_NAME")), QStringLiteral("CachyOS"));
        QCOMPARE(kv.value(QStringLiteral("BUILD_ID")), QStringLiteral("rolling"));
        const auto q = parseOsRelease(QStringLiteral("# comment\nNAME='Fedora Linux'\nVERSION=\"42 (Workstation \\\"Edition\\\")\"\nBAD LINE\n"));
        QCOMPARE(q.value(QStringLiteral("NAME")), QStringLiteral("Fedora Linux"));
        QCOMPARE(q.value(QStringLiteral("VERSION")), QStringLiteral("42 (Workstation \"Edition\")"));
    }

    void smallParsers()
    {
        QCOMPARE(parseCpuList(QStringLiteral("0-3,8,10-11\n")), (QVector<int>{0, 1, 2, 3, 8, 10, 11}));
        QCOMPARE(parseCpuList(QStringLiteral("5-2,x")), QVector<int>());
        QCOMPARE(parseCacheSize(QStringLiteral("32K")), qint64(32768));
        QCOMPARE(parseCacheSize(QStringLiteral("32768K")), qint64(32) << 20);
        QCOMPARE(parseCacheSize(QStringLiteral("2M")), qint64(2) << 20);
        QCOMPARE(parseCacheSize(QStringLiteral("junk")), qint64(0));
        QCOMPARE(cleanFirmwareString(QStringLiteral("  To Be Filled By O.E.M.  ")), QString());
        QCOMPARE(cleanFirmwareString(QStringLiteral(" ASUS ")), QStringLiteral("ASUS"));
        QCOMPARE(pciVendorName(0x10de), QStringLiteral("NVIDIA"));
    }

    void smbiosFull()
    {
        const SmbiosInfo s = parseSmbiosTable(SmbiosBuilder::sample(), {3, 6});
        QCOMPARE(s.biosVendor, QStringLiteral("American Megatrends International, LLC."));
        QCOMPARE(s.biosVersion, QStringLiteral("FA401EA.304"));
        QCOMPARE(s.biosDate, QStringLiteral("01/28/2026"));
        QCOMPARE(s.maker, QStringLiteral("ASUSTeK COMPUTER INC."));
        QCOMPARE(s.product, QStringLiteral("TUF Gaming A14 FA401EA"));
        QCOMPARE(s.family, QStringLiteral("TUF Gaming A14"));
        QCOMPARE(s.memoryDevices.size(), 2);  // the empty slot is skipped
        const SmbiosMemoryDevice &d = s.memoryDevices[0];
        QCOMPARE(d.sizeBytes, qint64(32) << 30);
        QCOMPARE(d.type, QStringLiteral("LPDDR5"));
        QCOMPARE(d.speedMts, 8000);
        QCOMPARE(d.configuredSpeedMts, 8000);
        QCOMPARE(d.manufacturer, QStringLiteral("Samsung"));
        QCOMPARE(d.partNumber, QStringLiteral("K3KL3L30EM-BGCV"));
        QCOMPARE(d.bankLocator, QStringLiteral("P0 CHANNEL A"));
        QCOMPARE(estimateMemoryChannels(s.memoryDevices), 2);
    }

    void smbiosSizesAndSpeeds()
    {
        SmbiosBuilder b;
        b.add(17, 0x22).word(0x0C, 16384).byte(0x12, 0x22).word(0x15, 5600).word(0x20, 4800);  // 16 GiB in MB
        b.add(17, 0x22).word(0x0C, 0x8000 | 512).byte(0x12, 0x1A);                               // 512 KB
        b.add(17, 0x5C).word(0x0C, 8192).word(0x15, 0xFFFF).dword(0x54, 10000).word(0x20, 0xFFFF).dword(0x58, 9600);
        b.add(17, 0x22).word(0x0C, 0xFFFF);  // unknown size: skipped
        const SmbiosInfo s = parseSmbiosTable(b.table(), {3, 3});
        QCOMPARE(s.memoryDevices.size(), 3);
        QCOMPARE(s.memoryDevices[0].sizeBytes, qint64(16) << 30);
        QCOMPARE(s.memoryDevices[0].type, QStringLiteral("DDR5"));
        QCOMPARE(s.memoryDevices[0].configuredSpeedMts, 4800);
        QCOMPARE(s.memoryDevices[1].sizeBytes, qint64(512) * 1024);
        QCOMPARE(s.memoryDevices[2].speedMts, 10000);         // extended speed (3.3+)
        QCOMPARE(s.memoryDevices[2].configuredSpeedMts, 9600);
        QCOMPARE(s.memoryDevices[0].locator, QString());       // no strings
        // Locators that don't say: fall back to the module count, capped at 2.
        QCOMPARE(estimateMemoryChannels(s.memoryDevices), 2);
        QCOMPARE(estimateMemoryChannels({}), 0);
    }

    void smbiosShortStructuresAndOldVersions()
    {
        SmbiosBuilder b;
        // An SMBIOS 2.3-era type 17 (0x1B bytes): no configured speed, no extended fields.
        b.add(17, 0x1B).word(0x0C, 4096).byte(0x12, 0x18).word(0x15, 1600).str(0x17, "Hynix").str(0x1A, "HMT");
        // Type 1 too short for a family field.
        b.add(1, 0x08).str(0x04, "Old Corp").str(0x05, "Box");
        const SmbiosInfo s = parseSmbiosTable(b.table(), {2, 3});
        QCOMPARE(s.memoryDevices.size(), 1);
        QCOMPARE(s.memoryDevices[0].type, QStringLiteral("DDR3"));
        QCOMPARE(s.memoryDevices[0].speedMts, 1600);
        QCOMPARE(s.memoryDevices[0].configuredSpeedMts, 0);
        QCOMPARE(s.memoryDevices[0].manufacturer, QStringLiteral("Hynix"));
        QCOMPARE(s.maker, QStringLiteral("Old Corp"));
        QVERIFY(s.family.isEmpty());
    }

    void smbiosMalformedNeverCrashes()
    {
        const QByteArray good = SmbiosBuilder::sample();
        // Every truncation, plus corrupted lengths and string indexes.
        for (int n = 0; n <= good.size(); ++n)
            parseSmbiosTable(good.left(n), {3, 6});
        for (int i = 0; i < good.size(); ++i) {
            QByteArray bad = good;
            bad[i] = char(0xFF);
            parseSmbiosTable(bad, {3, 6});
            bad[i] = char(0x01);
            parseSmbiosTable(bad, {3, 6});
        }
        QByteArray noTerminator = good;
        noTerminator.chop(2);
        parseSmbiosTable(noTerminator, {3, 6});
        QCOMPARE(parseSmbiosTable(QByteArray("\x11\x02\x00", 3), {3, 0}).memoryDevices.size(), 0);
    }

    void smbiosNeverReadsSerials()
    {
        const SmbiosInfo s = parseSmbiosTable(SmbiosBuilder::sample(), {3, 6});
        SystemInfo info;
        applySmbios(info, s);
        const QByteArray json = QJsonDocument(info.toJson()).toJson();
        QVERIFY(!json.contains("SERIAL"));
        QVERIFY(json.contains("K3KL3L30EM-BGCV"));
    }

    void smbiosEntryPointAndWindowsHeader()
    {
        QByteArray sm3("_SM3_\x00\x18\x03\x07", 9);
        QCOMPARE(parseSmbiosEntryPoint(sm3)->major, 3);
        QCOMPARE(parseSmbiosEntryPoint(sm3)->minor, 7);
        QByteArray sm2("_SM_\x00\x1f\x02\x08", 8);
        QCOMPARE(parseSmbiosEntryPoint(sm2)->major, 2);
        QCOMPARE(parseSmbiosEntryPoint(sm2)->minor, 8);
        QVERIFY(!parseSmbiosEntryPoint("nope"));

        const QByteArray table = SmbiosBuilder::sample();
        QByteArray raw(8, '\0');
        raw[1] = 3;
        raw[2] = 6;
        qToLittleEndian<quint32>(quint32(table.size()), raw.data() + 4);
        raw += table;
        const auto s = parseWindowsRawSmbios(raw);
        QVERIFY(s);
        QCOMPARE(s->version.major, 3);
        QCOMPARE(s->memoryDevices.size(), 2);
        qToLittleEndian<quint32>(quint32(table.size() + 1), raw.data() + 4);  // claims too much
        QVERIFY(!parseWindowsRawSmbios(raw));
        QVERIFY(!parseWindowsRawSmbios(QByteArray(3, '\0')));
    }

    void applySmbiosFillsGapsOnly()
    {
        SystemInfo info;
        info.machine.maker = QStringLiteral("From DMI");
        applySmbios(info, parseSmbiosTable(SmbiosBuilder::sample(), {3, 6}));
        QCOMPARE(info.machine.maker, QStringLiteral("From DMI"));
        QCOMPARE(info.machine.model, QStringLiteral("TUF Gaming A14 FA401EA"));
        QVERIFY(info.memory.modulesKnown);
        QCOMPARE(info.memory.modules.size(), 2);
        QCOMPARE(info.memory.channels, 2);
    }

    void snapshotShape()
    {
        SystemInfo info;
        info.machine.hostname = QStringLiteral("Knowhere");
        info.cpu.threads = 24;
        const SystemSnapshot s = info.toSnapshot();
        QCOMPARE(s.machineLabel, QStringLiteral("Knowhere"));
        for (const char *key : {"machine", "cpu", "memory", "gpus", "apis", "storage", "os", "npus", "battery", "app",
                                "emulated"})
            QVERIFY2(s.data.contains(QLatin1StringView(key)), key);
        QCOMPARE(s.toJson().value(QStringLiteral("sysinfo_version")).toInt(), 1);
        info.machine.hostname.clear();
        info.machine.maker = QStringLiteral("ASUS");
        info.machine.model = QStringLiteral("TUF Gaming A14 FA401EA");
        QCOMPARE(info.machineLabel(), QStringLiteral("ASUS TUF Gaming A14 FA401EA"));
    }
};

QTEST_GUILESS_MAIN(TestSysinfoParsers)
#include "tst_sysinfo_parsers.moc"

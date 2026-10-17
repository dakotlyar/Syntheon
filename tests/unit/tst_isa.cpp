// SPDX-License-Identifier: GPL-3.0-or-later
// synbench_isa (Phase 2 design §14.1 "ISA"): the decoders on injected CPUID /
// XCR0 / hwcap bits, the SIMD ladder (P9), the core ranking (P4) on fixture
// sysfs trees and mocked CPU-set data, and System info's view of it (P15).
#include "isa/core_ranking.h"
#include "isa/cpu_features.h"
#include "sysinfo/common/x86_cpu.h"

#include <QTest>

using namespace sb::isa;

namespace {

// Knowhere (AMD Ryzen AI Max+ 392, Zen 5), recorded 2026-10-16 with CPUID and
// XGETBV on CPU 1. The features the Phase 1 decoder reported for it are in
// kKnowhereFeatures; the new decoder must give System info exactly those.
X86Cpuid knowhere()
{
    X86Cpuid r;
    r.leaf0 = {0x00000010, 0x68747541, 0x444d4163, 0x69746e65};
    r.leaf1 = {0x00b70f00, 0x01180800, 0x7ed8320b, 0x178bfbff};
    r.leaf7 = {0x00000001, 0xf1bf97ab, 0x19405fde, 0x10000110};
    r.leaf7_1 = {0x00000030, 0x00000000, 0x00000000, 0x00000000};
    r.maxExtendedLeaf = 0x80000028;
    r.brand[0] = {0x20444d41, 0x455a5952, 0x4941204e, 0x58414d20};
    r.brand[1] = {0x3933202b, 0x2f772032, 0x64615220, 0x206e6f65};
    r.brand[2] = {0x30363038, 0x20202053, 0x20202020, 0x00202020};
    r.xcr0 = 0x2e7;
    return r;
}
const char *const kKnowhereFeatures = "sse2,sse3,ssse3,sse4.1,sse4.2,popcnt,aes,f16c,fma,bmi1,bmi2,avx,avx2,sha,"
                                      "avx512f,avx512dq,avx512cd,avx512bw,avx512vl,avx512vnni,avx512bf16,avxvnni";

// The fixture's sysfs root (<machine>/sys).
QString fixture(const char *machine)
{
    return QStringLiteral(SB_FIXTURES "/linux/") + QLatin1StringView(machine) + QStringLiteral("/sys");
}

} // namespace

class TestIsa : public QObject
{
    Q_OBJECT

private slots:
    void knowhereDecodesAsBefore()
    {
        const CpuFeatures f = decodeX86(knowhere());
        QCOMPARE(QString::fromStdString(f.vendor), QStringLiteral("AuthenticAMD"));
        QCOMPARE(QString::fromStdString(f.brand), QStringLiteral("AMD RYZEN AI MAX+ 392 w/ Radeon 8060S"));
        // System info is unchanged by the move onto synbench_isa (P15).
        const sb::sysinfo::X86Cpu s = sb::sysinfo::fromIsa(f);
        QCOMPARE(s.features.join(QLatin1Char(',')), QLatin1StringView(kKnowhereFeatures));
        QCOMPARE(s.vendor, QStringLiteral("AuthenticAMD"));
        // What the CPU test also needs, beyond System info's list.
        QVERIFY(f.has(Feature::Pclmulqdq));
        QVERIFY(f.has(Feature::Vaes));
        QVERIFY(f.has(Feature::Vpclmulqdq));
        QVERIFY(!f.avx512WithoutOsState);
        QCOMPARE(bestSimdPath(f), SimdPath::Avx512);
        QCOMPARE(QString::fromStdString(describeSimdChoice(f)),
                 QStringLiteral("AVX-512 (512-bit) path selected (F, BW, DQ, VL; OS ZMM state on)"));
    }

    // The CPU has AVX-512 but the OS saves only XMM/YMM (XCR0 bits 5-7 off).
    void avx512WithoutOsZmmStateFallsToAvx2()
    {
        X86Cpuid r = knowhere();
        r.xcr0 = 0x7;
        const CpuFeatures f = decodeX86(r);
        QVERIFY(!f.has(Feature::Avx512F));
        QVERIFY(!f.has(Feature::Avx512Bf16));
        QVERIFY(f.has(Feature::Avx2));
        QVERIFY(f.avx512WithoutOsState);
        QCOMPARE(bestSimdPath(f), SimdPath::Avx2Fma);
        QVERIFY(QString::fromStdString(describeSimdChoice(f)).contains(QLatin1StringView("doesn't save its ZMM state")));
    }

    // A hybrid Intel with AVX-512 fused off: the CPUID bits are simply absent.
    void avx512FusedOffIsAvx2()
    {
        X86Cpuid r = knowhere();
        r.leaf7.b &= ~((1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31));
        r.leaf7.c &= ~(1u << 11);
        const CpuFeatures f = decodeX86(r);
        QVERIFY(!f.has(Feature::Avx512F));
        QVERIFY(!f.avx512WithoutOsState);
        QCOMPARE(bestSimdPath(f), SimdPath::Avx2Fma);
        QCOMPARE(QString::fromStdString(describeSimdChoice(f)),
                 QStringLiteral("AVX2 + FMA (256-bit) path selected (no AVX-512 F/BW/DQ/VL)"));
    }

    // One AVX-512 piece missing (no BW, as on Knights Landing) isn't the AVX-512 path.
    void avx512NeedsAllFour()
    {
        X86Cpuid r = knowhere();
        r.leaf7.b &= ~(1u << 30);
        QCOMPARE(bestSimdPath(decodeX86(r)), SimdPath::Avx2Fma);
    }

    // No OSXSAVE: no AVX of any kind, whatever the CPU has.
    void noOsxsaveIsSse42()
    {
        X86Cpuid r = knowhere();
        r.leaf1.c &= ~(1u << 27);
        const CpuFeatures f = decodeX86(r);
        QVERIFY(!f.has(Feature::Avx));
        QVERIFY(!f.has(Feature::Avx2));
        QVERIFY(!f.has(Feature::Fma));
        QVERIFY(!f.has(Feature::Vaes));
        QVERIFY(f.has(Feature::Sse42));
        QVERIFY(f.has(Feature::Aes));  // AES-NI needs no OS state
        QCOMPARE(bestSimdPath(f), SimdPath::Sse42);
    }

    void avx2WithoutFmaIsSse42()
    {
        X86Cpuid r = knowhere();
        r.xcr0 = 0x7;
        r.leaf1.c &= ~(1u << 12);
        QCOMPARE(bestSimdPath(decodeX86(r)), SimdPath::Sse42);
    }

    void linuxArmHwcaps()
    {
        // ASIMD, AES, PMULL, SHA1, SHA2, CRC32, ATOMICS, ASIMDDP, SVE; HWCAP2 SVE2, I8MM.
        const std::uint64_t hwcap = (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (1ull << 7)
                                    | (1ull << 8) | (1ull << 20) | (1ull << 22);
        const std::uint64_t hwcap2 = (1ull << 1) | (1ull << 13);
        const CpuFeatures f = decodeLinuxArmHwcaps(hwcap, hwcap2);
        QVERIFY(f.has(Feature::Neon));
        QVERIFY(f.has(Feature::ArmAes));
        QVERIFY(f.has(Feature::ArmSha2));
        QVERIFY(f.has(Feature::Sve));
        QVERIFY(f.has(Feature::Sve2));
        QVERIFY(f.has(Feature::I8mm));
        QVERIFY(!f.has(Feature::Bf16));
        QCOMPARE(bestSimdPath(f, true), SimdPath::Sve);   // a Linux ARM64 build
        QCOMPARE(bestSimdPath(f, false), SimdPath::Neon);  // a build without an SVE path
        QVERIFY(QString::fromStdString(describeSimdChoice(f, false)).contains(QLatin1StringView("no SVE path")));
        const CpuFeatures plain = decodeLinuxArmHwcaps(1ull << 1, 0);
        QCOMPARE(bestSimdPath(plain, true), SimdPath::Neon);
        QVERIFY(!plain.has(Feature::ArmAes));
    }

    void windowsArm()
    {
        WindowsArmFeatures p;
        p.crypto = true;
        p.dotProd = true;
        const CpuFeatures f = decodeWindowsArm(p);
        QVERIFY(f.has(Feature::Neon));
        QVERIFY(f.has(Feature::ArmAes));
        QVERIFY(f.has(Feature::ArmPmull));
        QVERIFY(f.has(Feature::ArmSha1));
        QVERIFY(f.has(Feature::ArmSha2));
        QVERIFY(f.has(Feature::DotProd));
        QCOMPARE(bestSimdPath(f, false), SimdPath::Neon);
        QVERIFY(!decodeWindowsArm({}).has(Feature::ArmAes));
        WindowsArmFeatures q;
        q.i8mm = q.bf16 = q.fp16 = true;
        const CpuFeatures g = decodeWindowsArm(q);
        QVERIFY(g.has(Feature::I8mm) && g.has(Feature::Bf16) && g.has(Feature::Fp16));
        QVERIFY(!g.has(Feature::Sve));
    }

    // Knowhere's CPPC ranking (captured 2026-10-16): 216 is the top value,
    // shared by CPUs 0, 5, 12 and 17; the lowest number wins.
    void rankKnowhereCppc()
    {
        const CoreChoice c = rankLinuxCores(fixture("knowhere").toStdString(), {});
        QVERIFY(c.ranked);
        QCOMPARE(c.cpu, 0);
        QCOMPARE(QString::fromStdString(c.source), QStringLiteral("acpi_cppc/highest_perf"));
        // Only the CPUs this process may run on (taskset 1-4,6-11,13-16,18-23): the 211s next.
        std::vector<int> allowed;
        for (int i = 1; i < 24; ++i) {
            if (i != 5 && i != 12 && i != 17)
                allowed.push_back(i);
        }
        QCOMPARE(rankLinuxCores(fixture("knowhere").toStdString(), allowed).cpu, 3);
    }

    void rankArmBigLittleByCapacity()
    {
        const CoreChoice c = rankLinuxCores(fixture("arm64-biglittle").toStdString(), {});
        QVERIFY(c.ranked);
        QCOMPARE(c.cpu, 7);  // the one 1024-capacity core
        QCOMPARE(QString::fromStdString(c.source), QStringLiteral("cpu_capacity"));
    }

    void rankIntelHybridByMaxFrequency()
    {
        const CoreChoice c = rankLinuxCores(fixture("intel-hybrid").toStdString(), {});
        QVERIFY(c.ranked);
        QCOMPARE(c.cpu, 0);  // P-cores 0-1 at 4.7 GHz, E-cores at 3.4
        QCOMPARE(QString::fromStdString(c.source), QStringLiteral("cpuinfo_max_freq"));
        QCOMPARE(rankLinuxCores(fixture("intel-hybrid").toStdString(), {2, 3}).cpu, 2);
    }

    void noRankingData()
    {
        QVERIFY(!rankLinuxCores("/nonexistent-sysfs", {}).ranked);
        QVERIFY(!rankWindowsCpuSets({}).ranked);
    }

    // Mocked GetSystemCpuSetInformation data: two P-cores (EfficiencyClass 1),
    // two E-cores (0); the preferred core has the higher SchedulingClass.
    void rankWindowsCpuSets_()
    {
        std::vector<CpuSetRecord> sets = {
            {256, 0, 0, 0, 0, false}, {257, 0, 1, 0, 0, false},
            {258, 0, 2, 1, 5, false}, {259, 0, 3, 1, 7, false},
            {260, 1, 0, 1, 7, false},
        };
        CoreChoice c = rankWindowsCpuSets(sets);
        QVERIFY(c.ranked);
        QCOMPARE(c.group, 0);
        QCOMPARE(c.cpu, 3);
        QCOMPARE(c.cpuSetId, 259u);
        // Reserved for another process: skipped.
        sets[3].allocatedElsewhere = true;
        c = rankWindowsCpuSets(sets);
        QCOMPARE(c.group, 1);
        QCOMPARE(c.cpu, 0);
        // All equal: the lowest group and index.
        for (CpuSetRecord &s : sets) {
            s.efficiencyClass = 0;
            s.schedulingClass = 0;
            s.allocatedElsewhere = false;
        }
        c = rankWindowsCpuSets(sets);
        QCOMPARE(c.group, 0);
        QCOMPARE(c.cpu, 0);
        // Outside the process affinity, or real-time: skipped; parked: last resort.
        sets[0].inProcessAffinity = false;
        sets[1].realTime = true;
        sets[2].parked = true;
        c = rankWindowsCpuSets(sets);
        QCOMPARE(c.group, 0);
        QCOMPARE(c.cpu, 3);
        sets[3].inProcessAffinity = false;
        sets[4].inProcessAffinity = false;
        c = rankWindowsCpuSets(sets);
        QCOMPARE(c.cpu, 2);  // only the parked one is left
    }

    // The live machine: detection runs, and System info shows what the CPU
    // test dispatches on.
    void live()
    {
        const CpuFeatures f = detect();
#if defined(Q_PROCESSOR_X86_64)
        QVERIFY(f.has(Feature::Sse42));  // the x86-64-v2 baseline
        QCOMPARE(bestSimdPath(f) == SimdPath::None, false);
        QCOMPARE(sb::sysinfo::queryX86Cpu().features, sb::sysinfo::fromIsa(f).features);
#elif defined(Q_PROCESSOR_ARM_64)
        QVERIFY(f.has(Feature::Neon));
#endif
        const CoreChoice c = bestCoreForSingleThread();
        qInfo("single-thread core: %s (%s)", c.ranked ? qPrintable(QString::number(c.cpu)) : "unranked", c.source.c_str());
        if (c.ranked)
            QVERIFY(c.cpu >= 0);
    }
};

QTEST_GUILESS_MAIN(TestIsa)
#include "tst_isa.moc"

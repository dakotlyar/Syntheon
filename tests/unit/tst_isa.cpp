// SPDX-License-Identifier: GPL-3.0-or-later
// synbench_isa (Phase 2 design §14.1 "ISA"): the decoders on injected CPUID /
// XCR0 / hwcap bits, the SIMD ladder (P9), the core ranking (P4) on fixture
// sysfs trees and mocked CPU-set data, and System info's view of it (P15).
#include "isa/core_ranking.h"
#include "isa/cpu_features.h"
#include "sysinfo/common/x86_cpu.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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

std::vector<int> cpusOf(const std::vector<CoreCandidate> &candidates)
{
    std::vector<int> cpus;
    for (const CoreCandidate &c : candidates)
        cpus.push_back(c.core.cpu);
    return cpus;
}

// A /proc/stat with the given idle and iowait jiffies per CPU (user, nice,
// system and the rest are filler; the aggregate line comes first, as in the
// real file).
std::string procStat(const std::vector<std::pair<unsigned, unsigned>> &idleIowait)
{
    std::string text = "cpu  999999 0 999999 999999 999 0 0 0 0 0\n";
    for (std::size_t i = 0; i < idleIowait.size(); ++i)
        text += "cpu" + std::to_string(i) + " 1000 20 300 " + std::to_string(idleIowait[i].first) + " "
                + std::to_string(idleIowait[i].second) + " 0 5 0 0 0\n";
    return text + "intr 12345 0 0\nctxt 999\n";
}

} // namespace

class TestIsa : public QObject
{
    Q_OBJECT

private slots:
    // A core's physical-core keys, read as the tie set reads them (the RAM
    // test's no-ranking fallback, AP-5 nit): itself always, its siblings
    // where the OS lists them; nothing for an unranked core.
    void smtSiblingKeysOfThisMachine()
    {
        QVERIFY(sb::isa::smtSiblingKeys(sb::isa::CoreChoice{}).empty());
        const std::vector<sb::isa::CoreChoice> cpus = sb::isa::allowedLogicalCpus();
        if (cpus.empty())
            QSKIP("no CPU list on this platform");
        const std::vector<int> keys = sb::isa::smtSiblingKeys(cpus.front());
        QVERIFY(std::find(keys.begin(), keys.end(), sb::isa::cpuKey(cpus.front())) != keys.end());
        QVERIFY(keys.size() <= 8);
    }

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
                 QStringLiteral("AVX-512 (512-bit) path selected (F, BW, DQ, VL; OS saves ZMM state)"));
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

    // AP-5 L2: the AVX-512 path's TU may use FMA3 and AVX2 instructions too,
    // so a CPU (or hypervisor) that reports AVX-512 without them doesn't get it.
    void avx512WithoutFmaOrAvx2IsNotAvx512()
    {
        X86Cpuid noFma = knowhere();
        noFma.leaf1.c &= ~(1u << 12);
        const CpuFeatures f = decodeX86(noFma);
        QVERIFY(f.has(Feature::Avx512F) && f.has(Feature::Avx512Vl) && !f.has(Feature::Fma));
        QCOMPARE(bestSimdPath(f), SimdPath::Sse42);
        X86Cpuid noAvx2 = knowhere();
        noAvx2.leaf7.b &= ~(1u << 5);
        const CpuFeatures g = decodeX86(noAvx2);
        QVERIFY(g.has(Feature::Avx512F) && !g.has(Feature::Avx2));
        QCOMPARE(bestSimdPath(g), SimdPath::Sse42);
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

    // Decision D: the tie set is every CPU at the top value, with its SMT
    // siblings (Knowhere: 0, 5, 12, 17 at 216; pairs 0/12 and 5/17).
    void tieSetKnowhere()
    {
        const std::vector<CoreCandidate> top = linuxTopCandidates(fixture("knowhere").toStdString(), {});
        QCOMPARE(cpusOf(top), (std::vector<int>{0, 5, 12, 17}));
        QCOMPARE(top[0].smtKeys, (std::vector<int>{0, 12}));
        QCOMPARE(top[1].smtKeys, (std::vector<int>{5, 17}));
        QCOMPARE(top[2].smtKeys, (std::vector<int>{0, 12}));
        QCOMPARE(top[3].smtKeys, (std::vector<int>{5, 17}));
        for (const CoreCandidate &c : top) {
            QVERIFY(c.core.ranked);
            QCOMPARE(QString::fromStdString(c.core.source), QStringLiteral("acpi_cppc/highest_perf"));
        }
        // Only allowed CPUs: without 0, 5, 12, 17 the 211s (3 and 15) tie.
        std::vector<int> allowed;
        for (int i = 1; i < 24; ++i) {
            if (i != 5 && i != 12 && i != 17)
                allowed.push_back(i);
        }
        QCOMPARE(cpusOf(linuxTopCandidates(fixture("knowhere").toStdString(), allowed)), (std::vector<int>{3, 15}));
        // A hybrid CPU's tie set is its fastest cores only (P-core threads 0-1).
        QCOMPARE(cpusOf(linuxTopCandidates(fixture("intel-hybrid").toStdString(), {})), (std::vector<int>{0, 1}));
        QVERIFY(linuxTopCandidates("/nonexistent-sysfs", {}).empty());
    }

    void procStatIdle()
    {
        const IdleSample s = parseProcStatIdle(procStat({{100, 5}, {200, 0}}) + "cpu7 1 2 3 40\n");
        QCOMPARE(s.size(), std::size_t(3));  // the aggregate "cpu " line is skipped
        QCOMPARE(s.at(0), std::uint64_t(105));  // idle + iowait
        QCOMPARE(s.at(1), std::uint64_t(200));
        QCOMPARE(s.at(7), std::uint64_t(40));  // an old kernel without iowait
    }

    // The busy-sibling tie-break on Knowhere's tie set with injected samples:
    // a program on CPU 12 shares CPU 0's core, so CPU 5 wins, and the other
    // way round; idle cores tie and the lowest number wins.
    void busySiblingTieBreak()
    {
        const std::vector<CoreCandidate> top = linuxTopCandidates(fixture("knowhere").toStdString(), {});
        const auto pick = [&](int busyCpu, unsigned busyIdle) {
            std::vector<std::pair<unsigned, unsigned>> before(24, {1000, 10}), after(24, {1025, 10});
            after[std::size_t(busyCpu)].first = 1000 + busyIdle;  // 25 jiffies = 250 ms all idle
            return quietestCandidate(top, parseProcStatIdle(procStat(before)), parseProcStatIdle(procStat(after))).cpu;
        };
        QCOMPARE(pick(12, 5), 5);   // CPU 12 80% busy: core 0/12 is the busier one
        QCOMPARE(pick(0, 5), 5);    // the top CPU itself busy
        QCOMPARE(pick(17, 5), 0);   // core 5/17 busy: back to CPU 0
        QCOMPARE(pick(5, 24), 0);
        QCOMPARE(pick(3, 0), 0);    // a busy CPU outside the tie set changes nothing
        // iowait counts as idle.
        std::vector<std::pair<unsigned, unsigned>> before(24, {1000, 10}), after(24, {1025, 10});
        after[12] = {1000, 35};
        after[17] = {1010, 10};
        QCOMPARE(quietestCandidate(top, parseProcStatIdle(procStat(before)), parseProcStatIdle(procStat(after))).cpu, 0);
        // A key missing from a sample counts as never idle.
        IdleSample b = parseProcStatIdle(procStat(before)), a = parseProcStatIdle(procStat(before));
        for (auto &[cpu, idle] : a)
            idle += 25;
        a.erase(12);
        QCOMPARE(quietestCandidate(top, b, a).cpu, 5);
    }

    // No SMT (no thread_siblings_list): each core is just itself.
    void noSmt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int cpu = 0; cpu < 4; ++cpu) {
            const QString d = dir.path() + QStringLiteral("/devices/system/cpu/cpu%1/cpufreq").arg(cpu);
            QVERIFY(QDir().mkpath(d));
            QFile f(d + QStringLiteral("/cpuinfo_max_freq"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(cpu == 3 ? "3000000\n" : "3600000\n");
        }
        const std::vector<CoreCandidate> top = linuxTopCandidates(dir.path().toStdString(), {});
        QCOMPARE(cpusOf(top), (std::vector<int>{0, 1, 2}));
        for (const CoreCandidate &c : top)
            QCOMPARE(c.smtKeys, std::vector<int>{c.core.cpu});
        const IdleSample before{{0, 100}, {1, 100}, {2, 100}, {3, 100}};
        QCOMPARE(quietestCandidate(top, before, {{0, 110}, {1, 125}, {2, 120}, {3, 125}}).cpu, 1);
        QCOMPARE(quietestCandidate(top, before, {{0, 110}, {1, 110}, {2, 125}, {3, 125}}).cpu, 2);
        // CPU 3 is the quietest but slower: never picked.
        QCOMPARE(quietestCandidate(top, before, {{0, 101}, {1, 101}, {2, 101}, {3, 125}}).cpu, 0);
    }

    // A mixed tie set (a 2-thread core and a 1-thread core at the same
    // ranking): idle is compared per logical CPU, not summed, so two
    // half-busy threads don't beat one idle one.
    void mixedSmtTieSet()
    {
        const std::vector<CoreCandidate> top = {{{true, 0, 0, 0, "test"}, {0, 1}}, {{true, 2, 0, 0, "test"}, {2}}};
        const IdleSample before{{0, 0}, {1, 0}, {2, 0}};
        // Sums 30 against 25; means 15 against 25.
        QCOMPARE(quietestCandidate(top, before, {{0, 15}, {1, 15}, {2, 25}}).cpu, 2);
        // Both threads idle: means 25 against 25, a tie, so the earlier one.
        QCOMPARE(quietestCandidate(top, before, {{0, 25}, {1, 25}, {2, 25}}).cpu, 0);
        QCOMPARE(quietestCandidate(top, before, {{0, 25}, {1, 24}, {2, 25}}).cpu, 2);
    }

    // One core at the top (the ARM fixture's single 1024-capacity core): it
    // is the choice however busy it was.
    void singleCoreTied()
    {
        const std::vector<CoreCandidate> top = linuxTopCandidates(fixture("arm64-biglittle").toStdString(), {});
        QCOMPARE(cpusOf(top), std::vector<int>{7});
        QCOMPARE(top[0].smtKeys, std::vector<int>{7});
        IdleSample before, after;
        for (int cpu = 0; cpu < 8; ++cpu) {
            before[cpu] = 0;
            after[cpu] = cpu == 7 ? 0 : 25;
        }
        const CoreChoice c = quietestCandidate(top, before, after);
        QVERIFY(c.ranked);
        QCOMPARE(c.cpu, 7);
        QVERIFY(!quietestCandidate({}, before, after).ranked);
    }

    // Mocked Windows data: a hybrid CPU with two SMT P-cores (EfficiencyClass
    // 1; group 0 processors 0/1 and 2/3) and two E-cores (0; processors 4, 5);
    // idle cycles from QueryIdleProcessorCycleTimeEx keyed by windowsKey().
    void windowsCpuSetTieBreak()
    {
        std::vector<CpuSetRecord> sets = {
            {256, 0, 0, 1, 7}, {257, 0, 1, 1, 7}, {258, 0, 2, 1, 7}, {259, 0, 3, 1, 7},
            {260, 0, 4, 0, 0}, {261, 0, 5, 0, 0},
        };
        const std::vector<std::vector<int>> cores = {{0, 1}, {2, 3}, {4}, {5}};
        std::vector<CoreCandidate> top = windowsTopCandidates(sets, cores);
        QCOMPARE(cpusOf(top), (std::vector<int>{0, 1, 2, 3}));  // the P-cores only
        QCOMPARE(top[0].smtKeys, (std::vector<int>{0, 1}));
        QCOMPARE(top[3].smtKeys, (std::vector<int>{2, 3}));
        QCOMPARE(top[2].core.cpuSetId, 258u);
        QCOMPARE(QString::fromStdString(top[0].core.source), QStringLiteral("EfficiencyClass/SchedulingClass"));
        QCOMPARE(top.front().core.cpuSetId, rankWindowsCpuSets(sets).cpuSetId);

        const IdleSample before{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}};
        const std::uint64_t idle = 1'000'000'000;  // 250 ms at 4 GHz
        // Processor 1 busy: P-core 2/3 wins; the idle E-cores never do.
        QCOMPARE(quietestCandidate(top, before, {{0, idle}, {1, idle / 10}, {2, idle}, {3, idle}, {4, idle}, {5, idle}}).cpu, 2);
        QCOMPARE(quietestCandidate(top, before, {{0, idle}, {1, idle}, {2, idle}, {3, 0}, {4, idle}, {5, idle}}).cpu, 0);

        // The preferred core (higher SchedulingClass) is a tie set of its own
        // two threads.
        sets[2].schedulingClass = sets[3].schedulingClass = 8;
        top = windowsTopCandidates(sets, cores);
        QCOMPARE(cpusOf(top), (std::vector<int>{2, 3}));
        // A second processor group: keys don't collide, and an unknown core
        // relation leaves a processor on its own.
        sets.push_back({262, 1, 2, 1, 8});
        top = windowsTopCandidates(sets, cores);
        QCOMPARE(top.size(), std::size_t(3));
        QCOMPARE(top[2].core.group, 1);
        QCOMPARE(top[2].smtKeys, std::vector<int>{windowsKey(1, 2)});
        IdleSample b2 = before, a2{{0, idle}, {1, idle}, {2, idle / 2}, {3, 0}, {4, idle}, {5, idle}};
        b2[windowsKey(1, 2)] = 0;
        a2[windowsKey(1, 2)] = idle;
        const CoreChoice c = quietestCandidate(top, b2, a2);
        QCOMPARE(c.group, 1);
        QCOMPARE(c.cpu, 2);
        QCOMPARE(c.cpuSetId, 262u);
        QVERIFY(windowsTopCandidates({}, cores).empty());
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
        // The tie set starts with that choice, and every candidate's keys are
        // in a live idle sample.
        const std::vector<CoreCandidate> top = topCandidatesForSingleThread();
        QCOMPARE(top.empty(), !c.ranked);
        if (!top.empty()) {
            QCOMPARE(top.front().core.cpu, c.cpu);
            QCOMPARE(top.front().core.group, c.group);
            const IdleSample idle = sampleIdle();
            for (const CoreCandidate &k : top) {
                for (const int key : k.smtKeys)
                    QVERIFY2(idle.count(key), qPrintable(QStringLiteral("no idle sample for %1").arg(key)));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestIsa)
#include "tst_isa.moc"

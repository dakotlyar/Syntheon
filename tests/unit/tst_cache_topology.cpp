// SPDX-License-Identifier: GPL-3.0-or-later
// The per-CPU cache topology (Phase 3 design P6, §14.1 "Cache topology"):
// the Linux sysfs reader on fixture trees (Tiamat's asymmetric L3, Knowhere,
// the synthetic Intel hybrid and ARM big.LITTLE trees, a CPU without cache
// information), the Windows GetLogicalProcessorInformationEx(RelationCache)
// parser on synthetic records (one GroupMask, and Windows 11's GroupMasks
// over two groups), and the same-LLC filter of the single-thread tie set.
#include "isa/cache_topology.h"
#include "isa/core_ranking.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include <cstdint>
#include <vector>

using namespace sb::isa;

namespace {

std::string fixture(const char *machine)
{
    return (QStringLiteral(SB_FIXTURES "/linux/") + QLatin1StringView(machine) + QStringLiteral("/sys")).toStdString();
}

std::vector<int> range(int first, int last)
{
    std::vector<int> v;
    for (int i = first; i <= last; ++i)
        v.push_back(i);
    return v;
}

std::vector<int> join(std::vector<int> a, const std::vector<int> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

constexpr std::uint64_t KiB = 1024, MiB = 1024 * 1024;

// One SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record, little-endian, as
// Windows lays it out on x64/ARM64. `relationship` 2 = RelationCache; `type`
// 0 unified, 1 instruction, 2 data. groupCount 0 = before Windows 11 (one
// GroupMask); masks are (group, mask) pairs.
struct Mask {
    int group;
    std::uint64_t bits;
};
void appendRecord(std::vector<unsigned char> &out, std::uint32_t relationship, int level, std::uint32_t type,
                  int lineSize, std::uint32_t size, std::uint16_t groupCount, const std::vector<Mask> &masks)
{
    const std::size_t start = out.size();
    const std::size_t length = 40 + 16 * std::max<std::size_t>(1, masks.size());
    out.resize(start + length, 0);
    unsigned char *r = out.data() + start;
    const auto put = [](unsigned char *p, std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            p[i] = (unsigned char)(v >> (8 * i));
    };
    put(r + 0, relationship, 4);
    put(r + 4, length, 4);
    r[8] = (unsigned char)level;
    r[9] = 8;  // associativity
    put(r + 10, std::uint64_t(lineSize), 2);
    put(r + 12, size, 4);
    put(r + 16, type, 4);
    put(r + 38, groupCount, 2);
    for (std::size_t i = 0; i < masks.size(); ++i) {
        put(r + 40 + 16 * i, masks[i].bits, 8);
        put(r + 40 + 16 * i + 8, std::uint64_t(masks[i].group), 2);
    }
}

} // namespace

class TestCacheTopology : public QObject
{
    Q_OBJECT

private slots:
    // Tiamat (9950X3D): the V-cache CCD's 96 MiB L3 on CPUs 0-7/16-23, the
    // other CCD's 32 MiB on 8-15/24-31; per core L1d 48 KiB and L2 1 MiB;
    // instruction caches dropped; 64-byte lines.
    void tiamatAsymmetricL3()
    {
        const CacheTopology v = linuxCacheTopology(fixture("tiamat"), 0);
        QCOMPARE(v.levels.size(), std::size_t(3));
        QCOMPARE(v.level(1)->type, CacheLevel::Type::Data);
        QCOMPARE(v.level(1)->sizeBytes, 48 * KiB);
        QCOMPARE(v.level(1)->lineSize, 64);
        QCOMPARE(v.level(1)->sharedWith, (std::vector<int>{0, 16}));
        QCOMPARE(v.level(2)->sizeBytes, 1 * MiB);
        QCOMPARE(v.lastLevel()->level, 3);
        QCOMPARE(v.lastLevel()->sizeBytes, 96 * MiB);
        QCOMPARE(v.lastLevel()->sharedWith, join(range(0, 7), range(16, 23)));
        QCOMPARE(v.chaseLineSize(), 64);

        const CacheTopology f = linuxCacheTopology(fixture("tiamat"), 9);
        QCOMPARE(f.lastLevel()->sizeBytes, 32 * MiB);
        QCOMPARE(f.lastLevel()->sharedWith, join(range(8, 15), range(24, 31)));
        QCOMPARE(f.level(1)->sharedWith, (std::vector<int>{9, 25}));
    }

    // Knowhere's capture has no coherency_line_size: the line size is
    // unknown and the chase uses 64 bytes.
    void knowhereWithoutLineSize()
    {
        const CacheTopology t = linuxCacheTopology(fixture("knowhere"), 0);
        QCOMPARE(t.levels.size(), std::size_t(3));
        QCOMPARE(t.level(1)->lineSize, 0);
        QCOMPARE(t.chaseLineSize(), 64);
        QCOMPARE(t.lastLevel()->sizeBytes, 32 * MiB);
        QCOMPARE(t.lastLevel()->sharedWith, join(range(0, 5), range(12, 17)));
        QCOMPARE(linuxCacheTopology(fixture("knowhere"), 6).lastLevel()->sharedWith,
                 join(range(6, 11), range(18, 23)));
    }

    // The synthetic Intel hybrid: the P-core's private L2, the E-cores'
    // shared one, one L3.
    void intelHybrid()
    {
        const CacheTopology p = linuxCacheTopology(fixture("intel-hybrid"), 0);
        QCOMPARE(p.level(1)->sizeBytes, 48 * KiB);
        QCOMPARE(p.level(2)->sizeBytes, 1280 * KiB);
        QCOMPARE(p.level(2)->sharedWith, (std::vector<int>{0, 1}));
        QCOMPARE(p.lastLevel()->sizeBytes, 12 * MiB);
        const CacheTopology e = linuxCacheTopology(fixture("intel-hybrid"), 3);
        QCOMPARE(e.level(1)->sizeBytes, 32 * KiB);
        QCOMPARE(e.level(2)->sizeBytes, 2 * MiB);
        QCOMPARE(e.level(2)->sharedWith, (std::vector<int>{2, 3}));
        QCOMPARE(e.lastLevel()->sharedWith, range(0, 3));
    }

    void armBigLittle()
    {
        const CacheTopology t = linuxCacheTopology(fixture("arm64-biglittle"), 7);
        QCOMPARE(t.levels.size(), std::size_t(3));
        QCOMPARE(t.level(1)->sizeBytes, 64 * KiB);
        QCOMPARE(t.lastLevel()->sizeBytes, 4 * MiB);
        QCOMPARE(t.lastLevel()->sharedWith, range(0, 7));
    }

    // A CPU without cache information (some ARM boards, VMs), and one that
    // doesn't exist: empty, so the RAM test finds the levels on its curve.
    void noCacheInformation()
    {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("devices/system/cpu/cpu0")));
        const CacheTopology t = linuxCacheTopology(root.path().toStdString(), 0);
        QVERIFY(t.empty());
        QVERIFY(!t.lastLevel());
        QVERIFY(!t.level(1));
        QCOMPARE(t.chaseLineSize(), 64);
        QVERIFY(linuxCacheTopology(fixture("tiamat"), 99).empty());
        QVERIFY(cacheTopologyFor(CoreChoice{}).empty());
    }

    // Windows before 11: one GroupMask per record (GroupCount 0). CPU 2's
    // caches, from a buffer that also holds other CPUs' L1s, an instruction
    // cache, a processor-core record (relationship 0) and a 128 B line L1.
    void windowsSingleGroupMask()
    {
        std::vector<unsigned char> b;
        appendRecord(b, 0, 0, 0, 0, 0, 0, {{0, 0x3}});  // RelationProcessorCore: skipped
        for (int cpu = 0; cpu < 4; ++cpu) {
            appendRecord(b, 2, 1, 2, 128, 64 * KiB, 0, {{0, 1ull << cpu}});
            appendRecord(b, 2, 1, 1, 64, 192 * KiB, 0, {{0, 1ull << cpu}});
        }
        appendRecord(b, 2, 2, 0, 64, 12 * MiB, 0, {{0, 0xF}});
        const CacheTopology t = parseWindowsCacheRecords(b.data(), b.size(), 0, 2);
        QCOMPARE(t.levels.size(), std::size_t(2));  // no L3: an Oryon-like core
        QCOMPARE(t.level(1)->type, CacheLevel::Type::Data);
        QCOMPARE(t.level(1)->sizeBytes, 64 * KiB);
        QCOMPARE(t.level(1)->sharedWith, (std::vector<int>{2}));
        QCOMPARE(t.chaseLineSize(), 128);
        QCOMPARE(t.lastLevel()->level, 2);
        QCOMPARE(t.lastLevel()->sharedWith, range(0, 3));
        // A record running past the end stops the parse; what came before stays.
        QCOMPARE(parseWindowsCacheRecords(b.data(), b.size() - 1, 0, 2).levels.size(), std::size_t(1));
        QVERIFY(parseWindowsCacheRecords(b.data(), 0, 0, 2).empty());
    }

    // Windows 11's GroupMasks: an L3 shared over two processor groups.
    // Processor 5 of group 1 (key 69) finds it, with every key of both.
    void windowsMultiGroupMasks()
    {
        std::vector<unsigned char> b;
        appendRecord(b, 2, 1, 2, 64, 48 * KiB, 1, {{1, 1ull << 5}});
        appendRecord(b, 2, 2, 0, 64, 1 * MiB, 1, {{1, 1ull << 5}});
        appendRecord(b, 2, 3, 0, 64, 32 * MiB, 2, {{0, 0xFFull << 56}, {1, 0xFF}});
        const CacheTopology t = parseWindowsCacheRecords(b.data(), b.size(), 1, 5);
        QCOMPARE(t.levels.size(), std::size_t(3));
        QCOMPARE(t.level(1)->sharedWith, (std::vector<int>{windowsKey(1, 5)}));
        QCOMPARE(t.lastLevel()->sizeBytes, 32 * MiB);
        QCOMPARE(t.lastLevel()->sharedWith, join(range(56, 63), range(64, 71)));
        // Group 0's processor 5 is in none of them.
        QVERIFY(parseWindowsCacheRecords(b.data(), b.size(), 0, 5).empty());
        // The same-LLC filter with Windows keys.
        CoreCandidate a, c;
        a.core = {true, 5, 1, 0, "test"};
        c.core = {true, 5, 0, 0, "test"};
        const std::vector<CoreCandidate> kept = sameLastLevelCache({a, c}, t);
        QCOMPARE(kept.size(), std::size_t(1));
        QCOMPARE(kept[0].core.group, 1);
    }

    // §4.3: the tie set keeps to the first candidate's LLC. Tiamat's tie
    // (CPUs 9, 13, 25, 29) is on one CCD already; a mixed set with CPU 9
    // first drops the V-cache CCD's CPUs; no topology keeps them all.
    void sameLlcFilter()
    {
        const std::vector<CoreCandidate> top = linuxTopCandidates(fixture("tiamat"), {});
        std::vector<int> cpus;
        for (const CoreCandidate &c : top)
            cpus.push_back(c.core.cpu);
        QCOMPARE(cpus, (std::vector<int>{9, 13, 25, 29}));
        QCOMPARE(sameLastLevelCache(top, linuxCacheTopology(fixture("tiamat"), 9)).size(), top.size());

        std::vector<CoreCandidate> mixed;
        for (const int cpu : {9, 0, 13, 16}) {
            CoreCandidate c;
            c.core = {true, cpu, 0, 0, "test"};
            mixed.push_back(c);
        }
        std::vector<int> kept;
        for (const CoreCandidate &c : sameLastLevelCache(mixed, linuxCacheTopology(fixture("tiamat"), 9)))
            kept.push_back(c.core.cpu);
        QCOMPARE(kept, (std::vector<int>{9, 13}));
        QCOMPARE(sameLastLevelCache(mixed, CacheTopology{}).size(), mixed.size());
    }

    // This machine: the live topology of the top core, and the filtered tie
    // set is a non-empty subset of the tie set (it always keeps the first).
    void live()
    {
        const std::vector<CoreCandidate> top = topCandidatesForSingleThread();
        const std::vector<CoreCandidate> one = topCandidatesOnOneLastLevelCache();
        QCOMPARE(one.empty(), top.empty());
        QVERIFY(one.size() <= top.size());
        if (top.empty())
            QSKIP("no core ranking here");
        QCOMPARE(one.front().core.cpu, top.front().core.cpu);
        const CacheTopology t = cacheTopologyFor(top.front().core);
        if (t.empty())
            QSKIP("the OS reports no caches here");
        QVERIFY(t.level(1));
        QVERIFY(t.chaseLineSize() >= 64);
        for (std::size_t i = 1; i < t.levels.size(); ++i)
            QVERIFY(t.levels[i].level > t.levels[i - 1].level);
    }
};

QTEST_GUILESS_MAIN(TestCacheTopology)
#include "tst_cache_topology.moc"

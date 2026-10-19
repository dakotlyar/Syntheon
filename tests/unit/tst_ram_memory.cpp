// SPDX-License-Identifier: GPL-3.0-or-later
// The RAM test's HugeBuffer (Phase 3 design §6, P7, §14.1 "HugeBuffer"): the
// smaps parser on fixtures (full, mixed, none), the THP files' parsers, the
// alignment to 2 / 32 / 512 MiB huge pages, the `pages` wording, and live
// allocations (a coverage in 0-100 and a mode string; Windows: the
// privilege-absent fallback; Stop during the first touch).
#include "tests/ram/memory/huge_buffer.h"

#include <QElapsedTimer>
#include <QTest>

#if defined(Q_OS_LINUX)
#include <sys/mman.h>
#endif

#include <cerrno>
#include <cstdio>
#include <tuple>

#include <cstring>
#include <fstream>
#include <sstream>

using namespace sb::ram;

namespace {

constexpr std::size_t KiB = 1024, MiB = 1024 * 1024;

// /proc/self/smaps around a buffer at 7f4c00000000-7f4c40000000 (1 GiB),
// between a heap mapping and a library; `rssKb` / `hugeKb` for the buffer.
std::string smaps(unsigned long long rssKb, unsigned long long hugeKb)
{
    std::ostringstream s;
    s << "55d0c8a00000-55d0c8c21000 rw-p 00000000 00:00 0                          [heap]\n"
         "Size:               2180 kB\nRss:                2048 kB\nAnonHugePages:      2048 kB\n"
         "7f4c00000000-7f4c40000000 rw-p 00000000 00:00 0 \n"
         "Size:            1048576 kB\nKernelPageSize:        4 kB\nMMUPageSize:           4 kB\n"
      << "Rss:             " << rssKb << " kB\nPss:             " << rssKb << " kB\n"
      << "AnonHugePages:   " << hugeKb << " kB\nShmemPmdMapped:        0 kB\n"
      << "THPeligible:    1\nVmFlags: rd wr mr mw me ac sd hg \n"
         "7f4c40000000-7f4c40001000 ---p 00000000 00:00 0 \n"
         "Size:                  4 kB\nRss:                   0 kB\nAnonHugePages:         0 kB\n"
         "7f4c50000000-7f4c50200000 r-xp 00000000 103:02 1234567                    /usr/lib/libc.so.6\n"
         "Size:               2048 kB\nRss:                1900 kB\nAnonHugePages:         0 kB\n";
    return s.str();
}
constexpr std::uintptr_t kBegin = 0x7f4c00000000ull, kEnd = 0x7f4c40000000ull;

#if defined(Q_OS_LINUX)
std::string readFile(const char *path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
std::string readSmaps()
{
    return readFile("/proc/self/smaps");
}
// The smaps mapping that holds `addr`: start, end, permissions.
std::tuple<std::uintptr_t, std::uintptr_t, std::string> mappingAt(const std::string &smaps, std::uintptr_t addr)
{
    std::istringstream in(smaps);
    std::string line;
    while (std::getline(in, line)) {
        unsigned long long lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line.c_str(), "%llx-%llx %7s", &lo, &hi, perms) == 3 && addr >= lo && addr < hi)
            return {std::uintptr_t(lo), std::uintptr_t(hi), perms};
    }
    return {0, 0, {}};
}
#endif

} // namespace

class TestRamMemory : public QObject
{
    Q_OBJECT

private slots:
    void smapsFullMixedNone()
    {
        const auto full = parseSmapsCoverage(smaps(1048576, 1048576), kBegin, kEnd);
        QVERIFY(full);
        QCOMPARE(full->rss, std::uint64_t(1) << 30);
        QCOMPARE(full->percent(), 100.0);  // neither the heap nor libc counted
        const auto mixed = parseSmapsCoverage(smaps(1048576, 430080), kBegin, kEnd);
        QVERIFY(qAbs(mixed->percent() - 41.015625) < 1e-9);
        QCOMPARE(parseSmapsCoverage(smaps(1048576, 0), kBegin, kEnd)->percent(), 0.0);
        // Untouched: no Rss, no coverage (not a division by zero).
        QCOMPARE(parseSmapsCoverage(smaps(0, 0), kBegin, kEnd)->percent(), 0.0);
        // A range no mapping overlaps.
        QVERIFY(!parseSmapsCoverage(smaps(1, 1), 0x1000, 0x2000));
        // A buffer split over two mappings (a partial collapse) is summed.
        const std::string split = "7f4c00000000-7f4c20000000 rw-p 00000000 00:00 0\nRss: 524288 kB\nAnonHugePages: 524288 kB\n"
                                  "7f4c20000000-7f4c40000000 rw-p 00000000 00:00 0\nRss: 524288 kB\nAnonHugePages: 0 kB\n";
        QCOMPARE(parseSmapsCoverage(split, kBegin, kEnd)->percent(), 50.0);
    }

    void thpFiles()
    {
        QCOMPARE(parseThpEnabled("[always] madvise never\n"), std::string("always"));
        QCOMPARE(parseThpEnabled("always [madvise] never\n"), std::string("madvise"));
        QCOMPARE(parseThpEnabled("always madvise [never]\n"), std::string("never"));
        QCOMPARE(parseThpEnabled(""), std::string());
        QCOMPARE(parseHugePageSize("2097152\n"), std::optional<std::size_t>(2 * MiB));
        QCOMPARE(parseHugePageSize("33554432\n"), std::optional<std::size_t>(32 * MiB));
        QCOMPARE(parseHugePageSize("536870912\n"), std::optional<std::size_t>(512 * MiB));
        QVERIFY(!parseHugePageSize(""));
        QVERIFY(!parseHugePageSize("3000000"));  // not a power of two
    }

    // §6.1 step 1, for each huge page size (and base page): the start is
    // aligned, a guard page fits below it, at most one huge page is
    // trimmed in front, and the pieces add up. The size needn't be a
    // multiple of the huge page (only the start is aligned, AP-5 L1).
    void alignment()
    {
        for (const auto [huge, page] : {std::pair{2 * MiB, 4 * KiB}, std::pair{32 * MiB, 16 * KiB},
                                        std::pair{512 * MiB, 64 * KiB}}) {
            for (const std::uintptr_t base : {std::uintptr_t(0x7f0000000000ull), std::uintptr_t(0x7f0000000000ull + page),
                                              std::uintptr_t(0x7f0000000000ull + huge - page)}) {
                for (const std::size_t size : {3 * huge, page, 48 * KiB + page}) {
                    const std::size_t mapped = size + huge + 2 * page;
                    const AlignedRange r = alignedRange(base, mapped, huge, size, page);
                    QCOMPARE(r.start % huge, std::uintptr_t(0));
                    QVERIFY(r.start >= base + page);  // room for the lower guard
                    QVERIFY(r.headTrim < huge);
                    QCOMPARE(r.headTrim + page + size + page + r.tailTrim, mapped);
                }
            }
        }
    }

    void wording()
    {
        QCOMPARE(formatPageSize(4 * KiB), std::string("4 KiB"));
        QCOMPARE(formatPageSize(2 * MiB), std::string("2 MiB"));
        QCOMPARE(formatPageSize(1024 * MiB), std::string("1 GiB"));
        QCOMPARE(formatPageSize(1536), std::string("1536 B"));
        PageReport r = linuxPageReport(100, 2 * MiB, 4 * KiB, "always");
        QCOMPARE(r.mode, PageReport::Mode::Huge);
        QCOMPARE(r.text, std::string("2 MiB transparent huge pages"));
        QCOMPARE(linuxPageReport(90, 2 * MiB, 4 * KiB, "madvise").mode, PageReport::Mode::Huge);
        r = linuxPageReport(41.7, 2 * MiB, 4 * KiB, "always");
        QCOMPARE(r.mode, PageReport::Mode::Mixed);
        QCOMPARE(r.text, std::string("mixed: 41% 2 MiB pages"));
        QCOMPARE(linuxPageReport(89.9, 2 * MiB, 4 * KiB, "always").text, std::string("mixed: 89% 2 MiB pages"));
        QCOMPARE(linuxPageReport(10, 2 * MiB, 4 * KiB, "always").mode, PageReport::Mode::Mixed);
        r = linuxPageReport(0, 2 * MiB, 4 * KiB, "never");
        QCOMPARE(r.mode, PageReport::Mode::Small);
        QCOMPARE(r.text, std::string("4 KiB pages (THP is off)"));
        QCOMPARE(linuxPageReport(3, 2 * MiB, 4 * KiB, "").text, std::string("4 KiB pages (THP is off)"));
        QCOMPARE(linuxPageReport(3, 2 * MiB, 4 * KiB, "always").text, std::string("4 KiB pages (THP: no huge pages free)"));
        QCOMPARE(linuxPageReport(100, 32 * MiB, 16 * KiB, "always").text, std::string("32 MiB transparent huge pages"));
        QCOMPARE(linuxPageReport(0, 32 * MiB, 16 * KiB, "never").text, std::string("16 KiB pages (THP is off)"));
        // smaps unreadable (AP-5 L4): unknown, not "no huge pages free".
        r = linuxPageReport(std::nullopt, 2 * MiB, 4 * KiB, "always");
        QCOMPARE(r.mode, PageReport::Mode::Unknown);
        QCOMPARE(r.hugePercent, 0.0);
        QCOMPARE(r.text, std::string("2 MiB pages requested (coverage unknown: smaps not readable)"));
    }

    // A live allocation: usable, aligned, first touched, and reported.
    void liveAllocation()
    {
        HugeBuffer b = HugeBuffer::allocate(64 * MiB + 123);
        QVERIFY2(b.valid(), b.error().c_str());
        QCOMPARE(b.size(), 64 * MiB + 123);
        QVERIFY(b.basePageSize() >= 4 * KiB);
        QVERIFY(b.firstTouch({}));
        std::memset(b.data(), 0xA5, b.size());
        QCOMPARE(std::to_integer<int>(b.data()[b.size() - 1]), 0xA5);
        const PageReport r = b.settle({});
        QVERIFY(r.hugePercent >= 0 && r.hugePercent <= 100);
        QVERIFY(!r.text.empty());
        for (const std::string &n : r.notes)
            qInfo("note: %s", n.c_str());
        qInfo("pages: %s (%.1f%% huge)", r.text.c_str(), r.hugePercent);
#if defined(Q_OS_LINUX)
        // Aligned to the THP size, so every huge page can be used.
        QVERIFY(b.hugePageSize() >= 2 * MiB);
        QCOMPARE(reinterpret_cast<std::uintptr_t>(b.data()) % b.hugePageSize(), std::uintptr_t(0));
#elif defined(Q_OS_WIN)
        // CI runners (and an ordinary admin) don't hold the privilege.
        if (r.mode == PageReport::Mode::Small)
            QVERIFY2(r.text.find("no SeLockMemoryPrivilege") != std::string::npos
                         || r.text.find("large pages failed") != std::string::npos
                         || r.text.find("no large-page support") != std::string::npos,
                     r.text.c_str());
        else
            QCOMPARE(r.hugePercent, 100.0);
#endif
    }

#if defined(Q_OS_LINUX)
    // The guard pages keep the buffer's mapping its own (AP-5 L5): the page
    // just below and just above it are PROT_NONE mappings, so nothing can be
    // mapped right against it (MAP_FIXED_NOREPLACE there fails); huge-page
    // neighbours mapped just outside the guards don't merge into it, and
    // its mapping and Rss are exactly its own size.
    void guardPagesKeepTheMappingApart()
    {
        HugeBuffer a = HugeBuffer::allocate(8 * MiB);
        QVERIFY(a.valid());
        const std::size_t page = a.basePageSize();
        std::byte *lo = a.data() - page, *hi = a.data() + 8 * MiB;
        for (std::byte *at : {hi, lo}) {
            void *m = mmap(at, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            const int err = errno;
            QCOMPARE(m, MAP_FAILED);
            QCOMPARE(err, EEXIST);
        }
        // Neighbours with the same flags, right outside the guards.
        const std::size_t n = 2 * MiB;
        void *above = mmap(hi + page, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        void *below = mmap(lo - n, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        for (void *m : {above, below}) {
            if (m != MAP_FAILED) {
                madvise(m, n, MADV_HUGEPAGE);
                std::memset(m, 1, n);
            }
        }
        QVERIFY(a.firstTouch({}));
        const std::string text = readSmaps();
        const auto begin = reinterpret_cast<std::uintptr_t>(a.data());
        const auto [start, end, perms] = mappingAt(text, begin);
        QCOMPARE(start, begin);
        QCOMPARE(end, begin + 8 * MiB);
        QCOMPARE(std::get<2>(mappingAt(text, begin - 1)), std::string("---p"));
        QCOMPARE(std::get<2>(mappingAt(text, begin + 8 * MiB)), std::string("---p"));
        const auto coverage = parseSmapsCoverage(text, begin, begin + 8 * MiB);
        QVERIFY(coverage);
        QCOMPARE(coverage->rss, std::uint64_t(8 * MiB));
        for (void *m : {above, below}) {
            if (m != MAP_FAILED)
                munmap(m, n);
        }
    }

    // Only the start is aligned (AP-5 L1): a 48 KiB buffer is a 48 KiB
    // mapping, not a huge page's worth.
    void smallBufferIsItsOwnSize()
    {
        HugeBuffer b = HugeBuffer::allocate(48 * KiB);
        QVERIFY(b.valid());
        const auto begin = reinterpret_cast<std::uintptr_t>(b.data());
        const auto [start, end, perms] = mappingAt(readSmaps(), begin);
        QCOMPARE(end - start, std::uintptr_t(48 * KiB));
        QCOMPARE(begin % b.hugePageSize(), std::uintptr_t(0));
    }

    // Stop during the one MADV_COLLAPSE (AP-5 L2) is honoured within 250
    // ms. The buffer is first touched with THP off for it, so it's on small
    // pages and settle() collapses; uncancelled, the collapse raises it.
    void stopDuringCollapseWithin250ms()
    {
        const std::string thp = parseThpEnabled(readFile("/sys/kernel/mm/transparent_hugepage/enabled"));
        if (thp.empty() || thp == "never")
            QSKIP("THP is off here: settle() doesn't collapse");
        const std::size_t size = 1024 * MiB;
        const auto smallPaged = [&] {
            HugeBuffer b = HugeBuffer::allocate(size);
            [&] { QVERIFY(b.valid()); }();
            madvise(b.data(), size, MADV_NOHUGEPAGE);
            [&] { QVERIFY(b.firstTouch({})); }();
            madvise(b.data(), size, MADV_HUGEPAGE);
            return b;
        };
        {
            HugeBuffer b = smallPaged();
            const PageReport r = b.settle({});
            QVERIFY(!r.notes.empty());
            qInfo("%s", r.notes.back().c_str());
            QVERIFY(r.notes.back().find("MADV_COLLAPSE at") == 0);
        }
        HugeBuffer b = smallPaged();
        QElapsedTimer t;
        t.start();
        qint64 stopAt = -1;
        const PageReport r = b.settle([&] {
            if (stopAt < 0 && t.elapsed() >= 20)
                stopAt = t.elapsed();
            return stopAt >= 0;
        });
        const qint64 took = t.elapsed() - std::max<qint64>(stopAt, 0);
        QVERIFY(stopAt >= 0);
        QVERIFY2(took <= 250, qPrintable(QStringLiteral("Stop took %1 ms").arg(took)));
        QVERIFY(r.notes.back().find("stopped") != std::string::npos);
    }
#endif

    // Stop during a long first touch is honoured within 250 ms (AP-5 L2).
    void stopDuringFirstTouchWithin250ms()
    {
        HugeBuffer b = HugeBuffer::allocate(2048 * MiB);
        QVERIFY(b.valid());
        QElapsedTimer t;
        t.start();
        qint64 stopAt = -1;
        QVERIFY(!b.firstTouch([&] {
            if (stopAt < 0 && t.elapsed() >= 20)
                stopAt = t.elapsed();
            return stopAt >= 0;
        }));
        const qint64 took = t.elapsed() - stopAt;
        QVERIFY2(took <= 250, qPrintable(QStringLiteral("Stop took %1 ms").arg(took)));
    }

    // A failed allocation and a moved-from buffer (AP-5 M1): firstTouch()
    // and settle() do nothing (no write through a null pointer, no smaps
    // range over the whole address space), and say so.
    void invalidBuffersAreInert()
    {
        HugeBuffer failed = HugeBuffer::allocate(std::size_t(1) << 62);  // 4 EiB: no system has it
        QVERIFY(!failed.valid());
        QVERIFY(!failed.error().empty());
        QCOMPARE(failed.basePageSize(), std::size_t(0));
        QVERIFY(!failed.firstTouch({}));
        QCOMPARE(failed.settle({}).text, std::string("no buffer"));

        HugeBuffer a = HugeBuffer::allocate(4 * MiB);
        HugeBuffer b = std::move(a);
        QVERIFY(!a.valid());  // NOLINT(bugprone-use-after-move): the moved-from state is defined
        QCOMPARE(a.basePageSize(), std::size_t(0));
        QCOMPARE(a.size(), std::size_t(0));
        QVERIFY(!a.firstTouch({}));
        QCOMPARE(a.settle({}).text, std::string("no buffer"));
        QCOMPARE(HugeBuffer{}.settle({}).mode, PageReport::Mode::Small);
        QVERIFY(b.firstTouch({}));
    }

    // allocate(0) (AP-5 L8): valid, size() 0, one base page, on both OSes.
    void zeroBytes()
    {
        HugeBuffer b = HugeBuffer::allocate(0);
        QVERIFY(b.valid());
        QCOMPARE(b.size(), std::size_t(0));
        QVERIFY(b.firstTouch({}));
        b.data()[0] = std::byte{1};
#if defined(Q_OS_LINUX)
        const auto begin = reinterpret_cast<std::uintptr_t>(b.data());
        const auto [start, end, perms] = mappingAt(readSmaps(), begin);
        QCOMPARE(end - start, std::uintptr_t(b.basePageSize()));
#endif
    }

    // Stop during the first touch: false at once, and the buffer still frees.
    void cancelDuringFirstTouch()
    {
        HugeBuffer b = HugeBuffer::allocate(256 * MiB);
        QVERIFY(b.valid());
        int polls = 0;
        QVERIFY(!b.firstTouch([&] { return ++polls > 1; }));
        QCOMPARE(polls, 2);  // after one 64 MiB chunk
    }

    void moveOnly()
    {
        HugeBuffer a = HugeBuffer::allocate(4 * MiB);
        std::byte *p = a.data();
        HugeBuffer b = std::move(a);
        QVERIFY(!a.valid());  // NOLINT(bugprone-use-after-move): the moved-from state is defined
        QCOMPARE(b.data(), p);
        HugeBuffer c;
        QVERIFY(!c.valid());
        c = std::move(b);
        QCOMPARE(c.data(), p);
        QVERIFY(c.firstTouch({}));
    }
};

QTEST_GUILESS_MAIN(TestRamMemory)
#include "tst_ram_memory.moc"

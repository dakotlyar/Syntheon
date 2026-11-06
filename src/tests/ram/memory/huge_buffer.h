// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The RAM test's buffers, on huge / large pages where the system already
// allows them (Phase 3 design §6, P7, D9, D10). Syntheon never changes a
// system setting: no writes to /sys or /proc/sys, no hugetlbfs, no
// privilege granted.
//
//   Linux    an anonymous mapping aligned to the THP size (hpage_pmd_size:
//            2 MiB with 4 KiB base pages), madvise(MADV_HUGEPAGE), first
//            touched by its owner; the real coverage is read from
//            /proc/self/smaps (AnonHugePages / Rss), and below 90% one
//            best-effort MADV_COLLAPSE (Linux 6.1+) is tried, unless THP is
//            "never" (the administrator's choice is respected). PROT_NONE
//            guard pages below and above the buffer keep its VMA from
//            merging with a neighbour, so the coverage is this buffer's.
//            Only the start is aligned (§6.1): the buffer is the requested
//            size rounded up to the base page, never to the huge page.
//   Windows  MEM_LARGE_PAGES only when the token already holds
//            SeLockMemoryPrivilege: enabled for the allocation if it isn't
//            yet, then restored to its old state; otherwise (or if the
//            allocation fails) ordinary pages. The privilege is process-wide,
//            so concurrent allocations (one per worker, §4.2) share one
//            enable, counted, and the last one out restores it.
//
// The parsers and the wording are pure functions (huge_buffer.cpp), tested
// on fixtures; the allocation is per platform (huge_buffer_linux.cpp,
// huge_buffer_win.cpp).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sb::ram {

// What a buffer got, for the `pages` and `pages.huge` metrics and the log.
struct PageReport {
    // Unknown: Linux couldn't read the coverage (smaps unreadable, or the
    // buffer's mapping not in it).
    enum class Mode { Huge, Mixed, Small, Unknown };
    Mode mode = Mode::Small;
    double hugePercent = 0;   // measured coverage (Windows: 100 or 0; Unknown: 0)
    std::string text;         // `pages`: "2 MiB transparent huge pages", "mixed: 41% 2 MiB pages", ...
    std::vector<std::string> notes;  // for the log (an madvise failure, MADV_COLLAPSE's result)
};

class HugeBuffer
{
public:
    // Reserves `bytes` of read/write memory (rounded up to the base page;
    // 0 reserves one base page and size() is 0, on both OSes), asking for
    // huge / large pages. Not touched yet (Windows large pages are
    // committed and locked by the call itself, rounded up to the large
    // page). An invalid buffer (valid() false, reason in error()) if the
    // system has no memory for it.
    static HugeBuffer allocate(std::size_t bytes);

    HugeBuffer() = default;
    ~HugeBuffer();
    HugeBuffer(HugeBuffer &&other) noexcept;
    HugeBuffer &operator=(HugeBuffer &&other) noexcept;
    HugeBuffer(const HugeBuffer &) = delete;
    HugeBuffer &operator=(const HugeBuffer &) = delete;

    bool valid() const { return m_data != nullptr; }
    const std::string &error() const { return m_error; }
    std::byte *data() const { return m_data; }
    std::size_t size() const { return m_size; }
    // The huge / large page size asked for (Linux: hpage_pmd_size; Windows:
    // GetLargePageMinimum), and the base page size.
    std::size_t hugePageSize() const { return m_hugePageSize; }
    std::size_t basePageSize() const { return m_basePageSize; }

    // First touch (§6.1 step 3): writes every base page, in 64 MiB chunks,
    // on the calling thread (the buffer's owner, so its pages are local to
    // that thread's NUMA node), checking `cancelled` between chunks. False
    // if cancelled (the buffer is then partly touched, and still freed by
    // the destructor) or if the buffer isn't valid (nothing is touched).
    bool firstTouch(const std::function<bool()> &cancelled);

    // After the first touch: the page mode and coverage, with Linux's one
    // MADV_COLLAPSE below 90% (16 MiB chunks, `cancelled` checked between
    // them, so Stop is honoured within 250 ms; a cancel skips the rest of
    // it and the report is what was measured). An invalid buffer: Small,
    // "no buffer".
    PageReport settle(const std::function<bool()> &cancelled);

private:
    void release();

    std::byte *m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_hugePageSize = 0;
    std::size_t m_basePageSize = 0;
    std::string m_error;
    // The committed, touchable length (size() rounded up to the base page;
    // Windows large pages: to the large page).
    std::size_t m_length = 0;
    // Linux: the whole mapping (the guard page, the aligned buffer, the
    // guard page) and the THP "enabled" mode it was made under. Windows:
    // whether large pages were used and, if not, why.
    std::byte *m_mapStart = nullptr;
    std::size_t m_mapLength = 0;
    std::string m_thpEnabled;
    bool m_largePages = false;
    std::string m_smallReason;
    std::vector<std::string> m_notes;
};

// What probe() and the option tab say before anything is allocated (§2):
// whether huge / large pages can be had without changing a setting. Linux:
// THP's "enabled" mode (not "never"); Windows: SeLockMemoryPrivilege in the
// process token (D10). Whether a buffer really gets them is only known after
// its first touch (HugeBuffer::settle).
struct PageSupport {
    bool huge = false;
    std::string text;  // "2 MiB transparent huge pages available", "4 KiB pages (THP is off)", ...
};
PageSupport pageSupport();

// --- Pure helpers (unit-tested) ---

// smaps (§6.1 step 4): Rss and AnonHugePages (bytes) summed over the
// mappings that overlap [begin, end). Nullopt if none does.
struct SmapsCoverage {
    std::uint64_t rss = 0;
    std::uint64_t anonHuge = 0;
    double percent() const { return rss ? 100.0 * double(anonHuge) / double(rss) : 0.0; }
};
std::optional<SmapsCoverage> parseSmapsCoverage(const std::string &smaps, std::uintptr_t begin, std::uintptr_t end);

// /sys/kernel/mm/transparent_hugepage/enabled: the bracketed choice
// ("always", "madvise", "never"); empty if the text has none.
std::string parseThpEnabled(const std::string &text);
// hpage_pmd_size: bytes; nullopt if it doesn't parse.
std::optional<std::size_t> parseHugePageSize(const std::string &text);

// §6.1 step 1: where a mapping of `mappedLength` bytes at `base` is cut to
// hold a `guard`-byte page, then `size` bytes starting on an `alignment`
// boundary (a power of two), then another guard page. headTrim bytes
// before the lower guard and tailTrim after the upper one are given back.
// The mapping must be at least size + alignment + 2 x guard long.
struct AlignedRange {
    std::uintptr_t start = 0;
    std::size_t headTrim = 0;
    std::size_t tailTrim = 0;
};
AlignedRange alignedRange(std::uintptr_t base, std::size_t mappedLength, std::size_t alignment, std::size_t size,
                          std::size_t guard);

// "4 KiB", "2 MiB", "512 MiB", "1 GiB".
std::string formatPageSize(std::size_t bytes);

// §6.1 step 6: the report for a Linux buffer with `percent` of its pages
// huge (nullopt: the coverage couldn't be read), under THP mode
// `thpEnabled` (empty: THP not available).
PageReport linuxPageReport(std::optional<double> percent, std::size_t hugePageSize, std::size_t basePageSize,
                           const std::string &thpEnabled);

} // namespace sb::ram

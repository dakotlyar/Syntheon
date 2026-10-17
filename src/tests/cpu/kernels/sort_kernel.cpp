// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/sort_kernel.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"
#include "tests/cpu/kernels/prng.h"

#include <cstddef>
#include <cstring>
#include <utility>

SB_KERNEL_TU

namespace sb::cpu {

namespace {

constexpr std::ptrdiff_t kInsertionThreshold = 24;  // insertion sort below this many elements
constexpr std::size_t kArenaStrings = 131072;       // distinct strings the seeds draw from
constexpr std::size_t kStems = 512;
constexpr std::size_t kStemLength = 12;

constexpr std::uint64_t byteSwap(std::uint64_t v)
{
    return (v >> 56) | ((v >> 40) & 0xFF00ull) | ((v >> 24) & 0xFF0000ull) | ((v >> 8) & 0xFF000000ull)
           | ((v << 8) & 0xFF00000000ull) | ((v << 24) & 0xFF0000000000ull) | ((v << 40) & 0xFF000000000000ull)
           | (v << 56);
}

// 8 bytes at p as a big-endian number (the platforms are little-endian).
inline std::uint64_t loadBE(const char *p)
{
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return byteSwap(v);
}

int floorLog2(std::size_t n)
{
    int r = 0;
    while (n >>= 1)
        ++r;
    return r;
}

// --- the introsort ------------------------------------------------------------

template <typename T, typename Less>
void insertionSort(T *a, std::ptrdiff_t n, Less less)
{
    SB_NO_VECTOR
    for (std::ptrdiff_t i = 1; i < n; ++i) {
        const T v = a[i];
        std::ptrdiff_t j = i;
        while (j > 0 && less(v, a[j - 1])) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = v;
    }
}

template <typename T, typename Less>
void siftDown(T *a, std::ptrdiff_t root, std::ptrdiff_t n, Less less)
{
    const T v = a[root];
    for (;;) {
        std::ptrdiff_t child = 2 * root + 1;
        if (child >= n)
            break;
        if (child + 1 < n && less(a[child], a[child + 1]))
            ++child;
        if (!less(v, a[child]))
            break;
        a[root] = a[child];
        root = child;
    }
    a[root] = v;
}

template <typename T, typename Less>
void heapSort(T *a, std::ptrdiff_t n, Less less)
{
    for (std::ptrdiff_t start = n / 2 - 1; start >= 0; --start)
        siftDown(a, start, n, less);
    for (std::ptrdiff_t end = n - 1; end > 0; --end) {
        std::swap(a[0], a[end]);
        siftDown(a, 0, end, less);
    }
}

template <typename T, typename Less>
void introsortLoop(T *a, std::ptrdiff_t n, int depth, Less less)
{
    while (n > kInsertionThreshold) {
        if (depth == 0) {
            heapSort(a, n, less);
            return;
        }
        --depth;
        // Median of three into the (lower) middle, which is the pivot.
        const std::ptrdiff_t mid = (n - 1) / 2;
        if (less(a[mid], a[0]))
            std::swap(a[mid], a[0]);
        if (less(a[n - 1], a[mid])) {
            std::swap(a[n - 1], a[mid]);
            if (less(a[mid], a[0]))
                std::swap(a[mid], a[0]);
        }
        const T pivot = a[mid];
        // Hoare partition: [0, j] <= pivot <= [j + 1, n).
        std::ptrdiff_t i = -1, j = n;
        for (;;) {
            do
                ++i;
            while (less(a[i], pivot));
            do
                --j;
            while (less(pivot, a[j]));
            if (i >= j)
                break;
            std::swap(a[i], a[j]);
        }
        const std::ptrdiff_t cut = j + 1;
        // Recurse into the smaller part, loop on the larger: O(log n) stack.
        if (cut < n - cut) {
            introsortLoop(a, cut, depth, less);
            a += cut;
            n -= cut;
        } else {
            introsortLoop(a + cut, n - cut, depth, less);
            n = cut;
        }
    }
    insertionSort(a, n, less);
}

template <typename T, typename Less>
void introsortRange(T *first, T *last, Less less)
{
    const std::ptrdiff_t n = last - first;
    if (n > 1)
        introsortLoop(first, n, 2 * floorLog2(std::size_t(n)), less);
}

// Four independent lanes, so the hash isn't one long dependency chain.
template <typename F>
std::uint64_t hashSequence(std::size_t n, F &&valueAt)
{
    std::uint64_t h[4] = {1, 2, 3, 4};
    SB_NO_VECTOR
    for (std::size_t i = 0; i < n; ++i)
        h[i & 3] = foldChecksum(h[i & 3], valueAt(i));
    return foldChecksum(foldChecksum(h[0], h[1]), foldChecksum(h[2], h[3]));
}

} // namespace

bool strLess(const StrRef &a, const StrRef &b, const char *arena)
{
    if (a.prefixBE != b.prefixBE)
        return a.prefixBE < b.prefixBE;
    // Equal first 8 bytes (zero-padded): either both strings are at least 8
    // long, or they are the same string of up to 8.
    const std::uint32_t la = a.len, lb = b.len;
    if (la <= 8 || lb <= 8)
        return la < lb;
    const char *pa = arena + a.offset + 8;
    const char *pb = arena + b.offset + 8;
    std::uint32_t n = (la < lb ? la : lb) - 8;
    while (n >= 8) {
        const std::uint64_t wa = loadBE(pa), wb = loadBE(pb);
        if (wa != wb)
            return wa < wb;
        pa += 8;
        pb += 8;
        n -= 8;
    }
    if (n) {
        // The last 1..7 common bytes: the top of the big-endian word.
        const unsigned shift = 64 - 8 * n;
        const std::uint64_t wa = loadBE(pa) >> shift, wb = loadBE(pb) >> shift;
        if (wa != wb)
            return wa < wb;
    }
    return la < lb;
}

void introsort(std::uint64_t *first, std::uint64_t *last)
{
    introsortRange(first, last, [](std::uint64_t x, std::uint64_t y) { return x < y; });
}

void introsort(StrRef *first, StrRef *last, const char *arena)
{
    introsortRange(first, last, [arena](const StrRef &x, const StrRef &y) { return strLess(x, y, arena); });
}

SortArena::SortArena() : m_refs(kArenaStrings)
{
    // kArenaStrings strings of 4..24 letters, each starting with (up to 12
    // letters of) one of kStems stems; one draw per statement.
    Xoshiro256ss rng(0x534F525453545253ull);  // "SORTSTRS"
    std::vector<char> stems(kStems * kStemLength);
    for (char &c : stems) {
        const std::uint64_t letter = rng.below(26);
        c = char('a' + letter);
    }
    m_text.reserve(kArenaStrings * 15 + 8);
    for (StrRef &r : m_refs) {
        const std::uint64_t stem = rng.below(kStems);
        const std::uint64_t len = 4 + rng.below(21);
        r.offset = std::uint32_t(m_text.size());
        r.len = std::uint16_t(len);
        for (std::uint64_t i = 0; i < len; ++i) {
            if (i < kStemLength) {
                m_text.push_back(stems[stem * kStemLength + i]);
            } else {
                const std::uint64_t letter = rng.below(26);
                m_text.push_back(char('a' + letter));
            }
        }
        char first8[8] = {};
        std::memcpy(first8, m_text.data() + r.offset, len < 8 ? len : 8);
        r.prefixBE = loadBE(first8);
    }
    m_text.insert(m_text.end(), 8, '\0');
}

void makeSortKeys(std::uint64_t seed, std::uint64_t *out)
{
    Xoshiro256ss rng((seed % kSeeds) ^ 0x534F52544B455953ull);  // "SORTKEYS"; one draw per statement
    SB_NO_VECTOR
    for (std::size_t i = 0; i < kSortKeys; ++i)
        out[i] = rng.next();
}

void makeSortStrings(std::uint64_t seed, const SortArena &arena, StrRef *out)
{
    Xoshiro256ss pick((seed % kSeeds) ^ 0x534F525450494B53ull);  // "SORTPIKS"
    const StrRef *refs = arena.refs();
    SB_NO_VECTOR
    for (std::size_t i = 0; i < kSortStrings; ++i) {
        const std::uint64_t k = pick.below(kArenaStrings);
        out[i] = refs[k];
    }
}

SB_NOINLINE std::uint64_t sortKeysUnit(std::uint64_t seed, std::uint64_t *work)
{
    opaque(seed);
    opaque(work);
    makeSortKeys(seed, work);
    introsort(work, work + kSortKeys);
    return hashSequence(kSortKeys, [work](std::size_t i) { return work[i]; });
}

SB_NOINLINE std::uint64_t sortStringsUnit(std::uint64_t seed, const SortArena &arena, StrRef *work)
{
    opaque(seed);
    opaque(work);
    const char *text = arena.text();
    opaque(text);
    makeSortStrings(seed, arena, work);
    introsort(work, work + kSortStrings, text);
    return hashSequence(kSortStrings,
                        [work](std::size_t i) { return (std::uint64_t(work[i].offset) << 16) | work[i].len; });
}

std::unique_ptr<KernelThread> SortKeysKernel::makeThread(int) const
{
    struct T : KernelThread {
        std::vector<std::uint64_t> work = std::vector<std::uint64_t>(kSortKeys, 0);  // touched here
        std::uint64_t unit(std::uint64_t unitIndex) override { return sortKeysUnit(unitIndex % kSeeds, work.data()); }
    };
    return std::make_unique<T>();
}

std::optional<std::uint64_t> SortKeysKernel::golden(std::uint64_t seed) const
{
    return golden::kSortKeys[seed % kSeeds];
}

std::unique_ptr<KernelThread> SortStringsKernel::makeThread(int) const
{
    struct T : KernelThread {
        const SortArena *arena;
        std::vector<StrRef> work = std::vector<StrRef>(kSortStrings, StrRef{});  // touched here
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            return sortStringsUnit(unitIndex % kSeeds, *arena, work.data());
        }
    };
    auto t = std::make_unique<T>();
    t->arena = &m_arena;
    return t;
}

std::optional<std::uint64_t> SortStringsKernel::golden(std::uint64_t seed) const
{
    return golden::kSortStrings[seed % kSeeds];
}

} // namespace sb::cpu

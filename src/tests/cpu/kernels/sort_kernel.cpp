// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/sort_kernel.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"
#include "tests/cpu/kernels/prng.h"

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

SortInputs::SortInputs() : m_keys(kSeeds * kSortKeys), m_refs(kSeeds * kSortStrings)
{
    // The keys: one stream per seed, one draw per statement.
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        Xoshiro256ss rng(s ^ 0x534F52544B455953ull);  // "SORTKEYS"
        for (std::size_t i = 0; i < kSortKeys; ++i)
            m_keys[s * kSortKeys + i] = rng.next();
    }
    // The arena: kArenaStrings strings of 4..24 letters, each starting with
    // (up to 12 letters of) one of kStems stems.
    Xoshiro256ss rng(0x534F525453545253ull);  // "SORTSTRS"
    std::vector<char> stems(kStems * kStemLength);
    for (char &c : stems) {
        const std::uint64_t letter = rng.below(26);
        c = char('a' + letter);
    }
    std::vector<StrRef> all(kArenaStrings);
    m_arena.reserve(kArenaStrings * 15 + 8);
    for (StrRef &r : all) {
        const std::uint64_t stem = rng.below(kStems);
        const std::uint64_t len = 4 + rng.below(21);
        r.offset = std::uint32_t(m_arena.size());
        r.len = std::uint16_t(len);
        for (std::uint64_t i = 0; i < len; ++i) {
            if (i < kStemLength) {
                m_arena.push_back(stems[stem * kStemLength + i]);
            } else {
                const std::uint64_t letter = rng.below(26);
                m_arena.push_back(char('a' + letter));
            }
        }
        char first8[8] = {};
        std::memcpy(first8, m_arena.data() + r.offset, len < 8 ? len : 8);
        r.prefixBE = loadBE(first8);
    }
    m_arena.insert(m_arena.end(), 8, '\0');
    // Each seed's strings: kSortStrings drawn from the arena.
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        Xoshiro256ss pick(s ^ 0x534F525450494B53ull);  // "SORTPIKS"
        for (std::size_t i = 0; i < kSortStrings; ++i) {
            const std::uint64_t k = pick.below(kArenaStrings);
            m_refs[s * kSortStrings + i] = all[k];
        }
    }
}

std::span<const std::uint64_t> SortInputs::keys(std::uint64_t seed) const
{
    return std::span(m_keys).subspan(std::size_t(seed % kSeeds) * kSortKeys, kSortKeys);
}

std::span<const StrRef> SortInputs::strings(std::uint64_t seed) const
{
    return std::span(m_refs).subspan(std::size_t(seed % kSeeds) * kSortStrings, kSortStrings);
}

SB_NOINLINE std::uint64_t sortKeysUnit(std::span<const std::uint64_t> pristine, std::uint64_t *work)
{
    const std::uint64_t *src = pristine.data();
    opaque(src);
    opaque(work);
    std::memcpy(work, src, pristine.size() * sizeof(std::uint64_t));
    introsort(work, work + pristine.size());
    return hashSequence(pristine.size(), [work](std::size_t i) { return work[i]; });
}

SB_NOINLINE std::uint64_t sortStringsUnit(std::span<const StrRef> pristine, StrRef *work, const char *arena)
{
    const StrRef *src = pristine.data();
    opaque(src);
    opaque(work);
    opaque(arena);
    std::memcpy(work, src, pristine.size() * sizeof(StrRef));
    introsort(work, work + pristine.size(), arena);
    return hashSequence(pristine.size(),
                        [work](std::size_t i) { return (std::uint64_t(work[i].offset) << 16) | work[i].len; });
}

std::unique_ptr<KernelThread> SortKeysKernel::makeThread(int) const
{
    struct T : KernelThread {
        const SortInputs *inputs;
        std::vector<std::uint64_t> work = std::vector<std::uint64_t>(kSortKeys, 0);  // touched here
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            return sortKeysUnit(inputs->keys(unitIndex % kSeeds), work.data());
        }
    };
    auto t = std::make_unique<T>();
    t->inputs = &m_inputs;
    return t;
}

std::optional<std::uint64_t> SortKeysKernel::golden(std::uint64_t seed) const
{
    return golden::kSortKeys[seed % kSeeds];
}

std::unique_ptr<KernelThread> SortStringsKernel::makeThread(int) const
{
    struct T : KernelThread {
        const SortInputs *inputs;
        std::vector<StrRef> work = std::vector<StrRef>(kSortStrings, StrRef{});  // touched here
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            return sortStringsUnit(inputs->strings(unitIndex % kSeeds), work.data(), inputs->arena());
        }
    };
    auto t = std::make_unique<T>();
    t->inputs = &m_inputs;
    return t;
}

std::optional<std::uint64_t> SortStringsKernel::golden(std::uint64_t seed) const
{
    return golden::kSortStrings[seed % kSeeds];
}

} // namespace sb::cpu

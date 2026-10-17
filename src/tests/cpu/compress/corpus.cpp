// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/compress/corpus.h"

#include "tests/cpu/kernels/prng.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <thread>

namespace sb::cpu {

namespace {

// The block's sections (§5.4: 50 / 20 / 20 / 10 %).
constexpr std::size_t kTextBytes = kCorpusBlockSize / 2;           // 131072
constexpr std::size_t kJsonBytes = kCorpusBlockSize / 5;           // 52428
constexpr std::size_t kBinaryBytes = kCorpusBlockSize / 5;         // 52428
constexpr std::size_t kRandomBytes = kCorpusBlockSize - kTextBytes - kJsonBytes - kBinaryBytes;  // 26216

// Every section draws from its own stream: block and section in the seed.
constexpr std::uint64_t sectionSeed(std::size_t block, unsigned section)
{
    return 0x53594E42454E4300ull ^ (std::uint64_t(block) << 8) ^ section;  // "SYNBENC\0"
}

// Appends to a fixed span and silently stops at its end.
class Writer
{
public:
    explicit Writer(std::span<std::uint8_t> out) : m_out(out) {}
    bool full() const { return m_pos >= m_out.size(); }
    void put(std::uint8_t c)
    {
        if (m_pos < m_out.size())
            m_out[m_pos++] = c;
    }
    void put(std::string_view s)
    {
        for (char c : s)
            put(std::uint8_t(c));
    }
    void putNumber(long long v)
    {
        // Own decimal formatting: no locale, no printf.
        char buf[24];
        int n = 0;
        const bool negative = v < 0;
        unsigned long long u = negative ? 0ull - (unsigned long long)v : (unsigned long long)v;
        do {
            buf[n++] = char('0' + u % 10);
            u /= 10;
        } while (u);
        if (negative)
            put('-');
        while (n)
            put(std::uint8_t(buf[--n]));
    }
    void putLe(std::uint64_t v, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
            put(std::uint8_t(v >> (8 * i)));
    }
    std::size_t lineLength = 0;

private:
    std::span<std::uint8_t> m_out;
    std::size_t m_pos = 0;
};

// The generated vocabulary: 2048 words of 1-4 syllables, the same for every
// block (one fixed seed).
class Vocabulary
{
public:
    Vocabulary()
    {
        static constexpr std::array<std::string_view, 24> onsets = {
            "b", "c", "d", "f", "g", "h", "j", "k", "l", "m", "n", "p",
            "r", "s", "t", "v", "w", "z", "ch", "sh", "th", "st", "tr", "pl"};
        static constexpr std::array<std::string_view, 8> vowels = {"a", "e", "i", "o", "u", "ai", "ea", "ou"};
        static constexpr std::array<std::string_view, 8> codas = {"", "", "n", "r", "s", "t", "l", "nd"};
        Xoshiro256ss rng(0x564F434142ull);  // "VOCAB"
        std::array<std::string, 96> syllables;
        for (std::string &s : syllables) {
            s = std::string(onsets[rng.below(onsets.size())]) + std::string(vowels[rng.below(vowels.size())])
                + std::string(codas[rng.below(codas.size())]);
        }
        for (std::string &w : m_words) {
            const std::uint64_t n = 1 + rng.below(4);
            for (std::uint64_t i = 0; i < n; ++i)
                w += syllables[rng.below(syllables.size())];
        }
    }
    // Zipf-like: index below(below(N) + 1) favours the first words, as real text does.
    const std::string &pick(Xoshiro256ss &rng) const { return m_words[rng.below(rng.below(m_words.size()) + 1)]; }

private:
    std::array<std::string, 2048> m_words;
};

const Vocabulary &vocabulary()
{
    static const Vocabulary v;
    return v;
}

void text(Writer &w, Xoshiro256ss &rng)
{
    const Vocabulary &v = vocabulary();
    int sentences = 0;
    while (!w.full()) {
        const std::uint64_t words = 5 + rng.below(14);
        for (std::uint64_t i = 0; i < words; ++i) {
            std::string word = v.pick(rng);
            if (i == 0 && word[0] >= 'a' && word[0] <= 'z')
                word[0] = char(word[0] - 'a' + 'A');
            if (i + 1 < words && rng.below(8) == 0)
                word += ',';
            if (i + 1 == words)
                word += '.';
            if (w.lineLength + word.size() + 1 > 72) {
                w.put('\n');
                w.lineLength = 0;
            } else if (w.lineLength > 0) {
                w.put(' ');
                ++w.lineLength;
            }
            w.put(word);
            w.lineLength += word.size();
        }
        if (++sentences % 6 == 0) {
            w.put("\n\n");
            w.lineLength = 0;
        }
    }
}

void json(Writer &w, Xoshiro256ss &rng, std::size_t block)
{
    const Vocabulary &v = vocabulary();
    long long id = (long long)block * 100000;
    while (!w.full()) {
        id += 1 + (long long)rng.below(5);
        if (rng.below(3) != 0) {
            w.put("{\"id\": ");
            w.putNumber(id);
            w.put(", \"user\": \"");
            w.put(v.pick(rng));
            w.put("\", \"score\": ");
            w.putNumber((long long)rng.below(2001) - 1000);
            w.put(", \"active\": ");
            w.put(rng.below(2) ? "true" : "false");
            w.put(", \"tags\": [\"");
            w.put(v.pick(rng));
            w.put("\", \"");
            w.put(v.pick(rng));
            w.put("\"]}\n");
        } else {
            w.put("    ");
            w.put(v.pick(rng));
            w.put("_");
            w.put(v.pick(rng));
            w.put(" = ");
            w.putNumber((long long)rng.below(100000));
            w.put(";  // ");
            w.put(v.pick(rng));
            w.put(" ");
            w.put(v.pick(rng));
            w.put("\n");
        }
    }
}

// 16-byte records: u32 time (small steps), u16 sensor, i16 value (a small
// random walk), u32 counter, u32 flags (mostly 0).
void binary(Writer &w, Xoshiro256ss &rng, std::size_t block)
{
    std::uint32_t time = std::uint32_t(block) * 1000003u;
    std::uint32_t counter = 0;
    int value = 0;
    while (!w.full()) {
        time += std::uint32_t(rng.below(16));
        value = std::clamp(value + int(rng.below(9)) - 4, -30000, 30000);
        w.putLe(time, 4);
        w.putLe(rng.below(64), 2);
        w.putLe(std::uint16_t(std::int16_t(value)), 2);
        w.putLe(++counter, 4);
        w.putLe(rng.below(4) == 0 ? rng.below(256) : 0, 4);
    }
}

void random(Writer &w, Xoshiro256ss &rng)
{
    while (!w.full())
        w.putLe(rng.next(), 8);
}

} // namespace

void generateCorpusBlock(std::size_t index, std::span<std::uint8_t> out)
{
    std::span<std::uint8_t> rest = out.first(kCorpusBlockSize);
    const auto section = [&](std::size_t bytes, unsigned id, auto &&fill) {
        Writer w(rest.first(bytes));
        Xoshiro256ss rng(sectionSeed(index, id));
        fill(w, rng);
        rest = rest.subspan(bytes);
    };
    section(kTextBytes, 0, [](Writer &w, Xoshiro256ss &rng) { text(w, rng); });
    section(kJsonBytes, 1, [&](Writer &w, Xoshiro256ss &rng) { json(w, rng, index); });
    section(kBinaryBytes, 2, [&](Writer &w, Xoshiro256ss &rng) { binary(w, rng, index); });
    section(kRandomBytes, 3, [](Writer &w, Xoshiro256ss &rng) { random(w, rng); });
}

std::vector<std::uint8_t> generateCorpus(int threads)
{
    std::vector<std::uint8_t> corpus(kCorpusSize);
    (void)vocabulary();  // built once, before the threads share it
    const int n = std::clamp(threads, 1, int(kCorpusBlocks));
    const auto work = [&](int t) {
        for (std::size_t b = std::size_t(t); b < kCorpusBlocks; b += std::size_t(n))
            generateCorpusBlock(b, std::span(corpus).subspan(b * kCorpusBlockSize, kCorpusBlockSize));
    };
    if (n == 1) {
        work(0);
        return corpus;
    }
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t)
        pool.emplace_back(work, t);
    for (std::thread &t : pool)
        t.join();
    return corpus;
}

std::uint32_t crc32Of(std::span<const std::uint8_t> data)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    // zlib takes uInt lengths: feed it in 1 GiB pieces at most.
    while (!data.empty()) {
        const std::size_t n = std::min<std::size_t>(data.size(), std::size_t(1) << 30);
        crc = crc32(crc, data.data(), uInt(n));
        data = data.subspan(n);
    }
    return std::uint32_t(crc);
}

} // namespace sb::cpu

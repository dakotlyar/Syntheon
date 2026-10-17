// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The compression corpus (Phase 2 design §5.4, P11): 32 MiB generated at
// prepare time from SynBench's own PRNG, integer arithmetic only, so it is
// byte-identical on every platform (no bundled data). 128 blocks of 256 KiB;
// a compress unit works on one block.
//
// Each block is generated from its own index, so blocks can be made in
// parallel and the corpus is the same however many threads made it. A block
// holds the corpus mix in this order:
//   50%  pseudo-text: words from a generated syllable vocabulary, sentences,
//        line breaks
//   20%  JSON / source-like key-value lines
//   20%  structured binary records: small-delta little-endian integers
//   10%  incompressible random bytes
// The golden value is the CRC-32 (zlib's crc32()) of the whole corpus
// (golden/corpus.inc): it catches PRNG or generator drift between platforms.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sb::cpu {

inline constexpr std::size_t kCorpusBlockSize = 256 * 1024;
inline constexpr std::size_t kCorpusBlocks = 128;
inline constexpr std::size_t kCorpusSize = kCorpusBlockSize * kCorpusBlocks;  // 32 MiB

// Block `index` (0..127), exactly kCorpusBlockSize bytes, into `out`.
void generateCorpusBlock(std::size_t index, std::span<std::uint8_t> out);
// The whole corpus, made by `threads` threads (1 = this thread).
std::vector<std::uint8_t> generateCorpus(int threads = 1);
// zlib crc32() of `data`.
std::uint32_t crc32Of(std::span<const std::uint8_t> data);

} // namespace sb::cpu

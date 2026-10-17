// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The encryption kernels of synbench-cpucrypto (Phase 2 design §5.5, P12),
// on the CPU test's measurement engine, through OpenSSL's EVP interface. No
// Qt: this is the worker's code only.
//
// One unit, for a seed s in 0..63, is 64 records of 16 KiB (one TLS record
// each), 1 MiB in all:
//   AES-256-GCM  one message (§5.5; Dave, 2026-10-17): a key and a 12-byte IV
//                from the seed, then 64 EVP_EncryptUpdate calls of 16 KiB, the
//                final call and the 16-byte tag
//   SHA-256      per record a digest of a 16-byte header (seed, record number)
//                followed by the record
// The records are the same fixed 16 KiB of data on every thread (made in
// makeThread). The unit's checksum folds the tag or the 64 digests;
// golden/crypto.inc (made by `synbench-cpucrypto --print-golden`, a
// developer build only) holds it per seed. Every unit is checked against it,
// on the hardware and the software path alike: two implementations agreeing.

#include "tests/cpu/engine/kernel.h"

#include <cstdint>
#include <optional>
#include <string>

struct evp_cipher_st;
struct evp_md_st;

namespace sb::cpu::crypto {

enum class Alg { AesGcm, Sha256 };
inline constexpr std::size_t kRecordSize = 16 * 1024;
inline constexpr std::size_t kRecordsPerUnit = 64;
inline constexpr std::size_t kBytesPerUnit = kRecordSize * kRecordsPerUnit;

const char *algName(Alg a);  // "AES-256-GCM", "SHA-256"
const char *algArg(Alg a);   // "aes-256-gcm", "sha-256" (the worker's --alg)
std::optional<Alg> algFromArg(const std::string &arg);

// The golden checksum of one unit of `alg` for `seed`.
std::uint64_t goldenUnit(Alg alg, std::uint64_t seed);
// The checksum of one unit, computed here on this thread (the golden table
// generator and the worker's checksum pass).
std::uint64_t computeUnit(Alg alg, std::uint64_t seed);
// The known-answer tests (§5.5): NIST GCM test cases 13-16 for AES-256 and
// the SHA-256 vectors for "", "abc" and one million 'a'. Nothing if all
// pass, else what failed.
std::optional<std::string> knownAnswerFailure();

class CryptoKernel : public sb::cpu::Kernel
{
public:
    // `firstSeed`: thread t's first unit has seed firstSeed + t (mod 64).
    CryptoKernel(Alg alg, std::uint64_t firstSeed);
    ~CryptoKernel() override;
    CryptoKernel(const CryptoKernel &) = delete;
    CryptoKernel &operator=(const CryptoKernel &) = delete;

    std::string name() const override { return algName(m_alg); }
    double workPerUnit() const override { return double(kBytesPerUnit); }
    // P14, bytes per cycle per thread: AES-GCM with VAES does about 3, SHA-256
    // with SHA-NI under 1.
    double maxWorkPerCyclePerThread() const override { return m_alg == Alg::AesGcm ? 16 : 8; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    Alg m_alg;
    std::uint64_t m_firstSeed;
    evp_cipher_st *m_cipher = nullptr;  // fetched once, shared (EVP objects are thread-safe)
    evp_md_st *m_md = nullptr;
};

} // namespace sb::cpu::crypto

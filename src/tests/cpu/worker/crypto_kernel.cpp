// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/worker/crypto_kernel.h"

#include "tests/cpu/kernels/kernel_common.h"
#include "tests/cpu/kernels/prng.h"

#include <openssl/evp.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sb::cpu::crypto {

namespace golden {
#include "tests/cpu/golden/crypto.inc"
}

namespace {

std::vector<std::uint8_t> fromHex(const char *hex)
{
    std::vector<std::uint8_t> out;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        const auto nibble = [](char c) { return std::uint8_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
        out.push_back(std::uint8_t(nibble(p[0]) << 4 | nibble(p[1])));
    }
    return out;
}

std::uint64_t loadLe64(const std::uint8_t *p)
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

void storeLe64(std::uint8_t *p, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = std::uint8_t(v >> (8 * i));
}

// The same 16 KiB on every thread and platform.
std::vector<std::uint8_t> recordData()
{
    std::vector<std::uint8_t> data(kRecordSize);
    Xoshiro256ss rng(0x52454344415441ull);  // "RECDATA"
    for (std::size_t i = 0; i < data.size(); i += 8)
        storeLe64(data.data() + i, rng.next());
    return data;
}

[[noreturn]] void openSslFailed(const char *what)
{
    throw std::runtime_error(std::string("OpenSSL: ") + what + " failed");
}

// One AES-256-GCM unit (see crypto_kernel.h): one message.
std::uint64_t aesUnit(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, std::uint64_t seed, const std::uint8_t *data,
                      std::uint8_t *out)
{
    std::uint8_t key[32], ivBytes[16];
    SplitMix64 sm(seed ^ 0x41455347434D4B59ull);  // "AESGCMKY"; one draw per statement
    for (int i = 0; i < 4; ++i)
        storeLe64(key + 8 * i, sm.next());
    storeLe64(ivBytes, sm.next());
    storeLe64(ivBytes + 8, sm.next());
    // The 12-byte IV: the first 12 of those 16 bytes.
    if (EVP_EncryptInit_ex2(ctx, cipher, key, ivBytes, nullptr) != 1)
        openSslFailed("EVP_EncryptInit_ex2");
    for (std::size_t r = 0; r < kRecordsPerUnit; ++r) {
        int n = 0;
        if (EVP_EncryptUpdate(ctx, out, &n, data, int(kRecordSize)) != 1 || std::size_t(n) != kRecordSize)
            openSslFailed("EVP_EncryptUpdate");
    }
    int tail = 0;
    std::uint8_t tag[16];
    if (EVP_EncryptFinal_ex(ctx, out, &tail) != 1
        || EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, int(sizeof tag), tag) != 1)
        openSslFailed("AES-256-GCM final / tag");
    return foldChecksum(foldChecksum(0, loadLe64(tag)), loadLe64(tag + 8));
}

// One SHA-256 unit.
std::uint64_t shaUnit(EVP_MD_CTX *ctx, const EVP_MD *md, std::uint64_t seed, const std::uint8_t *data)
{
    std::uint64_t sum = 0;
    for (std::size_t r = 0; r < kRecordsPerUnit; ++r) {
        std::uint8_t header[16], digest[EVP_MAX_MD_SIZE];
        storeLe64(header, seed);
        storeLe64(header + 8, r);
        unsigned int len = 0;
        if (EVP_DigestInit_ex2(ctx, md, nullptr) != 1 || EVP_DigestUpdate(ctx, header, sizeof header) != 1
            || EVP_DigestUpdate(ctx, data, kRecordSize) != 1 || EVP_DigestFinal_ex(ctx, digest, &len) != 1 || len != 32)
            openSslFailed("SHA-256 record");
        for (int w = 0; w < 4; ++w)
            sum = foldChecksum(sum, loadLe64(digest + 8 * w));
    }
    return sum;
}

// A thread's OpenSSL state for one algorithm.
struct Contexts {
    EVP_CIPHER_CTX *cipherCtx = nullptr;
    EVP_MD_CTX *mdCtx = nullptr;
    std::vector<std::uint8_t> data = recordData();
    std::vector<std::uint8_t> out = std::vector<std::uint8_t>(kRecordSize + 16, 0);  // touched here
    Contexts() : cipherCtx(EVP_CIPHER_CTX_new()), mdCtx(EVP_MD_CTX_new())
    {
        if (!cipherCtx || !mdCtx) {
            EVP_CIPHER_CTX_free(cipherCtx);
            EVP_MD_CTX_free(mdCtx);
            throw std::runtime_error("OpenSSL: no memory for a context");
        }
    }
    ~Contexts()
    {
        EVP_CIPHER_CTX_free(cipherCtx);
        EVP_MD_CTX_free(mdCtx);
    }
    Contexts(const Contexts &) = delete;
    Contexts &operator=(const Contexts &) = delete;
    std::uint64_t unit(Alg alg, const EVP_CIPHER *cipher, const EVP_MD *md, std::uint64_t seed)
    {
        return alg == Alg::AesGcm ? aesUnit(cipherCtx, cipher, seed, data.data(), out.data())
                                  : shaUnit(mdCtx, md, seed, data.data());
    }
};

struct Fetched {
    EVP_CIPHER *cipher = EVP_CIPHER_fetch(nullptr, "AES-256-GCM", nullptr);
    EVP_MD *md = EVP_MD_fetch(nullptr, "SHA2-256", nullptr);
    Fetched()
    {
        if (!cipher || !md)
            openSslFailed("fetching AES-256-GCM / SHA2-256");
    }
    ~Fetched()
    {
        EVP_CIPHER_free(cipher);
        EVP_MD_free(md);
    }
    Fetched(const Fetched &) = delete;
    Fetched &operator=(const Fetched &) = delete;
};

bool gcmCase(const char *key, const char *iv, const char *aad, const char *plain, const char *cipherText,
             const char *tag)
{
    const auto k = fromHex(key), v = fromHex(iv), a = fromHex(aad), p = fromHex(plain), c = fromHex(cipherText),
               t = fromHex(tag);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    std::vector<std::uint8_t> out(p.size() + 16);
    std::uint8_t got[16];
    int n = 0, m = 0;
    bool ok = ctx && EVP_EncryptInit_ex2(ctx, EVP_aes_256_gcm(), k.data(), v.data(), nullptr) == 1
              && (a.empty() || EVP_EncryptUpdate(ctx, nullptr, &n, a.data(), int(a.size())) == 1)
              && EVP_EncryptUpdate(ctx, out.data(), &n, p.data(), int(p.size())) == 1
              && EVP_EncryptFinal_ex(ctx, out.data() + n, &m) == 1
              && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, got) == 1;
    EVP_CIPHER_CTX_free(ctx);
    ok = ok && std::size_t(n + m) == c.size() && std::equal(c.begin(), c.end(), out.begin())
         && std::equal(t.begin(), t.end(), got);
    return ok;
}

bool shaCase(const std::vector<std::uint8_t> &message, const char *digest)
{
    std::uint8_t got[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    const auto want = fromHex(digest);
    return EVP_Digest(message.data(), message.size(), got, &len, EVP_sha256(), nullptr) == 1 && len == want.size()
           && std::equal(want.begin(), want.end(), got);
}

} // namespace

const char *algName(Alg a)
{
    return a == Alg::AesGcm ? "AES-256-GCM" : "SHA-256";
}

const char *algArg(Alg a)
{
    return a == Alg::AesGcm ? "aes-256-gcm" : "sha-256";
}

std::optional<Alg> algFromArg(const std::string &arg)
{
    if (arg == "aes-256-gcm")
        return Alg::AesGcm;
    if (arg == "sha-256")
        return Alg::Sha256;
    return std::nullopt;
}

std::uint64_t goldenUnit(Alg alg, std::uint64_t seed)
{
    return alg == Alg::AesGcm ? golden::kAesGcm[seed % kSeeds] : golden::kSha256[seed % kSeeds];
}

std::uint64_t computeUnit(Alg alg, std::uint64_t seed)
{
    const Fetched f;
    Contexts c;
    return c.unit(alg, f.cipher, f.md, seed);
}

std::optional<std::string> knownAnswerFailure()
{
    // The GCM specification's test cases 13-16 (AES-256), as in NIST's GCM
    // validation suite.
    const char *k0 = "0000000000000000000000000000000000000000000000000000000000000000";
    const char *iv0 = "000000000000000000000000";
    const char *k1 = "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308";
    const char *iv1 = "cafebabefacedbaddecaf888";
    const char *p1 = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                     "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255";
    const char *c1 = "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
                     "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad";
    const char *p2 = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                     "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39";
    const char *c2 = "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
                     "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662";
    if (!gcmCase(k0, iv0, "", "", "", "530f8afbc74536b9a963b4f1c4cb738b"))
        return "AES-256-GCM known-answer test 13 failed";
    if (!gcmCase(k0, iv0, "", "00000000000000000000000000000000", "cea7403d4d606b6e074ec5d3baf39d18",
                 "d0d1c8a799996bf0265b98b5d48ab919"))
        return "AES-256-GCM known-answer test 14 failed";
    if (!gcmCase(k1, iv1, "", p1, c1, "b094dac5d93471bdec1a502270e3cc6c"))
        return "AES-256-GCM known-answer test 15 failed";
    if (!gcmCase(k1, iv1, "feedfacedeadbeeffeedfacedeadbeefabaddad2", p2, c2, "76fc6ece0f4e1768cddf8853bb2d551b"))
        return "AES-256-GCM known-answer test 16 failed";
    // FIPS 180-2's SHA-256 examples.
    if (!shaCase({}, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))
        return "SHA-256 known-answer test (empty message) failed";
    if (!shaCase({'a', 'b', 'c'}, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
        return "SHA-256 known-answer test (\"abc\") failed";
    if (!shaCase(std::vector<std::uint8_t>(1000000, 'a'),
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"))
        return "SHA-256 known-answer test (one million 'a') failed";
    return std::nullopt;
}

CryptoKernel::CryptoKernel(Alg alg, std::uint64_t firstSeed)
    : m_alg(alg), m_firstSeed(firstSeed % kSeeds), m_cipher(EVP_CIPHER_fetch(nullptr, "AES-256-GCM", nullptr)),
      m_md(EVP_MD_fetch(nullptr, "SHA2-256", nullptr))
{
    if (!m_cipher || !m_md) {
        EVP_CIPHER_free(m_cipher);
        EVP_MD_free(m_md);
        openSslFailed("fetching AES-256-GCM / SHA2-256");
    }
}

CryptoKernel::~CryptoKernel()
{
    EVP_CIPHER_free(m_cipher);
    EVP_MD_free(m_md);
}

std::unique_ptr<KernelThread> CryptoKernel::makeThread(int) const
{
    struct T : KernelThread {
        const CryptoKernel *k;
        Contexts c;
        std::uint64_t unit(std::uint64_t unitIndex) override
        {
            return c.unit(k->m_alg, k->m_cipher, k->m_md, (unitIndex + k->m_firstSeed) % kSeeds);
        }
    };
    auto t = std::make_unique<T>();
    t->k = this;
    return t;
}

std::optional<std::uint64_t> CryptoKernel::golden(std::uint64_t seed) const
{
    return goldenUnit(m_alg, (seed + m_firstSeed) % kSeeds);
}

} // namespace sb::cpu::crypto

// SPDX-License-Identifier: GPL-3.0-or-later
// synbench-cpucrypto: the CPU test's encryption worker (Phase 2 design §5.5,
// D5-D8, P12, P17). No Qt; OpenSSL 3.5 (libcrypto) is linked statically.
//
// Its own process because OpenSSL reads OPENSSL_ia32cap / OPENSSL_armcap
// once, at start-up (on Linux in a constructor before main()): the hardware
// and the software path are two runs of this program, the software one
// started with the mask in its environment. It also keeps OpenSSL out of the
// GUI process, which is elevated on Windows.
//
//   synbench-cpucrypto --alg aes-256-gcm|sha-256 --threads N --warmup-ms W
//                      --rep-ms L --reps R [--seed S]
//   synbench-cpucrypto --self-test
//
// stdout, one JSON object per line (C locale, flushed per line):
//   {"ev":"ready","openssl":"3.5.9","features":"aes,pclmul,sha,vaes,vpclmulqdq","masked":false}
//   {"ev":"window","i":0}            a window starts (0 = warm-up, then 1..R)
//   {"ev":"rep","i":1,"mbps":...}    each repetition, after the last window
//   {"ev":"result","median_mbps":...,"spread":...,"checksum":"0x..."}
//   {"ev":"self-test","ok":true,"openssl":"3.5.9"}
// Exit code 0; otherwise 1 (an error: its reason is the last line on
// stderr), 2 (bad arguments) or 3 (stdin was closed: SynBench is gone, so
// the worker never outlives it).
//
// The run: the known-answer tests, then the checksum pass (one unit of each
// seed 0..63 on this thread, each against the golden table; their checksums
// XOR-folded give "checksum", the same on the hardware and the software
// path), then the measurement on N threads (every unit against the golden
// table again).
//
// Hardening (P17), before anything else: on Windows the DLL search is
// limited to System32 and the program's own folder; OPENSSL_CONF,
// OPENSSL_MODULES and OPENSSL_ENGINES are removed from the environment (the
// client never passes them either); OpenSSL is initialised with
// OPENSSL_INIT_NO_LOAD_CONFIG (and is built with no-autoload-config,
// no-module, no-dso, no-engine), so no configuration file, module or engine
// is ever loaded. Nothing asks OpenSSL for its directories.
#include "isa/cpu_features.h"
#include "tests/cpu/engine/platform_threads.h"
#include "tests/cpu/engine/worker_pool.h"
#include "tests/cpu/worker/crypto_kernel.h"

#include <openssl/crypto.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#define SB_ENVIRON _environ
#else
#include <unistd.h>
extern "C" char **environ;
#define SB_ENVIRON environ
#endif

using namespace sb::cpu;
using sb::cpu::crypto::Alg;

namespace {

void hardenProcess()
{
#if defined(_WIN32)
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
#endif
    for (const char *name : {"OPENSSL_CONF", "OPENSSL_MODULES", "OPENSSL_ENGINES"}) {
#if defined(_WIN32)
        SetEnvironmentVariableA(name, nullptr);
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
}

void line(const std::string &json)
{
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

[[noreturn]] void failWith(const std::string &reason, int code = 1)
{
    std::fflush(stdout);
    std::fprintf(stderr, "synbench-cpucrypto: %s\n", reason.c_str());
    std::fflush(stderr);
    std::exit(code);
}

std::string number(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

// The CPU's crypto features (what OpenSSL can use unless masked).
std::string cryptoFeatures()
{
    using sb::isa::Feature;
    const sb::isa::CpuFeatures f = sb::isa::detect();
    std::string out;
    const auto add = [&](Feature feature, const char *name) {
        if (f.has(feature))
            out += (out.empty() ? "" : ",") + std::string(name);
    };
    add(Feature::Aes, "aes");
    add(Feature::Pclmulqdq, "pclmul");
    add(Feature::Sha, "sha");
    add(Feature::Vaes, "vaes");
    add(Feature::Vpclmulqdq, "vpclmulqdq");
    add(Feature::ArmAes, "aes");
    add(Feature::ArmPmull, "pmull");
    add(Feature::ArmSha2, "sha2");
    return out;
}

bool masked()
{
    return std::getenv("OPENSSL_ia32cap") != nullptr || std::getenv("OPENSSL_armcap") != nullptr;
}

std::string readyLine()
{
    return std::string(R"({"ev":"ready","openssl":")") + OpenSSL_version(OPENSSL_VERSION_STRING) + R"(","features":")"
           + cryptoFeatures() + R"(","masked":)" + (masked() ? "true" : "false") + "}";
}

// SynBench holds our stdin open while it lives: EOF means it is gone. Raw
// reads, not stdio: a thread blocked in fgetc() holds stdin's lock, and
// exit() then waits for it forever (glibc flushes every stream).
void exitOnStdinEof()
{
    std::thread([] {
        char buf[256];
#if defined(_WIN32)
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD n = 0;
        while (in != INVALID_HANDLE_VALUE && in != nullptr && ReadFile(in, buf, DWORD(sizeof buf), &n, nullptr) && n > 0) {
        }
#else
        while (::read(0, buf, sizeof buf) > 0) {
        }
#endif
        std::_Exit(3);
    }).detach();
}

// One unit of every seed on this thread, against the golden table; the XOR
// of their checksums.
std::uint64_t checksumPass(Alg alg)
{
    std::uint64_t x = 0;
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t sum = crypto::computeUnit(alg, s);
        if (sum != crypto::goldenUnit(alg, s))
            failWith(std::string(crypto::algName(alg)) + " produced a wrong result (seed " + std::to_string(s)
                     + "): possible miscompile or hardware fault");
        x ^= sum;
    }
    return x;
}

struct Args {
    std::optional<Alg> alg;
    int threads = 0;
    long long warmupMs = -1, repMs = -1;
    int reps = 0;
    std::uint64_t seed = 0;
};

// A whole decimal number in [lo, hi], else nothing.
std::optional<long long> parseNumber(const char *s, long long lo, long long hi)
{
    if (!*s)
        return std::nullopt;
    long long v = 0;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9' || v > hi)
            return std::nullopt;
        v = v * 10 + (*p - '0');
    }
    if (v < lo || v > hi)
        return std::nullopt;
    return v;
}

[[noreturn]] void usage(const std::string &problem)
{
    failWith(problem
                 + " (usage: synbench-cpucrypto --alg aes-256-gcm|sha-256 --threads N --warmup-ms W --rep-ms L "
                   "--reps R [--seed S] | --self-test)",
             2);
}

Args parseArgs(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (i + 1 >= argc)
            usage("missing value for " + opt);
        const char *value = argv[++i];
        if (opt == "--alg") {
            a.alg = crypto::algFromArg(value);
            if (!a.alg)
                usage(std::string("unknown --alg '") + value + "'");
        } else if (opt == "--threads") {
            const auto v = parseNumber(value, 1, 4096);
            if (!v)
                usage("bad --threads");
            a.threads = int(*v);
        } else if (opt == "--warmup-ms" || opt == "--rep-ms") {
            const auto v = parseNumber(value, 1, 600000);
            if (!v)
                usage("bad " + opt);
            (opt == "--warmup-ms" ? a.warmupMs : a.repMs) = *v;
        } else if (opt == "--reps") {
            const auto v = parseNumber(value, 1, 100);
            if (!v)
                usage("bad --reps");
            a.reps = int(*v);
        } else if (opt == "--seed") {
            const auto v = parseNumber(value, 0, std::int64_t(kSeeds) - 1);
            if (!v)
                usage("bad --seed");
            a.seed = std::uint64_t(*v);
        } else {
            usage("unknown option '" + opt + "'");
        }
    }
    if (!a.alg || !a.threads || a.warmupMs < 0 || a.repMs < 0 || !a.reps)
        usage("--alg, --threads, --warmup-ms, --rep-ms and --reps are required");
    return a;
}

int selfTest()
{
    if (const auto failure = crypto::knownAnswerFailure())
        failWith(*failure);
    for (const Alg alg : {Alg::AesGcm, Alg::Sha256})
        checksumPass(alg);
    line(std::string(R"({"ev":"self-test","ok":true,"openssl":")") + OpenSSL_version(OPENSSL_VERSION_STRING) + "\"}");
    return 0;
}

#if defined(SYNBENCH_DEV_HOOKS)
// Developer builds only: the golden table (src/tests/cpu/golden/crypto.inc),
// and the environment this process sees (the client's env tests).
int printGolden()
{
    std::printf("// SPDX-License-Identifier: GPL-3.0-or-later\n"
                "// GENERATED by synbench-cpucrypto --print-golden (a developer build); do not edit.\n"
                "// A change here changes what the CPU test accepts: bump its testVersion.\n"
                "// The encryption worker (section 5.5): the checksum of one unit per seed\n"
                "// (64 records of 16 KiB: one GCM message's tag, or the 64 SHA-256 digests, folded).\n");
    for (const Alg alg : {Alg::AesGcm, Alg::Sha256}) {
        std::printf("inline constexpr std::array<std::uint64_t, %d> %s = {\n", int(kSeeds),
                    alg == Alg::AesGcm ? "kAesGcm" : "kSha256");
        for (std::uint64_t s = 0; s < kSeeds; ++s)
            std::printf("%s0x%016llXull,%s", s % 4 == 0 ? "    " : " ", (unsigned long long)crypto::computeUnit(alg, s),
                        s % 4 == 3 ? "\n" : "");
        std::printf("};\n");
    }
    return 0;
}

int printEnvironment()
{
    for (char **e = SB_ENVIRON; e && *e; ++e)
        std::printf("%s\n", *e);
    return 0;
}
#endif

} // namespace

int main(int argc, char **argv)
{
    hardenProcess();
    if (OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) != 1)
        failWith("OPENSSL_init_crypto failed");

    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
        return selfTest();
#if defined(SYNBENCH_DEV_HOOKS)
    if (argc == 2 && std::strcmp(argv[1], "--print-golden") == 0)
        return printGolden();
    if (argc == 2 && std::strcmp(argv[1], "--print-env") == 0)
        return printEnvironment();
#endif
    const Args args = parseArgs(argc, argv);
    exitOnStdinEof();
    try {
        if (const auto failure = crypto::knownAnswerFailure())
            failWith(*failure);
        const std::uint64_t checksum = checksumPass(*args.alg);
        line(readyLine());

        const ScopedProcessPowerThrottlingOptOut throttling;
        const crypto::CryptoKernel kernel(*args.alg, args.seed);
        WorkerPool pool({args.threads, {}});
        const WindowSpec spec{std::chrono::milliseconds(args.warmupMs), std::chrono::milliseconds(args.repMs), args.reps};
        const Measurement m = pool.measure(
            kernel, spec, [] { return false; },
            [](int window) { line(R"({"ev":"window","i":)" + std::to_string(window) + "}"); });
        if (!m.ok())
            failWith(m.error.empty() ? "the measurement failed" : m.error);
        for (std::size_t i = 0; i < m.reps.size(); ++i)
            line(R"({"ev":"rep","i":)" + std::to_string(i + 1) + R"(,"mbps":)" + number(m.reps[i].rate / 1e6) + "}");
        char hex[32];
        std::snprintf(hex, sizeof hex, "0x%016llX", (unsigned long long)checksum);
        line(R"({"ev":"result","median_mbps":)" + number(m.median / 1e6) + R"(,"spread":)" + number(m.spread)
             + R"(,"checksum":")" + hex + "\"}");
    } catch (const std::exception &e) {
        failWith(e.what());
    }
    return 0;
}

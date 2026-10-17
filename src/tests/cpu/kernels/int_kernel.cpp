// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/int_kernel.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"
#include "tests/cpu/kernels/prng.h"

#include <array>

SB_KERNEL_TU

namespace sb::cpu {

namespace {

constexpr std::uint64_t rotl(std::uint64_t x, unsigned n)
{
    return (x << n) | (x >> ((64 - n) & 63));
}

// One ALU step: shift-xor, rotate, multiply, add (5 operations).
inline std::uint64_t aluStep(std::uint64_t x, unsigned rot, std::uint64_t add)
{
    x ^= x >> 29;
    x = rotl(x, rot);
    return x * 0x9FB21C651E98DF25ull + add;
}

// One division step on chain (n, d): 10 operations, see int_kernel.h.
inline void divStep(std::uint64_t &n, std::uint64_t &d)
{
    const std::uint64_t divisor = (d >> 33) | 1;
    const std::uint64_t dividend = n | (1ull << 63);
    const std::uint64_t q = dividend / divisor;
    const std::uint64_t r = dividend % divisor;
    n = (q << 7) ^ r ^ d;
    d = d * 0xD1342543DE82EF95ull + q;
}

struct Insn {
    std::uint8_t op;
    std::uint8_t a;
    std::uint8_t b;
    std::uint32_t imm;
};
constexpr unsigned kProgramSize = 256;
constexpr unsigned kOpcodes = 12;

// The interpreter's program: the same 256 instructions for every seed and
// every platform (one fixed PRNG stream, one draw per statement).
const std::array<Insn, kProgramSize> &program()
{
    static const std::array<Insn, kProgramSize> p = [] {
        std::array<Insn, kProgramSize> out{};
        Xoshiro256ss rng(0x50524F4752414Dull);  // "PROGRAM"
        for (Insn &in : out) {
            const std::uint64_t op = rng.below(kOpcodes);
            const std::uint64_t a = rng.below(8);
            const std::uint64_t b = rng.below(8);
            const std::uint64_t imm = rng.next() >> 32;
            in = {std::uint8_t(op), std::uint8_t(a), std::uint8_t(b), std::uint32_t(imm)};
        }
        return out;
    }();
    return p;
}

} // namespace

SB_NOINLINE std::uint64_t intUnit(std::uint64_t seed)
{
    opaque(seed);
    // The starting values: one draw per statement (the determinism rule).
    std::uint64_t start[24];
    SplitMix64 init(seed ^ 0x494E544547455221ull);  // "INTEGER!"
    for (std::uint64_t &v : start)
        v = init.next();

    // 1. ALU: eight chains in named locals, so every compiler keeps them in
    // registers.
    std::uint64_t c0 = start[0], c1 = start[1], c2 = start[2], c3 = start[3];
    std::uint64_t c4 = start[4], c5 = start[5], c6 = start[6], c7 = start[7];
    SB_NO_VECTOR
    for (std::uint64_t i = 0; i < kIntAluSteps; ++i) {
        c0 = aluStep(c0, 7, 0x1F);
        c1 = aluStep(c1, 13, 0x3D);
        c2 = aluStep(c2, 19, 0x5B);
        c3 = aluStep(c3, 23, 0x79);
        c4 = aluStep(c4, 31, 0x97);
        c5 = aluStep(c5, 37, 0xB5);
        c6 = aluStep(c6, 43, 0xD3);
        c7 = aluStep(c7, 53, 0xF1);
    }

    // 2. Division.
    std::uint64_t n0 = start[8], n1 = start[9], n2 = start[10], n3 = start[11];
    std::uint64_t d0 = start[12], d1 = start[13], d2 = start[14], d3 = start[15];
    SB_NO_VECTOR
    for (std::uint64_t i = 0; i < kIntDivSteps; ++i) {
        divStep(n0, d0);
        divStep(n1, d1);
        divStep(n2, d2);
        divStep(n3, d3);
    }

    // 3. The interpreter.
    const Insn *prog = program().data();
    opaque(prog);
    std::uint64_t r[8];
    for (int i = 0; i < 8; ++i)
        r[i] = start[16 + i];
    unsigned pc = 0;
    SB_NO_VECTOR
    for (std::uint64_t i = 0; i < kIntDispatches; ++i) {
        const Insn in = prog[pc];
        pc = (pc + 1) & (kProgramSize - 1);
        std::uint64_t &x = r[in.a];
        const std::uint64_t y = r[in.b];
        switch (in.op) {
        case 0: x += y; break;
        case 1: x -= y; break;
        case 2: x ^= y; break;
        case 3: x = (x & y) + in.imm; break;
        case 4: x = (x | y) ^ in.imm; break;
        case 5: x *= (std::uint64_t(in.imm) << 1) | 1; break;
        case 6: x ^= x << (in.imm & 31); break;
        case 7: x ^= x >> ((in.imm & 31) | 1); break;
        case 8: x = rotl(x, unsigned(y & 63)); break;
        case 9: x += in.imm; break;
        case 10:  // skip the next instruction if a data bit is set
            if ((x >> (in.imm & 63)) & 1)
                pc = (pc + 1) & (kProgramSize - 1);
            break;
        default:  // 11: jump ahead by 0..15, from the data
            pc = (pc + unsigned(x & 15)) & (kProgramSize - 1);
            break;
        }
    }

    std::uint64_t sum = 0;
    for (const std::uint64_t v : {c0, c1, c2, c3, c4, c5, c6, c7, n0, n1, n2, n3, d0, d1, d2, d3})
        sum = foldChecksum(sum, v);
    for (const std::uint64_t v : r)
        sum = foldChecksum(sum, v);
    return sum;
}

std::unique_ptr<KernelThread> IntKernel::makeThread(int) const
{
    struct T : KernelThread {
        std::uint64_t unit(std::uint64_t unitIndex) override { return intUnit(unitIndex % kSeeds); }
    };
    return std::make_unique<T>();
}

std::optional<std::uint64_t> IntKernel::golden(std::uint64_t seed) const
{
    return golden::kInt[seed % kSeeds];
}

} // namespace sb::cpu

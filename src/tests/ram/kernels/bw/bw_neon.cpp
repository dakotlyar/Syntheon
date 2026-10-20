// SPDX-License-Identifier: GPL-3.0-or-later
// The NEON bandwidth path (Phase 3 design §5.3, D4): 128-bit loads and plain
// stores, 8 registers (128 bytes) per iteration. ARM64 has no store that
// guarantees "no read-for-ownership" (STNP is a hint), and its cores detect
// streaming writes themselves, so there is one store variant. Baseline
// flags (armv8-a); it follows bw_common.h's ODR rule like the x64 paths:
// only <arm_neon.h> and bw_common.h, only its sb_bw_neon_* entry points.
#include "tests/ram/kernels/bw/bw_common.h"

#include <arm_neon.h>

namespace sb::ram::bw_neon {
namespace {

constexpr std::size_t kRow = 128;  // 8 x 16 bytes

uint64x2_t lanes(std::uint64_t first)
{
    return vcombine_u64(vcreate_u64(first), vcreate_u64(first + 1));
}

} // namespace
} // namespace sb::ram::bw_neon

using namespace sb::ram::bw_neon;

extern "C" std::uint64_t sb_bw_neon_read(const void *p, std::size_t bytes)
{
    const std::uint64_t *at = static_cast<const std::uint64_t *>(p);
    const std::uint64_t *const end = at + bytes / sizeof(std::uint64_t);
    uint64x2_t a0 = vdupq_n_u64(0), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (; at < end; at += kRow / sizeof(std::uint64_t)) {
        a0 = vaddq_u64(a0, vld1q_u64(at));
        a1 = vaddq_u64(a1, vld1q_u64(at + 2));
        a2 = vaddq_u64(a2, vld1q_u64(at + 4));
        a3 = vaddq_u64(a3, vld1q_u64(at + 6));
        a4 = vaddq_u64(a4, vld1q_u64(at + 8));
        a5 = vaddq_u64(a5, vld1q_u64(at + 10));
        a6 = vaddq_u64(a6, vld1q_u64(at + 12));
        a7 = vaddq_u64(a7, vld1q_u64(at + 14));
    }
    const uint64x2_t s = vaddq_u64(vaddq_u64(vaddq_u64(a0, a1), vaddq_u64(a2, a3)),
                                   vaddq_u64(vaddq_u64(a4, a5), vaddq_u64(a6, a7)));
    return vgetq_lane_u64(s, 0) + vgetq_lane_u64(s, 1);
}

extern "C" void sb_bw_neon_write(void *p, std::size_t bytes, std::uint64_t base)
{
    std::uint64_t *at = static_cast<std::uint64_t *>(p);
    std::uint64_t *const end = at + bytes / sizeof(std::uint64_t);
    uint64x2_t v0 = lanes(base), v1 = lanes(base + 2), v2 = lanes(base + 4), v3 = lanes(base + 6);
    uint64x2_t v4 = lanes(base + 8), v5 = lanes(base + 10), v6 = lanes(base + 12), v7 = lanes(base + 14);
    const uint64x2_t step = vdupq_n_u64(16);
    for (; at < end; at += kRow / sizeof(std::uint64_t)) {
        vst1q_u64(at, v0);
        vst1q_u64(at + 2, v1);
        vst1q_u64(at + 4, v2);
        vst1q_u64(at + 6, v3);
        vst1q_u64(at + 8, v4);
        vst1q_u64(at + 10, v5);
        vst1q_u64(at + 12, v6);
        vst1q_u64(at + 14, v7);
        v0 = vaddq_u64(v0, step);
        v1 = vaddq_u64(v1, step);
        v2 = vaddq_u64(v2, step);
        v3 = vaddq_u64(v3, step);
        v4 = vaddq_u64(v4, step);
        v5 = vaddq_u64(v5, step);
        v6 = vaddq_u64(v6, step);
        v7 = vaddq_u64(v7, step);
    }
}

extern "C" void sb_bw_neon_copy(void *dst, const void *src, std::size_t bytes)
{
    std::uint64_t *to = static_cast<std::uint64_t *>(dst);
    const std::uint64_t *from = static_cast<const std::uint64_t *>(src);
    const std::uint64_t *const end = from + bytes / sizeof(std::uint64_t);
    for (; from < end; from += kRow / sizeof(std::uint64_t), to += kRow / sizeof(std::uint64_t)) {
        const uint64x2_t v0 = vld1q_u64(from);
        const uint64x2_t v1 = vld1q_u64(from + 2);
        const uint64x2_t v2 = vld1q_u64(from + 4);
        const uint64x2_t v3 = vld1q_u64(from + 6);
        const uint64x2_t v4 = vld1q_u64(from + 8);
        const uint64x2_t v5 = vld1q_u64(from + 10);
        const uint64x2_t v6 = vld1q_u64(from + 12);
        const uint64x2_t v7 = vld1q_u64(from + 14);
        vst1q_u64(to, v0);
        vst1q_u64(to + 2, v1);
        vst1q_u64(to + 4, v2);
        vst1q_u64(to + 6, v3);
        vst1q_u64(to + 8, v4);
        vst1q_u64(to + 10, v5);
        vst1q_u64(to + 12, v6);
        vst1q_u64(to + 14, v7);
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/best_of.h"

#include <cstdlib>

namespace sb::gpu {

namespace {
constexpr int kSignatureBytes = 16 * 9 * 3;
}

std::optional<Candidate> bestOf(const QVector<Candidate> &inOrder)
{
    std::optional<Candidate> best;
    for (const Candidate &c : inOrder) {
        if (c.value > 0 && (!best || c.value > best->value))
            best = c;
    }
    return best;
}

std::optional<SigDiff> compareSignatures(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != kSignatureBytes || b.size() != kSignatureBytes)
        return std::nullopt;
    SigDiff d;
    long total = 0;
    for (int i = 0; i < kSignatureBytes; ++i) {
        const int diff = std::abs(int(static_cast<unsigned char>(a[i])) - int(static_cast<unsigned char>(b[i])));
        total += diff;
        if (diff > d.max)
            d.max = diff;
    }
    d.mean = double(total) / double(kSignatureBytes);
    return d;
}

bool signaturesMatch(const SigDiff &d) { return d.mean <= 4.0 && d.max <= 32; }

} // namespace sb::gpu

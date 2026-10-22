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

SignatureVerdict judgeSignatures(const QVector<QByteArray> &inRunOrder)
{
    SignatureVerdict v;
    v.diffToReference.resize(inRunOrder.size());
    QVector<int> ran;
    for (int i = 0; i < inRunOrder.size(); ++i) {
        if (!inRunOrder[i].isEmpty())
            ran.append(i);
    }
    if (ran.isEmpty())
        return v;
    const auto matches = [&](int a, int b) {
        const auto d = compareSignatures(inRunOrder[a], inRunOrder[b]);
        return d && signaturesMatch(*d);
    };
    int best = -1, bestCount = -1;
    for (const int i : ran) {
        int count = 0;
        for (const int j : ran)
            count += (i != j && matches(i, j)) ? 1 : 0;
        if (count > bestCount) {  // strictly more: a tie keeps the earlier API
            best = i;
            bestCount = count;
        }
    }
    v.reference = best;
    for (const int i : ran) {
        if (i == best)
            continue;
        const auto d = compareSignatures(inRunOrder[i], inRunOrder[best]);
        if (d)
            v.diffToReference[i] = *d;
        else
            v.diffToReference[i] = SigDiff{255.0, 255};
        if (!d || !signaturesMatch(*d))
            v.outliers.append(i);
    }
    return v;
}

} // namespace sb::gpu

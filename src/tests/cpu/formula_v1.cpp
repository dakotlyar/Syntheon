// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/formula_v1.h"

#include <cmath>

namespace sb::cpu::formula_v1 {

const QVector<GeometricGroup> &groups()
{
    // Provisional anchors (see the header): Knowhere, 2026-10-18.
    static const QVector<GeometricGroup> g = {
        {QStringLiteral("int"), 0.25, {{QStringLiteral("int.rate"), 160000, 1.0}}},
        {QStringLiteral("fp"), 0.25, {{QStringLiteral("fp.rate"), 150000, 0.8}, {QStringLiteral("fp.primes"), 17300, 0.2}}},
        {QStringLiteral("simd"), 0.15, {{QStringLiteral("simd.rate"), 1950000, 1.0}}},
        {QStringLiteral("compress"), 0.10,
         {{QStringLiteral("compress.zlib.c"), 484, 0.125},
          {QStringLiteral("compress.zlib.d"), 8120, 0.125},
          {QStringLiteral("compress.zstd.c"), 4710, 0.125},
          {QStringLiteral("compress.zstd.d"), 16500, 0.125},
          {QStringLiteral("compress.lz4.c"), 10100, 0.125},
          {QStringLiteral("compress.lz4.d"), 62800, 0.125},
          {QStringLiteral("compress.xz.c"), 103, 0.125},
          {QStringLiteral("compress.xz.d"), 2970, 0.125}}},
        {QStringLiteral("crypto"), 0.10,
         {{QStringLiteral("crypto.aes_gcm"), 238000, 0.5}, {QStringLiteral("crypto.sha256"), 39000, 0.5}}},
        {QStringLiteral("sort"), 0.10, {{QStringLiteral("sort.u64"), 445, 0.5}, {QStringLiteral("sort.str"), 238, 0.5}}},
        {QStringLiteral("physics"), 0.05, {{QStringLiteral("physics.nbody"), 3710, 1.0}}},
    };
    return g;
}

ScoreBreakdown score(const TestResult &result)
{
    const GeometricScore s = weightedGeometricScore(result, kScale, groups());
    ScoreBreakdown b;
    b.total = s.total;
    b.subScores = s.groups;
    return b;
}

std::optional<double> singleThreadMark(double singleInt, double singleFp, double singleCompress)
{
    if (!(singleInt > 0) || !(singleFp > 0) || !(singleCompress > 0))
        return std::nullopt;
    const double l = 5.0 / 12.0 * std::log(singleInt / kAnchorSingleInt) + 5.0 / 12.0 * std::log(singleFp / kAnchorSingleFp)
                     + 2.0 / 12.0 * std::log(singleCompress / kAnchorSingleCompress);
    return std::round(kScale * std::exp(l));
}

} // namespace sb::cpu::formula_v1

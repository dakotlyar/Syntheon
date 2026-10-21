// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The best API (or path) per workload, and the image-signature comparison
// (Phase 4a design §5.7, P14, D5, D20). Pure.

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace sb::gpu {

struct Candidate {
    QString source;  // an api key or a native path key
    double value = 0;
};
// The highest value above 0; a tie goes to the earliest candidate.
std::optional<Candidate> bestOf(const QVector<Candidate> &inOrder);

struct SigDiff {
    double mean = 0;  // in 1/255 steps
    int max = 0;
};
// nullopt unless both signatures are 432 bytes (16 x 9 blocks x RGB).
std::optional<SigDiff> compareSignatures(const QByteArray &a, const QByteArray &b);
bool signaturesMatch(const SigDiff &d);  // mean <= 4.0 && max <= 32

} // namespace sb::gpu

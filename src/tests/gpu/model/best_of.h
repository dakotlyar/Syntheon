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

// §5.7, D20: the signatures of one workload in run order (Vulkan, DX12, DX11, OpenGL); an empty
// entry is an API that didn't run and is skipped. The reference is the API that matches the most
// others (ties: the earliest); every other API that doesn't match it is an outlier.
struct SignatureVerdict {
    int reference = -1;          // an index into the input, -1 when nothing ran
    QVector<int> outliers;       // the indices that don't match the reference
    QVector<SigDiff> diffToReference;  // per input entry (zero for the reference and skipped ones; a mismatch of unusable data has mean 255)
};
SignatureVerdict judgeSignatures(const QVector<QByteArray> &inRunOrder);

} // namespace sb::gpu

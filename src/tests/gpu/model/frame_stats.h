// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Frame-time maths of the GPU test (Phase 4a design §9, P7). Pure.

#include <QPointF>
#include <QString>
#include <QVector>

#include <optional>
#include <vector>

namespace sb::gpu {

// frames = n; seconds = the sum of the frame times; fps = n / seconds;
// p1, p50, p99 = nearest-rank percentiles (ms); low1 = 1000 / p99 (the 1% low);
// min = the fastest frame (ms), for the plausibility rules.
struct FrameStats {
    int frames = 0;
    double seconds = 0, fps = 0, p1 = 0, p50 = 0, p99 = 0, low1 = 0, min = 0;
};

FrameStats frameStats(const std::vector<double> &frameMs);  // §9; empty -> all 0
// q in [0,100]; p_q = sorted[ceil(q/100 * n) - 1], index clamped to [0, n-1]; empty -> 0.
double percentileNearestRank(const std::vector<double> &sortedAscending, double q);
double median(std::vector<double> v);                // even n: mean of the two middle values; empty -> 0
double geometricMean(const std::vector<double> &v);  // 0 if empty or any value <= 0
// 100 ms buckets by each frame's end time; value = the bucket's mean frame
// time; empty buckets skipped; x = the bucket centre in seconds.
QVector<QPointF> bucketSeries(const std::vector<double> &frameMs, double bucketMs = 100.0);

// §9's plausibility rules for one measured workload, checked in this order:
//   FPS > 100,000 or a frame time < 0.005 ms -> error "implausible frame rate";
//   n < 20                                   -> error "too few frames to measure (n)";
//   median GPU time > 1.5 x p50              -> dropGpu (a warning; `gpu` is not stored).
// minFrameMs nullopt = not known (that half of the first rule is skipped).
inline constexpr int kMinFrames = 20;
inline constexpr double kMaxFps = 100000.0, kMinFrameMs = 0.005, kMaxGpuToP50 = 1.5;
inline const QString kGpuTimeWarning = QStringLiteral("GPU timestamps look wrong; GPU time not shown");
struct Plausibility {
    QString error;  // empty: plausible
    bool dropGpu = false;
};
// minFrames: the n floor, kMinFrames except under the GPU test's dev hook for the CI smoke (SYNBENCH_GPU_MIN_FRAMES).
Plausibility plausibility(int frames, double fps, std::optional<double> minFrameMs, double p50Ms, std::optional<double> gpuMs,
                          int minFrames = kMinFrames);

} // namespace sb::gpu

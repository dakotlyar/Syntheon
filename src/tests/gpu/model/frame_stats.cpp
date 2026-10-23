// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/frame_stats.h"

#include <algorithm>
#include <cmath>

namespace sb::gpu {

double percentileNearestRank(const std::vector<double> &sorted, double q)
{
    if (sorted.empty())
        return 0;
    const double n = double(sorted.size());
    const auto rank = std::int64_t(std::ceil(q * n / 100.0));
    const auto index = std::clamp<std::int64_t>(rank - 1, 0, std::int64_t(sorted.size()) - 1);
    return sorted[std::size_t(index)];
}

FrameStats frameStats(const std::vector<double> &frameMs)
{
    FrameStats s;
    if (frameMs.empty())
        return s;
    double sumMs = 0;
    for (const double v : frameMs)
        sumMs += v;
    std::vector<double> sorted = frameMs;
    std::sort(sorted.begin(), sorted.end());
    s.frames = int(frameMs.size());
    s.seconds = sumMs / 1000.0;
    s.fps = sumMs > 0 ? double(frameMs.size()) / s.seconds : 0;
    s.min = sorted.front();
    s.p1 = percentileNearestRank(sorted, 1);
    s.p50 = percentileNearestRank(sorted, 50);
    s.p99 = percentileNearestRank(sorted, 99);
    s.low1 = s.p99 > 0 ? 1000.0 / s.p99 : 0;
    return s;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double geometricMean(const std::vector<double> &v)
{
    if (v.empty())
        return 0;
    double sum = 0;
    for (const double x : v) {
        if (!(x > 0))
            return 0;
        sum += std::log(x);
    }
    return std::exp(sum / double(v.size()));
}

QVector<QPointF> bucketSeries(const std::vector<double> &frameMs, double bucketMs)
{
    QVector<QPointF> out;
    if (frameMs.empty() || !(bucketMs > 0))
        return out;
    double end = 0, bucketSum = 0;
    std::int64_t bucket = 0, count = 0;
    const auto flush = [&] {
        if (count > 0)
            out.append(QPointF((double(bucket) + 0.5) * bucketMs / 1000.0, bucketSum / double(count)));
    };
    for (const double ms : frameMs) {
        end += ms;
        const auto b = std::int64_t(std::floor(end / bucketMs));
        if (b != bucket) {
            flush();
            bucket = b;
            bucketSum = 0;
            count = 0;
        }
        bucketSum += ms;
        ++count;
    }
    flush();
    return out;
}

Plausibility plausibility(int frames, double fps, std::optional<double> minFrameMs, double p50Ms, std::optional<double> gpuMs, int minFrames)
{
    Plausibility p;
    if (fps > kMaxFps || (minFrameMs && *minFrameMs < kMinFrameMs))
        p.error = QStringLiteral("implausible frame rate");
    else if (frames < minFrames)
        p.error = QStringLiteral("too few frames to measure (%1)").arg(frames);
    p.dropGpu = gpuMs && *gpuMs > kMaxGpuToP50 * p50Ms;
    return p;
}

} // namespace sb::gpu

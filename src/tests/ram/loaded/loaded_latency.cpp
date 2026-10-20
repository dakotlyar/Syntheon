// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ram/loaded/loaded_latency.h"

#include <chrono>

namespace sb::ram {

LoadedResult measureLoadedLatency(cpu::WorkerPool &chasePool, const ChaseKernel &chase, LoadGenerator &load,
                                  const cpu::WindowSpec &spec, const std::function<bool()> &cancelled,
                                  const std::function<void(int level, int window)> &progress)
{
    using Clock = std::chrono::steady_clock;
    struct Mark {
        Clock::time_point at;
        std::uint64_t bytes;
    };
    LoadedResult result;
    result.nsPerSpin = load.calibrate();
    int level = 0;
    for (const double delay : kLoadDelaysNs) {
        load.run(delay);
        // marks[w]: the load at window w's start (0 = the warm-up), then one
        // more at the end.
        std::vector<Mark> marks;
        const cpu::Measurement m = chasePool.measure(chase, spec, cancelled, [&](int window) {
            marks.push_back({Clock::now(), load.bytesRead()});
            if (progress)
                progress(level, window);
        });
        marks.push_back({Clock::now(), load.bytesRead()});
        load.park();
        if (!m.ok()) {
            result.outcome = m.outcome;
            result.error = m.error;
            return result;
        }
        LoadedLevel l;
        l.delayNs = delay;
        l.latencyNs = latencyNs(m.median);
        l.spread = m.spread;
        // Repetition r (1..reps) runs from marks[r] to marks[r + 1].
        for (std::size_t r = 1; r + 1 < marks.size(); ++r) {
            const double seconds = std::chrono::duration<double>(marks[r + 1].at - marks[r].at).count();
            l.repLoadGBs.push_back(seconds > 0 ? double(marks[r + 1].bytes - marks[r].bytes) / seconds / 1e9 : 0.0);
        }
        l.loadGBs = cpu::medianOf(l.repLoadGBs);
        result.levels.push_back(std::move(l));
        ++level;
    }
    return result;
}

} // namespace sb::ram

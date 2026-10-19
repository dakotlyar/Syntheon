// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One logical CPU's data caches, as the OS reports them (Phase 3 design P6,
// §5.2): the RAM test measures each cache level at a size inside that
// core's own caches, and keeps the single-thread core on one last-level
// cache. Plain C++20, no Qt.
//
//   Linux    /sys/devices/system/cpu/cpuN/cache/index*/{level,type,size,
//            coherency_line_size,shared_cpu_list}
//   Windows  GetLogicalProcessorInformationEx(RelationCache): the records
//            whose GroupMask (or, on Windows 11, GroupMasks) holds the CPU
//
// Instruction (and trace) caches are dropped: the chase and the bandwidth
// kernels only load and store data. Levels above 3 are kept (annotations
// only). The parsers are pure functions over a sysfs tree / a byte buffer,
// so the tests feed them fixtures and synthetic records.

#include "isa/core_ranking.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sb::isa {

struct CacheLevel {
    enum class Type { Data, Unified };
    int level = 0;
    Type type = Type::Unified;
    std::uint64_t sizeBytes = 0;
    int lineSize = 0;  // bytes; 0: not reported
    // The logical CPUs that share this cache, as IdleSample keys (Linux: CPU
    // numbers; Windows: windowsKey(group, index)), ascending. The CPU itself
    // is always in it.
    std::vector<int> sharedWith;
};

struct CacheTopology {
    // Ascending by level; at most one per level (a level reported both as
    // Data and Unified keeps the Data one).
    std::vector<CacheLevel> levels;

    bool empty() const { return levels.empty(); }
    // The cache of level n, or null.
    const CacheLevel *level(int n) const;
    // The highest level (the last-level cache), or null.
    const CacheLevel *lastLevel() const;
    // The pointer chase's line size (§5.1): the L1's coherency line size,
    // at least 64; 64 when nothing reports one.
    int chaseLineSize() const;
};

// Linux: the caches of `cpu` under <sysfsRoot>/devices/system/cpu (normally
// sysfsRoot = "/sys"). Empty when the CPU has no cache directory (some ARM
// boards, VMs: the RAM test then finds the levels on its curve).
CacheTopology linuxCacheTopology(const std::string &sysfsRoot, int cpu);

// Windows: the caches of logical processor (group, logicalIndex) in a
// GetLogicalProcessorInformationEx(RelationCache) buffer. The layout is
// read by offset (x64/ARM64: KAFFINITY is 64 bits), so the parser builds
// and is tested on every platform; on Windows the offsets are checked
// against <windows.h> at compile time. A GroupCount of 0 (before Windows
// 11) means the single GroupMask. Records that don't fit the buffer end
// the parse.
CacheTopology parseWindowsCacheRecords(const unsigned char *data, std::size_t length, int group, int logicalIndex);

// This machine: the platform's topology for the chosen core (empty if the
// core is unranked or the OS reports nothing).
CacheTopology cacheTopologyFor(const CoreChoice &core);

// §4.3 (Phase 3 D6): the single-thread tie set, kept to the candidates that
// share the first one's last-level cache, so a re-pick never moves the RAM
// test to another CCD's L3. `first` is the first candidate's topology; when
// it reports no caches, the candidates are returned as they are.
std::vector<CoreCandidate> sameLastLevelCache(const std::vector<CoreCandidate> &candidates, const CacheTopology &first);

// This machine: topCandidatesForSingleThread(), filtered by
// sameLastLevelCache() on the first candidate's live topology.
std::vector<CoreCandidate> topCandidatesOnOneLastLevelCache();

} // namespace sb::isa

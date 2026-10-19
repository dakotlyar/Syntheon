// SPDX-License-Identifier: GPL-3.0-or-later
#include "isa/cache_topology.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace sb::isa {

namespace {

// The first line of a sysfs file, without its newline.
std::string readLine(const std::filesystem::path &file)
{
    std::ifstream in(file);
    std::string line;
    if (in)
        std::getline(in, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

// A non-negative decimal number with an optional K/M/G suffix (sysfs cache
// sizes are "48K", "1024K", "32768K"); 0 if it doesn't parse.
std::uint64_t parseSize(const std::string &text)
{
    errno = 0;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || errno != 0)
        return 0;
    switch (*end) {
    case 'K':
        return std::uint64_t(v) << 10;
    case 'M':
        return std::uint64_t(v) << 20;
    case 'G':
        return std::uint64_t(v) << 30;
    case '\0':
        return std::uint64_t(v);
    default:
        return 0;
    }
}

// Adds `c` to `levels` keyed by level: one entry per level, Data over
// Unified (an L1 is reported as Data + Instruction, the levels above as
// Unified; a level with both would be unusual, and the data side is what
// the kernels see).
void addLevel(std::map<int, CacheLevel> &levels, CacheLevel c)
{
    std::sort(c.sharedWith.begin(), c.sharedWith.end());
    c.sharedWith.erase(std::unique(c.sharedWith.begin(), c.sharedWith.end()), c.sharedWith.end());
    const auto it = levels.find(c.level);
    if (it == levels.end())
        levels.emplace(c.level, std::move(c));
    else if (it->second.type == CacheLevel::Type::Unified && c.type == CacheLevel::Type::Data)
        it->second = std::move(c);
}

CacheTopology fromMap(std::map<int, CacheLevel> levels)
{
    CacheTopology t;
    for (auto &[level, c] : levels)
        t.levels.push_back(std::move(c));
    return t;
}

// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX with a CACHE_RELATIONSHIP, by
// offset from the record's start (64-bit KAFFINITY).
constexpr std::size_t kRelationship = 0;      // DWORD
constexpr std::size_t kSize = 4;              // DWORD
constexpr std::size_t kLevel = 8;             // BYTE
constexpr std::size_t kLineSize = 10;         // WORD
constexpr std::size_t kCacheSize = 12;        // DWORD
constexpr std::size_t kType = 16;             // PROCESSOR_CACHE_TYPE (4 bytes)
constexpr std::size_t kGroupCount = 38;       // WORD (Windows 11; 0 before)
constexpr std::size_t kGroupMasks = 40;       // GROUP_AFFINITY[GroupCount]
constexpr std::size_t kGroupAffinitySize = 16;  // KAFFINITY Mask; WORD Group; WORD Reserved[3]
constexpr std::uint32_t kRelationCache = 2;
constexpr std::uint32_t kCacheUnified = 0, kCacheData = 2;

// (GroupCount isn't checked: SDKs before Windows 11's have Reserved[20]
// there instead, the same bytes.)
#if defined(_WIN32)
static_assert(sizeof(KAFFINITY) == 8, "x64 and ARM64 only");
static_assert(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Relationship) == kRelationship);
static_assert(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Size) == kSize);
static_assert(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) + offsetof(CACHE_RELATIONSHIP, Level) == kLevel);
static_assert(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) + offsetof(CACHE_RELATIONSHIP, LineSize) == kLineSize);
static_assert(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) + offsetof(CACHE_RELATIONSHIP, CacheSize) == kCacheSize);
static_assert(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) + offsetof(CACHE_RELATIONSHIP, Type) == kType);
static_assert(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) + offsetof(CACHE_RELATIONSHIP, GroupMask) == kGroupMasks);
static_assert(sizeof(GROUP_AFFINITY) == kGroupAffinitySize);
static_assert(offsetof(GROUP_AFFINITY, Group) == 8);
static_assert(RelationCache == kRelationCache && CacheUnified == kCacheUnified && CacheData == kCacheData);
#endif

template <typename T>
T readLe(const unsigned char *p)
{
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v = T(v | (T(p[i]) << (8 * i)));
    return v;
}

} // namespace

const CacheLevel *CacheTopology::level(int n) const
{
    for (const CacheLevel &c : levels) {
        if (c.level == n)
            return &c;
    }
    return nullptr;
}

const CacheLevel *CacheTopology::lastLevel() const
{
    return levels.empty() ? nullptr : &levels.back();
}

int CacheTopology::chaseLineSize() const
{
    const CacheLevel *l1 = level(1);
    return std::max(64, l1 ? l1->lineSize : 0);
}

CacheTopology linuxCacheTopology(const std::string &sysfsRoot, int cpu)
{
    namespace fs = std::filesystem;
    const fs::path base = fs::path(sysfsRoot) / "devices" / "system" / "cpu" / ("cpu" + std::to_string(cpu)) / "cache";
    std::map<int, CacheLevel> levels;
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(base, ec)) {
        const std::string name = e.path().filename().string();
        if (name.compare(0, 5, "index") != 0)
            continue;
        const std::string type = readLine(e.path() / "type");
        CacheLevel c;
        if (type == "Data")
            c.type = CacheLevel::Type::Data;
        else if (type == "Unified")
            c.type = CacheLevel::Type::Unified;
        else
            continue;  // Instruction
        c.level = std::atoi(readLine(e.path() / "level").c_str());
        c.sizeBytes = parseSize(readLine(e.path() / "size"));
        if (c.level <= 0 || c.sizeBytes == 0)
            continue;
        c.lineSize = std::max(0, std::atoi(readLine(e.path() / "coherency_line_size").c_str()));
        const std::set<int> shared = parseCpuList(readLine(e.path() / "shared_cpu_list"));
        c.sharedWith.assign(shared.begin(), shared.end());
        c.sharedWith.push_back(cpu);
        addLevel(levels, std::move(c));
    }
    return fromMap(std::move(levels));
}

CacheTopology parseWindowsCacheRecords(const unsigned char *data, std::size_t length, int group, int logicalIndex)
{
    std::map<int, CacheLevel> levels;
    for (std::size_t offset = 0; offset + kGroupMasks <= length;) {
        const unsigned char *r = data + offset;
        const std::uint32_t size = readLe<std::uint32_t>(r + kSize);
        if (size < kGroupMasks || offset + size > length)
            break;
        offset += size;
        if (readLe<std::uint32_t>(r + kRelationship) != kRelationCache)
            continue;
        const std::uint32_t type = readLe<std::uint32_t>(r + kType);
        if (type != kCacheData && type != kCacheUnified)
            continue;
        const std::uint16_t groupCount = std::max<std::uint16_t>(1, readLe<std::uint16_t>(r + kGroupCount));
        if (kGroupMasks + std::size_t(groupCount) * kGroupAffinitySize > size)
            continue;
        CacheLevel c;
        c.level = r[kLevel];
        c.type = type == kCacheData ? CacheLevel::Type::Data : CacheLevel::Type::Unified;
        c.lineSize = readLe<std::uint16_t>(r + kLineSize);
        c.sizeBytes = readLe<std::uint32_t>(r + kCacheSize);
        bool mine = false;
        for (std::uint16_t g = 0; g < groupCount; ++g) {
            const unsigned char *ga = r + kGroupMasks + std::size_t(g) * kGroupAffinitySize;
            const std::uint64_t mask = readLe<std::uint64_t>(ga);
            const int maskGroup = readLe<std::uint16_t>(ga + 8);
            for (int bit = 0; bit < 64; ++bit) {
                if (!((mask >> bit) & 1u))
                    continue;
                c.sharedWith.push_back(windowsKey(maskGroup, bit));
                mine = mine || (maskGroup == group && bit == logicalIndex);
            }
        }
        if (mine && c.level > 0 && c.sizeBytes > 0)
            addLevel(levels, std::move(c));
    }
    return fromMap(std::move(levels));
}

CacheTopology cacheTopologyFor(const CoreChoice &core)
{
    if (!core.ranked || core.cpu < 0)
        return {};
#if defined(__linux__)
    return linuxCacheTopology("/sys", core.cpu);
#elif defined(_WIN32)
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
    if (length == 0)
        return {};
    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationCache,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                                          &length))
        return {};
    return parseWindowsCacheRecords(buffer.data(), length, core.group, core.cpu);
#else
    return {};
#endif
}

std::vector<CoreCandidate> sameLastLevelCache(const std::vector<CoreCandidate> &candidates, const CacheTopology &first)
{
    const CacheLevel *llc = first.lastLevel();
    if (!llc)
        return candidates;
    std::vector<CoreCandidate> out;
    for (const CoreCandidate &c : candidates) {
        const int key = windowsKey(c.core.group, c.core.cpu);  // Linux: group 0, so the CPU number
        if (std::binary_search(llc->sharedWith.begin(), llc->sharedWith.end(), key))
            out.push_back(c);
    }
    return out;
}

std::vector<CoreCandidate> topCandidatesOnOneLastLevelCache()
{
    const std::vector<CoreCandidate> top = topCandidatesForSingleThread();
    if (top.empty())
        return top;
    return sameLastLevelCache(top, cacheTopologyFor(top.front().core));
}

} // namespace sb::isa

// SPDX-License-Identifier: GPL-3.0-or-later
// HugeBuffer's platform-independent parts: the parsers, the alignment and
// the wording (Phase 3 design §6.1).
#include "tests/ram/memory/huge_buffer.h"

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace sb::ram {

HugeBuffer::~HugeBuffer()
{
    release();
}

HugeBuffer::HugeBuffer(HugeBuffer &&other) noexcept
{
    *this = std::move(other);
}

HugeBuffer &HugeBuffer::operator=(HugeBuffer &&other) noexcept
{
    if (this != &other) {
        release();
        m_data = std::exchange(other.m_data, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_hugePageSize = std::exchange(other.m_hugePageSize, 0);
        m_basePageSize = std::exchange(other.m_basePageSize, 0);
        m_length = std::exchange(other.m_length, 0);
        m_error = std::move(other.m_error);
        m_mapStart = std::exchange(other.m_mapStart, nullptr);
        m_mapLength = std::exchange(other.m_mapLength, 0);
        m_thpEnabled = std::move(other.m_thpEnabled);
        m_largePages = std::exchange(other.m_largePages, false);
        m_smallReason = std::move(other.m_smallReason);
        m_notes = std::move(other.m_notes);
    }
    return *this;
}

std::optional<SmapsCoverage> parseSmapsCoverage(const std::string &smaps, std::uintptr_t begin, std::uintptr_t end)
{
    std::istringstream in(smaps);
    std::string line;
    bool inside = false, any = false;
    SmapsCoverage c;
    while (std::getline(in, line)) {
        // A mapping's header: "7f12a0000000-7f12e0000000 rw-p 00000000 00:00 0 ..."
        const std::size_t dash = line.find('-');
        const std::size_t space = line.find(' ');
        if (dash != std::string::npos && space != std::string::npos && dash < space && dash > 0) {
            char *e1 = nullptr, *e2 = nullptr;
            const unsigned long long lo = std::strtoull(line.c_str(), &e1, 16);
            const unsigned long long hi = std::strtoull(line.c_str() + dash + 1, &e2, 16);
            if (e1 == line.c_str() + dash && e2 == line.c_str() + space) {
                inside = lo < end && hi > begin;
                any = any || inside;
                continue;
            }
        }
        if (!inside)
            continue;
        // "Rss:              1048576 kB"
        const auto field = [&](const char *key) -> std::optional<std::uint64_t> {
            const std::size_t n = std::char_traits<char>::length(key);
            if (line.compare(0, n, key) != 0)
                return std::nullopt;
            return std::uint64_t(std::strtoull(line.c_str() + n, nullptr, 10)) * 1024;
        };
        if (const auto v = field("Rss:"))
            c.rss += *v;
        else if (const auto h = field("AnonHugePages:"))
            c.anonHuge += *h;
    }
    if (!any)
        return std::nullopt;
    return c;
}

std::string parseThpEnabled(const std::string &text)
{
    const std::size_t open = text.find('[');
    const std::size_t close = text.find(']', open);
    if (open == std::string::npos || close == std::string::npos)
        return {};
    return text.substr(open + 1, close - open - 1);
}

std::optional<std::size_t> parseHugePageSize(const std::string &text)
{
    errno = 0;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || errno != 0 || v == 0 || (v & (v - 1)) != 0)
        return std::nullopt;  // a page size is a power of two
    return std::size_t(v);
}

AlignedRange alignedRange(std::uintptr_t base, std::size_t mappedLength, std::size_t alignment, std::size_t size,
                          std::size_t guard)
{
    AlignedRange r;
    r.start = (base + guard + alignment - 1) & ~std::uintptr_t(alignment - 1);
    r.headTrim = std::size_t(r.start - guard - base);
    r.tailTrim = mappedLength - r.headTrim - guard - size - guard;
    return r;
}

std::string formatPageSize(std::size_t bytes)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB"};
    int u = 0;
    while (u < 3 && bytes >= 1024 && bytes % 1024 == 0) {
        bytes /= 1024;
        ++u;
    }
    return std::to_string(bytes) + " " + units[u];
}

PageReport linuxPageReport(std::optional<double> measured, std::size_t hugePageSize, std::size_t basePageSize,
                           const std::string &thpEnabled)
{
    PageReport r;
    const std::string huge = formatPageSize(hugePageSize), base = formatPageSize(basePageSize);
    if (!measured) {
        r.mode = PageReport::Mode::Unknown;
        r.text = huge + " pages requested (coverage unknown: smaps not readable)";
        return r;
    }
    const double percent = *measured;
    r.hugePercent = percent;
    if (percent >= 90) {
        r.mode = PageReport::Mode::Huge;
        r.text = huge + " transparent huge pages";
    } else if (percent >= 10) {
        r.mode = PageReport::Mode::Mixed;
        r.text = "mixed: " + std::to_string(int(percent))  // truncated: 89.6% is not "90%"
                  + "% " + huge + " pages";
    } else {
        r.mode = PageReport::Mode::Small;
        r.text = base + (thpEnabled.empty() || thpEnabled == "never" ? " pages (THP is off)"
                                                                     : " pages (THP: no huge pages free)");
    }
    return r;
}

} // namespace sb::ram

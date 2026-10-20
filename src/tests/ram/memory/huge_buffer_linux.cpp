// SPDX-License-Identifier: GPL-3.0-or-later
// HugeBuffer on Linux (Phase 3 design §6.1): transparent huge pages through
// madvise, never a system setting.
#include "tests/ram/memory/huge_buffer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25  // Linux 6.1; older headers lack it
#endif

namespace sb::ram {

namespace {

constexpr std::size_t kTouchChunk = std::size_t(64) << 20;  // first touch, between cancel checks
// MADV_COLLAPSE copies into new huge pages: 16 MiB takes a few ms, so a
// Stop between chunks is honoured well within 250 ms.
constexpr std::size_t kCollapseChunk = std::size_t(16) << 20;
constexpr std::size_t kDefaultHugePage = std::size_t(2) << 20;
constexpr double kCollapseBelow = 90;

std::string readFile(const char *path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::size_t roundUp(std::size_t n, std::size_t to)
{
    return (n + to - 1) / to * to;
}

} // namespace

PageSupport pageSupport()
{
    PageSupport s;
    const std::string mode = parseThpEnabled(readFile("/sys/kernel/mm/transparent_hugepage/enabled"));
    const std::size_t huge = parseHugePageSize(readFile("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size"))
                                 .value_or(kDefaultHugePage);
    const std::string base = formatPageSize(std::size_t(std::max(4096L, sysconf(_SC_PAGESIZE))));
    if (mode == "always" || mode == "madvise") {
        s.huge = true;
        s.text = formatPageSize(huge) + " transparent huge pages available";
    } else {
        s.text = base + (mode.empty() ? " pages (no transparent huge pages)" : " pages (THP is off)");
    }
    return s;
}

HugeBuffer HugeBuffer::allocate(std::size_t bytes)
{
    HugeBuffer b;
    const std::size_t page = std::size_t(std::max(4096L, sysconf(_SC_PAGESIZE)));
    b.m_basePageSize = page;
    // Read only, to word the result: THP's mode and size (2 MiB with 4 KiB
    // base pages, 32 MiB with 16 KiB, 512 MiB with 64 KiB on ARM).
    b.m_thpEnabled = parseThpEnabled(readFile("/sys/kernel/mm/transparent_hugepage/enabled"));
    b.m_hugePageSize = parseHugePageSize(readFile("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size"))
                           .value_or(kDefaultHugePage);
    // Only the start is aligned to the huge page (§6.1): the buffer is the
    // request rounded up to the base page.
    const std::size_t length = roundUp(std::max<std::size_t>(bytes, 1), page);
    // Room to align it, and a guard page on each side.
    const std::size_t mapped = length + b.m_hugePageSize + 2 * page;
    void *base = mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        b.m_error = "could not reserve " + formatPageSize(length) + " of memory: " + std::strerror(errno);
        b.m_basePageSize = 0;
        return b;
    }
    const AlignedRange r = alignedRange(reinterpret_cast<std::uintptr_t>(base), mapped, b.m_hugePageSize, length, page);
    auto *start = reinterpret_cast<std::byte *>(r.start);
    std::byte *lowerGuard = start - page;
    std::byte *upperGuard = start + length;
    if (r.headTrim)
        munmap(base, r.headTrim);
    if (r.tailTrim)
        munmap(upperGuard + page, r.tailTrim);
    mprotect(lowerGuard, page, PROT_NONE);
    mprotect(upperGuard, page, PROT_NONE);
    b.m_mapStart = lowerGuard;
    b.m_mapLength = length + 2 * page;
    b.m_data = start;
    b.m_size = bytes;
    b.m_length = length;
    if (madvise(start, length, MADV_HUGEPAGE) != 0)
        b.m_notes.push_back(std::string("madvise(MADV_HUGEPAGE) failed: ") + std::strerror(errno));
    return b;
}

void HugeBuffer::release()
{
    if (m_mapStart)
        munmap(m_mapStart, m_mapLength);
    m_mapStart = nullptr;
    m_mapLength = 0;
    m_data = nullptr;
    m_size = 0;
    m_length = 0;
    m_basePageSize = 0;
}

bool HugeBuffer::firstTouch(const std::function<bool()> &cancelled)
{
    if (!m_data)
        return false;
    for (std::size_t chunk = 0; chunk < m_length; chunk += kTouchChunk) {
        if (cancelled && cancelled())
            return false;
        const std::size_t end = std::min(m_length, chunk + kTouchChunk);
        for (std::size_t page = chunk; page < end; page += m_basePageSize)
            *reinterpret_cast<volatile std::byte *>(m_data + page) = std::byte{0};
    }
    return true;
}

PageReport HugeBuffer::settle(const std::function<bool()> &cancelled)
{
    if (!m_data) {
        PageReport r;
        r.text = "no buffer";
        return r;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(m_data);
    const auto measure = [&]() -> std::optional<double> {
        const std::optional<SmapsCoverage> c = parseSmapsCoverage(readFile("/proc/self/smaps"), begin, begin + m_length);
        if (!c)
            return std::nullopt;
        return c->percent();
    };
    std::optional<double> percent = measure();
    std::vector<std::string> notes = m_notes;
    if (percent && *percent < kCollapseBelow && m_thpEnabled != "never" && !m_thpEnabled.empty()) {
        // One best-effort collapse (Linux 6.1+): EINVAL on older kernels,
        // EAGAIN when no huge page could be found; only logged.
        int failure = 0;
        bool stopped = false;
        for (std::size_t chunk = 0; chunk < m_length; chunk += kCollapseChunk) {
            if (cancelled && cancelled()) {
                stopped = true;
                break;
            }
            if (madvise(m_data + chunk, std::min(kCollapseChunk, m_length - chunk), MADV_COLLAPSE) != 0 && !failure)
                failure = errno;
        }
        const std::optional<double> after = measure();
        std::string note = "MADV_COLLAPSE at " + std::to_string(int(*percent)) + "% huge pages: ";
        if (stopped)
            note += "stopped";
        else if (failure == EINVAL)
            note += "not supported by this kernel";
        else if (failure)
            note += std::string("partly failed (") + std::strerror(failure) + ")";
        else
            note += "done";
        notes.push_back(note + (after ? ", now " + std::to_string(int(*after)) + "%" : ", coverage now unknown"));
        percent = after;
    }
    PageReport r = linuxPageReport(percent, m_hugePageSize, m_basePageSize, m_thpEnabled);
    r.notes = std::move(notes);
    return r;
}

} // namespace sb::ram

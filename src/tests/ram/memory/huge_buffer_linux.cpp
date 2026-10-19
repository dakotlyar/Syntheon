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

constexpr std::size_t kChunk = std::size_t(64) << 20;  // first touch and MADV_COLLAPSE, between cancel checks
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

HugeBuffer HugeBuffer::allocate(std::size_t bytes)
{
    HugeBuffer b;
    b.m_basePageSize = std::size_t(std::max(4096L, sysconf(_SC_PAGESIZE)));
    // Read only, to word the result: THP's mode and size (2 MiB with 4 KiB
    // base pages, 32 MiB with 16 KiB, 512 MiB with 64 KiB on ARM).
    b.m_thpEnabled = parseThpEnabled(readFile("/sys/kernel/mm/transparent_hugepage/enabled"));
    b.m_hugePageSize = parseHugePageSize(readFile("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size"))
                           .value_or(kDefaultHugePage);
    const std::size_t rounded = roundUp(std::max<std::size_t>(bytes, 1), b.m_hugePageSize);
    // The buffer, room to align it, and one guard page after it.
    const std::size_t length = rounded + b.m_hugePageSize + b.m_basePageSize;
    void *base = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        b.m_error = "could not reserve " + formatPageSize(rounded) + " of memory: " + std::strerror(errno);
        return b;
    }
    const AlignedRange r =
        alignedRange(reinterpret_cast<std::uintptr_t>(base), length - b.m_basePageSize, b.m_hugePageSize, rounded);
    auto *start = reinterpret_cast<std::byte *>(r.start);
    if (r.headTrim)
        munmap(base, r.headTrim);
    // The tail beyond the guard page goes back; the guard stays, PROT_NONE.
    std::byte *guard = start + rounded;
    if (r.tailTrim)
        munmap(guard + b.m_basePageSize, r.tailTrim);
    mprotect(guard, b.m_basePageSize, PROT_NONE);
    b.m_mapStart = start;
    b.m_mapLength = rounded + b.m_basePageSize;
    b.m_data = start;
    b.m_size = bytes;
    if (madvise(start, rounded, MADV_HUGEPAGE) != 0)
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
}

bool HugeBuffer::firstTouch(const std::function<bool()> &cancelled)
{
    const std::size_t length = m_mapLength - m_basePageSize;  // the whole aligned buffer, not the guard
    for (std::size_t chunk = 0; chunk < length; chunk += kChunk) {
        if (cancelled && cancelled())
            return false;
        const std::size_t end = std::min(length, chunk + kChunk);
        for (std::size_t page = chunk; page < end; page += m_basePageSize)
            *reinterpret_cast<volatile std::byte *>(m_data + page) = std::byte{0};
    }
    return true;
}

PageReport HugeBuffer::settle(const std::function<bool()> &cancelled)
{
    const auto begin = reinterpret_cast<std::uintptr_t>(m_data);
    const std::size_t length = m_mapLength - m_basePageSize;
    const auto measure = [&] {
        const std::optional<SmapsCoverage> c = parseSmapsCoverage(readFile("/proc/self/smaps"), begin, begin + length);
        return c ? c->percent() : 0.0;
    };
    double percent = measure();
    std::vector<std::string> notes = m_notes;
    if (percent < kCollapseBelow && m_thpEnabled != "never" && !m_thpEnabled.empty()) {
        // One best-effort collapse (Linux 6.1+): EINVAL on older kernels,
        // EAGAIN when no huge page could be found; only logged.
        int failure = 0;
        bool stopped = false;
        for (std::size_t chunk = 0; chunk < length; chunk += kChunk) {
            if (cancelled && cancelled()) {
                stopped = true;
                break;
            }
            if (madvise(m_data + chunk, std::min(kChunk, length - chunk), MADV_COLLAPSE) != 0 && !failure)
                failure = errno;
        }
        const double after = measure();
        std::string note = "MADV_COLLAPSE at " + std::to_string(int(percent)) + "% huge pages: ";
        if (stopped)
            note += "stopped";
        else if (failure == EINVAL)
            note += "not supported by this kernel";
        else if (failure)
            note += std::string("partly failed (") + std::strerror(failure) + ")";
        else
            note += "done";
        notes.push_back(note + ", now " + std::to_string(int(after)) + "%");
        percent = after;
    }
    PageReport r = linuxPageReport(percent, m_hugePageSize, m_basePageSize, m_thpEnabled);
    r.notes = std::move(notes);
    return r;
}

} // namespace sb::ram

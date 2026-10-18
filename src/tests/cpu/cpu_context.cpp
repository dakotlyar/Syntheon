// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/cpu_context.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <powrprof.h>
#endif

namespace sb::cpu {

std::optional<CpuTimes> parseProcStatTotal(const std::string &procStat)
{
    std::istringstream in(procStat);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 4, "cpu ") != 0)
            continue;
        std::istringstream fields(line.substr(4));
        CpuTimes t;
        unsigned long long v = 0;
        int i = 0;
        while (fields >> v) {
            // user nice system idle iowait irq softirq steal (guest and
            // guest_nice are already inside user and nice).
            if (i < 8) {
                t.total += v;
                if (i != 3 && i != 4)
                    t.busy += v;
            }
            ++i;
        }
        if (i < 4)
            return std::nullopt;
        return t;
    }
    return std::nullopt;
}

double busyPercent(const CpuTimes &before, const CpuTimes &after)
{
    if (after.total <= before.total || after.busy < before.busy)
        return 0;
    return 100.0 * double(after.busy - before.busy) / double(after.total - before.total);
}

std::optional<CpuTimes> systemCpuTimes()
{
#if defined(__linux__)
    std::ifstream f("/proc/stat");
    std::ostringstream text;
    text << f.rdbuf();
    return parseProcStatTotal(text.str());
#elif defined(_WIN32)
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user))
        return std::nullopt;
    const auto u64 = [](const FILETIME &t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    // Kernel time includes idle time.
    CpuTimes t;
    t.total = u64(kernel) + u64(user);
    t.busy = t.total - u64(idle);
    return t;
#else
    return std::nullopt;
#endif
}

std::optional<double> sampleBackgroundLoadPercent(std::chrono::milliseconds duration,
                                                  const std::function<bool()> &cancelled)
{
    const auto before = systemCpuTimes();
    if (!before)
        return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + duration;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(std::chrono::milliseconds(50),
                                                                                  deadline - now));
        if (cancelled && cancelled())
            return std::nullopt;
    }
    const auto after = systemCpuTimes();
    if (!after)
        return std::nullopt;
    return busyPercent(*before, *after);
}

std::string linuxPlatformProfile(const std::string &sysfsRoot)
{
    std::ifstream f(std::filesystem::path(sysfsRoot) / "firmware" / "acpi" / "platform_profile");
    std::string line;
    if (!f || !std::getline(f, line))
        return {};
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

std::string powerProfile()
{
#if defined(__linux__)
    return linuxPlatformProfile("/sys");
#elif defined(_WIN32)
    GUID *scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme)
        return {};
    std::string name;
    DWORD size = 0;
    if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        std::wstring wide(size / sizeof(wchar_t), L'\0');
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR *>(wide.data()), &size)
            == ERROR_SUCCESS) {
            while (!wide.empty() && wide.back() == L'\0')
                wide.pop_back();
            const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
            name.resize(std::size_t(n));
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), name.data(), n, nullptr, nullptr);
        }
    }
    LocalFree(scheme);
    return name;
#else
    return {};
#endif
}

std::optional<std::uint64_t> availableMemoryBytes()
{
#if defined(__linux__)
    std::ifstream f("/proc/meminfo");
    std::string key;
    std::uint64_t kib = 0;
    std::string unit;
    while (f >> key >> kib >> unit) {
        if (key == "MemAvailable:")
            return kib * 1024;
    }
    return std::nullopt;
#elif defined(_WIN32)
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m))
        return std::nullopt;
    return std::uint64_t(m.ullAvailPhys);
#else
    return std::nullopt;
#endif
}

} // namespace sb::cpu

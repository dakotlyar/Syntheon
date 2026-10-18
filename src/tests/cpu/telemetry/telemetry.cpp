// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/telemetry/telemetry.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace sb::cpu::telemetry {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> readLine(const fs::path &file)
{
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line))
        return std::nullopt;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

std::optional<long long> readInteger(const fs::path &file)
{
    const auto line = readLine(file);
    if (!line)
        return std::nullopt;
    errno = 0;
    char *end = nullptr;
    const long long v = std::strtoll(line->c_str(), &end, 10);
    if (end == line->c_str() || errno != 0)
        return std::nullopt;
    return v;
}

// A hwmon device's temperature inputs: index -> label ("" without a label).
std::map<int, std::string> tempInputs(const fs::path &hwmon)
{
    std::map<int, std::string> inputs;
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(hwmon, ec)) {
        const std::string name = e.path().filename().string();
        // temp<N>_input
        if (name.size() > 10 && name.compare(0, 4, "temp") == 0 && name.compare(name.size() - 6, 6, "_input") == 0) {
            const std::string digits = name.substr(4, name.size() - 10);
            if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            const int index = std::atoi(digits.c_str());
            inputs[index] = readLine(hwmon / ("temp" + digits + "_label")).value_or(std::string());
        }
    }
    return inputs;
}

fs::path inputPath(const fs::path &hwmon, int index)
{
    return hwmon / ("temp" + std::to_string(index) + "_input");
}

} // namespace

std::optional<double> linuxClockMHz(const std::string &sysfsRoot, const std::vector<int> &cpus)
{
    const fs::path base = fs::path(sysfsRoot) / "devices" / "system" / "cpu";
    double sum = 0;
    int n = 0;
    for (const int cpu : cpus) {
        const auto khz = readInteger(base / ("cpu" + std::to_string(cpu)) / "cpufreq" / "scaling_cur_freq");
        if (khz && *khz > 0) {
            sum += double(*khz) / 1000.0;
            ++n;
        }
    }
    if (n == 0)
        return std::nullopt;
    return sum / n;
}

std::optional<TempSource> linuxTempSource(const std::string &sysfsRoot)
{
    // Every hwmon by name, in hwmonN order (the class links resolve to the devices).
    std::vector<std::pair<int, fs::path>> devices;
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(fs::path(sysfsRoot) / "class" / "hwmon", ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 5 && name.compare(0, 5, "hwmon") == 0)
            devices.emplace_back(std::atoi(name.c_str() + 5), e.path());
    }
    std::sort(devices.begin(), devices.end());

    // Lower rank = preferred.
    std::optional<TempSource> best;
    int bestRank = 1 << 30;
    for (const auto &[number, hwmon] : devices) {
        const std::string name = readLine(hwmon / "name").value_or(std::string());
        const std::map<int, std::string> inputs = tempInputs(hwmon);
        if (inputs.empty())
            continue;
        const auto withLabel = [&](const std::string &label) -> std::optional<int> {
            for (const auto &[index, l] : inputs) {
                if (l == label)
                    return index;
            }
            return std::nullopt;
        };
        const auto withPrefix = [&](const std::string &prefix) -> std::optional<int> {
            for (const auto &[index, l] : inputs) {
                if (l.compare(0, prefix.size(), prefix) == 0)
                    return index;
            }
            return std::nullopt;
        };
        int rank = 0;
        std::optional<int> index;
        std::string label;
        if (name == "k10temp") {
            rank = 0;
            for (const char *l : {"Tctl", "Tdie"}) {
                if ((index = withLabel(l))) {
                    label = l;
                    break;
                }
            }
            if (!index && (index = withPrefix("Tccd")))
                label = inputs.at(*index);
            if (!index) {
                index = inputs.begin()->first;
                label = inputs.begin()->second;
            }
            label = "k10temp" + (label.empty() ? std::string() : " " + label);
        } else if (name == "coretemp") {
            rank = 1;
            index = withLabel("Package id 0");
            if (!index)
                index = inputs.begin()->first;
            label = "coretemp" + (inputs.at(*index).empty() ? std::string() : " " + inputs.at(*index));
        } else if (name == "cpu_thermal"
                   || (name.size() > 8 && name.compare(name.size() - 8, 8, "-thermal") == 0)) {
            rank = 2;
            index = inputs.begin()->first;
            label = name;
        } else if (name == "acpitz") {
            rank = 3;
            index = inputs.begin()->first;
            label = "ACPI zone";
        } else {
            continue;
        }
        if (rank < bestRank) {
            bestRank = rank;
            best = TempSource{inputPath(hwmon, *index).string(), label};
        }
    }
    return best;
}

std::optional<double> readHwmonCelsius(const std::string &path)
{
    const auto milli = readInteger(path);
    if (!milli)
        return std::nullopt;
    return double(*milli) / 1000.0;
}

std::optional<std::uint64_t> linuxThrottleCount(const std::string &sysfsRoot)
{
    const fs::path base = fs::path(sysfsRoot) / "devices" / "system" / "cpu";
    std::uint64_t sum = 0;
    bool any = false;
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(base, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() <= 3 || name.compare(0, 3, "cpu") != 0
            || !std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        for (const char *f : {"core_throttle_count", "package_throttle_count"}) {
            if (const auto v = readInteger(e.path() / "thermal_throttle" / f); v && *v >= 0) {
                sum += std::uint64_t(*v);
                any = true;
            }
        }
    }
    if (!any)
        return std::nullopt;
    return sum;
}

double pdhClockMHz(double processorPerformancePercent, double processorFrequencyMHz)
{
    return processorPerformancePercent * processorFrequencyMHz / 100.0;
}

double tenthsOfKelvinToCelsius(double tenthsOfKelvin)
{
    return tenthsOfKelvin / 10.0 - 273.15;
}

namespace {

#if !defined(__linux__) && !defined(_WIN32)
class NoSource : public Source
{
public:
    std::optional<double> clockMHz(const ClockScope &) override { return std::nullopt; }
    std::optional<double> temperatureC() override { return std::nullopt; }
    std::string temperatureLabel() const override { return {}; }
    std::optional<std::uint64_t> throttleCount() override { return std::nullopt; }
    std::optional<double> performanceLimitPercent() override { return std::nullopt; }
};
#endif

#if defined(__linux__)
class LinuxSource : public Source
{
public:
    LinuxSource() : m_temp(linuxTempSource("/sys"))
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            for (int c = 0; c < CPU_SETSIZE; ++c) {
                if (CPU_ISSET(c, &set))
                    m_allowed.push_back(c);
            }
        }
    }
    std::optional<double> clockMHz(const ClockScope &scope) override
    {
        if (!scope.allCpus && scope.core.ranked)
            return linuxClockMHz("/sys", {scope.core.cpu});
        return linuxClockMHz("/sys", m_allowed);
    }
    std::optional<double> temperatureC() override
    {
        return m_temp ? readHwmonCelsius(m_temp->inputPath) : std::nullopt;
    }
    std::string temperatureLabel() const override { return m_temp ? m_temp->label : std::string(); }
    std::optional<std::uint64_t> throttleCount() override { return linuxThrottleCount("/sys"); }
    std::optional<double> performanceLimitPercent() override { return std::nullopt; }

private:
    std::optional<TempSource> m_temp;
    std::vector<int> m_allowed;
};
#endif

#if defined(_WIN32)
// PDH: one query holding every counter, collected once per reading. Rate
// counters need two collections; the first reading after construction
// primes them (the constructor collects once).
class WindowsSource : public Source
{
public:
    WindowsSource()
    {
        if (PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS) {
            m_query = nullptr;
            return;
        }
        addCounter(L"\\Processor Information(_Total)\\% Processor Performance", m_perfTotal);
        addCounter(L"\\Processor Information(_Total)\\Processor Frequency", m_freqTotal);
        addCounter(L"\\Processor Information(_Total)\\% Performance Limit", m_limit);
        addCounter(L"\\Thermal Zone Information(*)\\High Precision Temperature", m_thermal);
        PdhCollectQueryData(m_query);
    }
    ~WindowsSource() override
    {
        if (m_query)
            PdhCloseQuery(m_query);
    }
    std::optional<double> clockMHz(const ClockScope &scope) override
    {
        if (!m_query)
            return std::nullopt;
        PDH_HCOUNTER perf = m_perfTotal, freq = m_freqTotal;
        if (!scope.allCpus && scope.core.ranked) {
            // The pinned core's own instance, "<group>,<index>", added on first use.
            const int key = isa::windowsKey(scope.core.group, scope.core.cpu);
            if (m_coreCounters.find(key) == m_coreCounters.end()) {
                const std::wstring instance = std::to_wstring(scope.core.group) + L"," + std::to_wstring(scope.core.cpu);
                PDH_HCOUNTER p = nullptr, f = nullptr;
                addCounter((L"\\Processor Information(" + instance + L")\\% Processor Performance").c_str(), p);
                addCounter((L"\\Processor Information(" + instance + L")\\Processor Frequency").c_str(), f);
                m_coreCounters[key] = {p, f};
                PdhCollectQueryData(m_query);  // prime the new rate counter
            }
            perf = m_coreCounters[key].first;
            freq = m_coreCounters[key].second;
        }
        collect();
        const auto p = value(perf), f = value(freq);
        if (!p || !f)
            return std::nullopt;
        return pdhClockMHz(*p, *f);
    }
    std::optional<double> temperatureC() override
    {
        if (!m_query || !m_thermal)
            return std::nullopt;
        collect();
        // Every thermal zone; the hottest.
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(m_thermal, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA)
            return std::nullopt;
        std::vector<unsigned char> buffer(size);
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
        if (PdhGetFormattedCounterArrayW(m_thermal, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
            return std::nullopt;
        std::optional<double> hottest;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                continue;
            const double c = tenthsOfKelvinToCelsius(items[i].FmtValue.doubleValue);
            if (!hottest || c > *hottest)
                hottest = c;
        }
        return hottest;
    }
    std::string temperatureLabel() const override
    {
        return m_thermal ? "ACPI thermal zone (may not be the CPU sensor)" : std::string();
    }
    std::optional<std::uint64_t> throttleCount() override { return std::nullopt; }
    std::optional<double> performanceLimitPercent() override
    {
        if (!m_query)
            return std::nullopt;
        collect();
        return value(m_limit);
    }

private:
    void addCounter(const wchar_t *path, PDH_HCOUNTER &counter)
    {
        // English names: the counters' localised names differ per language.
        if (PdhAddEnglishCounterW(m_query, path, 0, &counter) != ERROR_SUCCESS)
            counter = nullptr;
    }
    // One collection per sampler tick, however many readings it makes.
    void collect()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - m_lastCollect >= 200) {
            PdhCollectQueryData(m_query);
            m_lastCollect = now;
        }
    }
    static std::optional<double> value(PDH_HCOUNTER counter)
    {
        if (!counter)
            return std::nullopt;
        PDH_FMT_COUNTERVALUE v;
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS)
            return std::nullopt;
        if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA)
            return std::nullopt;
        return v.doubleValue;
    }

    PDH_HQUERY m_query = nullptr;
    PDH_HCOUNTER m_perfTotal = nullptr, m_freqTotal = nullptr, m_limit = nullptr, m_thermal = nullptr;
    std::map<int, std::pair<PDH_HCOUNTER, PDH_HCOUNTER>> m_coreCounters;
    ULONGLONG m_lastCollect = 0;
};
#endif

} // namespace

std::unique_ptr<Source> makeSystemSource()
{
#if defined(__linux__)
    return std::make_unique<LinuxSource>();
#elif defined(_WIN32)
    return std::make_unique<WindowsSource>();
#else
    return std::make_unique<NoSource>();
#endif
}

} // namespace sb::cpu::telemetry

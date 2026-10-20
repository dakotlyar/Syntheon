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

std::optional<double> linuxTopClockMHz(const std::string &sysfsRoot, int cpu)
{
    const auto khz = readInteger(fs::path(sysfsRoot) / "devices" / "system" / "cpu" / ("cpu" + std::to_string(cpu)) / "cpufreq"
                                 / "cpuinfo_max_freq");
    if (!khz || *khz <= 0)
        return std::nullopt;
    return double(*khz) / 1000.0;
}

std::optional<double> linuxWatchableTopClockMHz(const std::string &sysfsRoot, int cpu)
{
    if (!linuxClockMHz(sysfsRoot, {cpu}))
        return std::nullopt;
    return linuxTopClockMHz(sysfsRoot, cpu);
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
            // Tdie first: on Zen 1 / Zen+ Tctl is Tdie plus a 10-27 °C offset
            // (fan control), so it would overstate the temperature.
            for (const char *l : {"Tdie", "Tctl"}) {
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
            // A CPU zone ("cpu_thermal", "cpu0-thermal") before the SoC's
            // other zones (Qualcomm: aoss0-thermal, gpuss0-thermal, nsp0-thermal).
            rank = name.compare(0, 3, "cpu") == 0 ? 2 : 3;
            index = inputs.begin()->first;
            label = name;
        } else if (name == "acpitz") {
            rank = 4;
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
    std::map<long long, std::uint64_t> packages;  // physical_package_id -> its count (every CPU of it shows the same)
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(base, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() <= 3 || name.compare(0, 3, "cpu") != 0
            || !std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        if (const auto v = readInteger(e.path() / "thermal_throttle" / "core_throttle_count"); v && *v >= 0) {
            sum += std::uint64_t(*v);
            any = true;
        }
        if (const auto v = readInteger(e.path() / "thermal_throttle" / "package_throttle_count"); v && *v >= 0) {
            // Once per package, not once per CPU in it. Without a package id,
            // the CPU counts as a package of its own.
            const long long package = readInteger(e.path() / "topology" / "physical_package_id")
                                          .value_or(-1 - std::atoll(name.c_str() + 3));
            packages[package] = std::max(packages[package], std::uint64_t(*v));
            any = true;
        }
    }
    for (const auto &[package, count] : packages)
        sum += count;
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
// PDH: one query holding every counter, collected at most once per sampler
// tick. The processor counters are wildcards, so every core's instance
// ("<group>,<index>") and _Total come from the same collections: a core the
// single-thread mark moves to has a value at once. Rate counters need two
// collections: the constructor makes the first, and the first reading is
// taken no sooner than 200 ms later. Not thread-safe: one thread (the
// sampler) reads it.
class WindowsSource : public Source
{
public:
    WindowsSource()
    {
        if (PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS) {
            m_query = nullptr;
            return;
        }
        addCounter(L"\\Processor Information(*)\\% Processor Performance", m_perf);
        addCounter(L"\\Processor Information(*)\\Processor Frequency", m_freq);
        addCounter(L"\\Processor Information(_Total)\\% Performance Limit", m_limit);
        addCounter(L"\\Thermal Zone Information(*)\\High Precision Temperature", m_thermal);
        PdhCollectQueryData(m_query);
        m_lastCollect = GetTickCount64();
    }
    ~WindowsSource() override
    {
        if (m_query)
            PdhCloseQuery(m_query);
    }
    std::optional<double> clockMHz(const ClockScope &scope) override
    {
        if (!m_query || !collect())
            return std::nullopt;
        const std::wstring instance = !scope.allCpus && scope.core.ranked
                                          ? std::to_wstring(scope.core.group) + L"," + std::to_wstring(scope.core.cpu)
                                          : std::wstring(L"_Total");
        const auto p = instanceValue(m_perf, instance), f = instanceValue(m_freq, instance);
        if (!p || !f)
            return std::nullopt;
        return pdhClockMHz(*p, *f);
    }
    std::optional<double> temperatureC() override
    {
        if (!m_query || !collect())
            return std::nullopt;
        // Every thermal zone; the hottest.
        std::optional<double> hottest;
        for (const auto &[name, v] : values(m_thermal)) {
            const double c = tenthsOfKelvinToCelsius(v);
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
        if (!m_query || !collect() || !m_limit)
            return std::nullopt;
        PDH_FMT_COUNTERVALUE v;
        if (PdhGetFormattedCounterValue(m_limit, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS)
            return std::nullopt;
        if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA)
            return std::nullopt;
        return v.doubleValue;
    }

private:
    void addCounter(const wchar_t *path, PDH_HCOUNTER &counter)
    {
        // English names: the counters' localised names differ per language.
        if (PdhAddEnglishCounterW(m_query, path, 0, &counter) != ERROR_SUCCESS)
            counter = nullptr;
    }
    // Collects when 200 ms have passed since the last collection; false
    // until the first reading after the constructor's is possible.
    bool collect()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - m_lastCollect >= 200) {
            PdhCollectQueryData(m_query);
            m_lastCollect = now;
            m_collected = true;
        }
        return m_collected;
    }
    // A wildcard counter's instances and values.
    static std::vector<std::pair<std::wstring, double>> values(PDH_HCOUNTER counter)
    {
        std::vector<std::pair<std::wstring, double>> out;
        if (!counter)
            return out;
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA))
            return out;
        std::vector<unsigned char> buffer(size);
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
            return out;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                out.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
        }
        return out;
    }
    static std::optional<double> instanceValue(PDH_HCOUNTER counter, const std::wstring &instance)
    {
        for (const auto &[name, v] : values(counter)) {
            if (name == instance)
                return v;
        }
        return std::nullopt;
    }

    PDH_HQUERY m_query = nullptr;
    PDH_HCOUNTER m_perf = nullptr, m_freq = nullptr, m_limit = nullptr, m_thermal = nullptr;
    ULONGLONG m_lastCollect = 0;
    bool m_collected = false;
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

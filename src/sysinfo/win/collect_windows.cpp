// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows system info (x64 and ARM64), §5: native APIs only, no WMI, no
// shelling out (P13). Runs elevated (P12), but nothing here needs it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <batclass.h>
#include <winioctl.h>

#include "sysinfo/collector.h"

#include "privileged/win_inproc/windows_inprocess_ops.h"
#include "sysinfo/common/vulkan_probe.h"
#include "sysinfo/common/x86_cpu.h"
#include "sysinfo/parsers/smbios.h"
#include "sysinfo/parsers/text_parsers.h"
#include "sysinfo/parsers/windows_parsers.h"

#include <QHash>
#include <QSettings>
#include <QStorageInfo>
#include <QSysInfo>

#include <algorithm>
#include <vector>

namespace sb::sysinfo {

namespace {

// --- small Win32 helpers -----------------------------------------------------

template <typename T>
void release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : h(handle) {}
    ~Handle()
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
            CloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    bool ok() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

QSettings registry(const QString &path)
{
    return QSettings(QStringLiteral("HKEY_LOCAL_MACHINE\\") + path, QSettings::Registry64Format);
}

QString computerName()
{
    wchar_t buf[256] = {};
    DWORD size = 255;
    if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &size))
        return {};
    return QString::fromWCharArray(buf, int(size));
}

// --- CPU ---------------------------------------------------------------------

void readTopology(CpuInfo &cpu)
{
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0)
        return;
    std::vector<char> buffer(length);
    auto *first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationAll, first, &length))
        return;

    QVector<win::CoreEntry> cores;
    QVector<win::CacheEntry> caches;
    int packages = 0;
    for (DWORD offset = 0; offset < length;) {
        const auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (info->Size == 0)
            break;
        switch (info->Relationship) {
        case RelationProcessorCore: {
            win::CoreEntry c;
            c.efficiencyClass = info->Processor.EfficiencyClass;
            c.logicalProcessors = 0;
            for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                KAFFINITY mask = info->Processor.GroupMask[g].Mask;
                while (mask) {
                    c.logicalProcessors += int(mask & 1);
                    mask >>= 1;
                }
            }
            cores.append(c);
            break;
        }
        case RelationProcessorPackage:
            ++packages;
            break;
        case RelationCache:
            caches.append({info->Cache.Level, int(info->Cache.Type), qint64(info->Cache.CacheSize)});
            break;
        default:
            break;
        }
        offset += info->Size;
    }
    cpu.packages = packages;
    win::countCores(cores, cpu);
    cpu.caches = win::sumCaches(caches);
}

#if defined(_M_ARM64) || defined(__aarch64__)
QStringList armFeatures()
{
    // NEON is architectural on ARM64.
    QStringList raw{QStringLiteral("asimd")};
    const auto has = [](DWORD feature) { return IsProcessorFeaturePresent(feature) != 0; };
#ifdef PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("aes") << QStringLiteral("sha2");
#endif
#ifdef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("crc32");
#endif
#ifdef PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("atomics");
#endif
#ifdef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("asimddp");
#endif
#ifdef PF_ARM_SVE_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_SVE_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("sve");
#endif
#ifdef PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("sve2");
#endif
#ifdef PF_ARM_SVE_I8MM_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_SVE_I8MM_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("i8mm");
#endif
#ifdef PF_ARM_SVE_BF16_INSTRUCTIONS_AVAILABLE
    if (has(PF_ARM_SVE_BF16_INSTRUCTIONS_AVAILABLE))
        raw << QStringLiteral("bf16");
#endif
    return normalizeArmFeatures(raw);
}
#endif

CpuInfo readCpu()
{
    CpuInfo cpu;
    QSettings reg = registry(QStringLiteral("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"));
    cpu.name = reg.value(QStringLiteral("ProcessorNameString")).toString().simplified();
    cpu.baseMhz = reg.value(QStringLiteral("~MHz")).toInt();  // nominal clock
    const QString vendorId = reg.value(QStringLiteral("VendorIdentifier")).toString().simplified();
#if defined(_M_ARM64) || defined(__aarch64__)
    cpu.arch = QStringLiteral("arm64");
    cpu.vendor = vendorId.contains(QLatin1StringView("Qualcomm"), Qt::CaseInsensitive) ? QStringLiteral("Qualcomm")
                                                                                       : vendorId;
    cpu.features = armFeatures();
#else
    cpu.arch = QStringLiteral("x86_64");
    const X86Cpu x86 = queryX86Cpu();
    cpu.features = x86.features;
    if (cpu.name.isEmpty())
        cpu.name = x86.brand;
    const QString v = x86.vendor.isEmpty() ? vendorId : x86.vendor;
    cpu.vendor = v == QLatin1StringView("AuthenticAMD")   ? QStringLiteral("AMD")
                 : v == QLatin1StringView("GenuineIntel") ? QStringLiteral("Intel")
                                                          : v;
#endif
    readTopology(cpu);
    return cpu;
}

// --- emulation (D9) ------------------------------------------------------------

QString emulatedNativeArch()
{
    using IsWow64Process2Fn = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
    const auto fn = reinterpret_cast<IsWow64Process2Fn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2")));
    if (!fn)
        return {};  // before Windows 10 1709: no ARM64 emulation either
    USHORT process = 0, native = 0;
    if (!fn(GetCurrentProcess(), &process, &native))
        return {};
#if defined(_M_X64) || defined(__x86_64__)
    return win::emulatedNativeArch(true, native);
#else
    return win::emulatedNativeArch(false, native);
#endif
}

// --- GPUs: DXGI, D3D12, D3D11 (loaded at run time) ------------------------------

void readGpus(SystemInfo &info)
{
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi)
        return;
    using CreateFactory1 = HRESULT(WINAPI *)(REFIID, void **);
    const auto createFactory =
        reinterpret_cast<CreateFactory1>(reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto createD3D12 = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                                         reinterpret_cast<void *>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                                   : nullptr;
    const auto createD3D11 = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
                                         reinterpret_cast<void *>(GetProcAddress(d3d11, "D3D11CreateDevice")))
                                   : nullptr;

    IDXGIFactory1 *factory = nullptr;
    if (createFactory && SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
        IDXGIFactory6 *factory6 = nullptr;
        factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&factory6));
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1 *adapter = nullptr;
            HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                         __uuidof(IDXGIAdapter1),
                                                                         reinterpret_cast<void **>(&adapter))
                                  : factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr))
                break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {  // Microsoft Basic Render Driver
                release(adapter);
                continue;
            }
            GpuInfo g;
            g.name = QString::fromWCharArray(desc.Description).simplified();
            g.vendorId = desc.VendorId;
            g.deviceId = desc.DeviceId;
            g.vendor = pciVendorName(desc.VendorId);
            g.vramBytes = qint64(desc.DedicatedVideoMemory);
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                g.driverVersion = win::dxgiDriverVersion(umd.QuadPart);

            const bool firstHardware = info.gpus.isEmpty();  // the high-performance adapter
            if (firstHardware && createD3D12) {
                ID3D12Device *device = nullptr;
                if (SUCCEEDED(createD3D12(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                          reinterpret_cast<void **>(&device)))) {
                    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                                        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                                        D3D_FEATURE_LEVEL_11_0};
                    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
                    fl.NumFeatureLevels = UINT(std::size(levels));
                    fl.pFeatureLevelsRequested = levels;
                    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
                        info.apis.d3d12FeatureLevel = win::featureLevelName(fl.MaxSupportedFeatureLevel);
                    release(device);
                }
            }
            if (firstHardware && createD3D11) {
                const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                                    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
                D3D_FEATURE_LEVEL got{};
                // A null device pointer only asks which level would be created.
                if (SUCCEEDED(createD3D11(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, UINT(std::size(levels)),
                                          D3D11_SDK_VERSION, nullptr, &got, nullptr)))
                    info.apis.d3d11FeatureLevel = win::featureLevelName(got);
            }
            info.gpus.append(g);
            release(adapter);
        }
        release(factory6);
        release(factory);
    }
    if (d3d11)
        FreeLibrary(d3d11);
    if (d3d12)
        FreeLibrary(d3d12);
    FreeLibrary(dxgi);
}

// --- storage ---------------------------------------------------------------------

struct DriveInfo {
    QString model, bus, kind;
};

DriveInfo readDrive(const QString &rootPath)
{
    DriveInfo d;
    // "C:/" -> "\\.\C:". Opening with no access rights is enough for these IOCTLs.
    if (rootPath.size() < 2 || rootPath[1] != QLatin1Char(':'))
        return d;
    const QString volume = QStringLiteral("\\\\.\\") + rootPath.left(2);
    Handle vol(CreateFileW(reinterpret_cast<LPCWSTR>(volume.utf16()), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr));
    if (!vol.ok())
        return d;
    alignas(VOLUME_DISK_EXTENTS) char extentsBuf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
    DWORD bytes = 0;
    if (!DeviceIoControl(vol.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, extentsBuf, sizeof extentsBuf,
                         &bytes, nullptr))
        return d;
    const auto *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(extentsBuf);
    if (extents->NumberOfDiskExtents < 1)
        return d;
    const QString disk = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(extents->Extents[0].DiskNumber);
    Handle drive(CreateFileW(reinterpret_cast<LPCWSTR>(disk.utf16()), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr));
    if (!drive.ok())
        return d;

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    QByteArray descriptor(4096, '\0');
    if (DeviceIoControl(drive.h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, descriptor.data(),
                        DWORD(descriptor.size()), &bytes, nullptr)) {
        descriptor.resize(int(bytes));
        if (const auto parsed = win::parseStorageDeviceDescriptor(descriptor)) {
            d.model = parsed->model();
            d.bus = win::busTypeName(parsed->busType);
        }
    }

    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    if (DeviceIoControl(drive.h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &seek, sizeof seek, &bytes,
                        nullptr)
        && bytes >= sizeof seek)
        d.kind = seek.IncursSeekPenalty ? QStringLiteral("HDD") : QStringLiteral("SSD");
    return d;
}

QVector<VolumeInfo> readVolumes()
{
    QVector<VolumeInfo> out;
    for (const QStorageInfo &s : QStorageInfo::mountedVolumes()) {
        if (!s.isValid() || !s.isReady())
            continue;
        VolumeInfo v;
        v.mount = s.rootPath();
        v.fileSystem = QString::fromLatin1(s.fileSystemType());
        v.totalBytes = s.bytesTotal();
        v.freeBytes = s.bytesAvailable();
        const DriveInfo d = readDrive(v.mount);
        v.model = d.model;
        v.bus = d.bus;
        v.kind = d.kind;
        out.append(v);
    }
    return out;
}

// --- OS --------------------------------------------------------------------------

OsInfo readOs(const QString &nativeArch)
{
    OsInfo os;
    QSettings reg = registry(QStringLiteral("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"));
    const int build = reg.value(QStringLiteral("CurrentBuild")).toInt();
    const int ubr = reg.value(QStringLiteral("UBR")).toInt();
    os.name = win::windowsName(reg.value(QStringLiteral("ProductName")).toString(), build);
    os.version = reg.value(QStringLiteral("DisplayVersion")).toString();  // "24H2"
    os.build = QStringLiteral("%1.%2").arg(build).arg(ubr);
    os.kernel = QSysInfo::kernelVersion();
    os.arch = nativeArch.isEmpty() ? QSysInfo::currentCpuArchitecture() : nativeArch;
    return os;
}

// --- SetupAPI: NPUs (ComputeAccelerator class) and batteries ---------------------

// {F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}
const GUID kComputeAcceleratorClass = {0xf01a9d53, 0x3ff6, 0x48d2, {0x9f, 0x97, 0xc8, 0xa7, 0x00, 0x4b, 0xe1, 0x0c}};

QString deviceProperty(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD property)
{
    wchar_t buf[512] = {};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, &type, reinterpret_cast<PBYTE>(buf), sizeof buf - 2,
                                           nullptr))
        return {};
    return QString::fromWCharArray(buf);  // REG_MULTI_SZ: the first string
}

QVector<NpuInfo> readNpus()
{
    QVector<NpuInfo> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kComputeAcceleratorClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return out;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        NpuInfo n;
        n.name = deviceProperty(set, &dev, SPDRP_FRIENDLYNAME);
        if (n.name.isEmpty())
            n.name = deviceProperty(set, &dev, SPDRP_DEVICEDESC);
        n.vendor = deviceProperty(set, &dev, SPDRP_MFG);
        if (const auto ids = win::parseHardwareId(deviceProperty(set, &dev, SPDRP_HARDWAREID))) {
            n.vendorId = ids->first;
            n.deviceId = ids->second;
            if (const QString v = pciVendorName(n.vendorId); !v.isEmpty())
                n.vendor = v;
        }
        n.driver = deviceProperty(set, &dev, SPDRP_SERVICE);
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            wchar_t version[64] = {};
            DWORD size = sizeof version - 2;
            if (RegGetValueW(key, nullptr, L"DriverVersion", RRF_RT_REG_SZ, nullptr, version, &size) == ERROR_SUCCESS)
                n.driverVersion = QString::fromWCharArray(version);
            RegCloseKey(key);
        }
        out.append(n);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

QVector<win::BatteryFields> readBatteries()
{
    QVector<win::BatteryFields> out;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return out;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; i < 16 && SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVCLASS_BATTERY, i, &iface); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
        if (needed == 0)
            continue;
        std::vector<char> detailBuf(needed);
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr, nullptr))
            continue;
        Handle battery(CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!battery.ok())
            continue;
        ULONG wait = 0, tag = 0;
        DWORD bytes = 0;
        if (!DeviceIoControl(battery.h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof wait, &tag, sizeof tag, &bytes, nullptr)
            || tag == 0)
            continue;
        BATTERY_QUERY_INFORMATION query{};
        query.BatteryTag = tag;
        query.InformationLevel = BatteryInformation;
        BATTERY_INFORMATION bi{};
        if (!DeviceIoControl(battery.h, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof query, &bi, sizeof bi, &bytes,
                             nullptr))
            continue;
        if (!(bi.Capabilities & BATTERY_SYSTEM_BATTERY))
            continue;  // a UPS or a peripheral
        win::BatteryFields f;
        f.relative = (bi.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
        f.designedCapacity = bi.DesignedCapacity;
        f.fullChargedCapacity = bi.FullChargedCapacity;
        f.cycleCount = bi.CycleCount;
        out.append(f);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

BatteryInfo readBattery()
{
    SYSTEM_POWER_STATUS ps{};
    const bool havePs = GetSystemPowerStatus(&ps) != 0;
    return win::combineBattery(readBatteries(), havePs ? ps.ACLineStatus : 255, havePs ? ps.BatteryLifePercent : 255,
                               havePs ? ps.BatteryFlag : 255);
}

} // namespace

BatteryInfo readBatteryNow()
{
    return readBattery();
}

SystemInfo collectSystemInfo()
{
    SystemInfo info;
    info.machine.hostname = computerName();
    info.nativeArch = emulatedNativeArch();
    info.emulated = !info.nativeArch.isEmpty();

    info.cpu = readCpu();

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem))
        info.memory.totalBytes = qint64(mem.ullTotalPhys);
    WindowsInProcessOps ops;  // GetSystemFirmwareTable('RSMB'): no driver needed
    applyPrivilegedSmbios(info, ops);  // modules; also fills maker/model/BIOS

    readGpus(info);
    const VulkanInfo vk = probeVulkan();
    info.apis.vulkanInstanceVersion = vk.instanceVersion;
    for (GpuInfo &g : info.gpus) {
        for (const VulkanDevice &d : vk.devices) {
            if (d.vendorId == g.vendorId && d.deviceId == g.deviceId) {
                g.vulkanVersion = d.apiVersion;
                break;
            }
        }
    }

    info.volumes = readVolumes();
    info.os = readOs(info.nativeArch);
    info.npus = readNpus();
    info.battery = readBattery();
    info.app = appInfo();
    return info;
}

} // namespace sb::sysinfo

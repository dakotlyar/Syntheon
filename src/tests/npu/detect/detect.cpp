// SPDX-License-Identifier: GPL-3.0-or-later
// The vendor selection + N/A-reasons taxonomy (Phase 4c design §3): reads the
// snapshot's `npus[]`, picks the primary vendor in Intel → AMD → Qualcomm
// order (N3), applies the platform gate (Qualcomm is Windows-on-ARM only, N4),
// then hands off to the per-OS runtime probe (detect_win.cpp / detect_linux.cpp).
#include "tests/npu/detect/detect.h"

#include <QJsonArray>
#include <QJsonObject>

namespace sb::npu {

namespace {

QString keyNoNpu() { return QStringLiteral("no_npu"); }
QString keyDriverMissing() { return QStringLiteral("driver_missing"); }
QString keyRuntimeMissing() { return QStringLiteral("runtime_missing"); }
QString keyModelMissing() { return QStringLiteral("model_missing"); }
QString keyUnsupported() { return QStringLiteral("unsupported_vendor"); }
QString keyLoadFailed() { return QStringLiteral("runtime_load_failed"); }

// The user strings (§3.3), exactly. %1 = vendor, %2 = platform, %3 = error.
QString templateFor(NaReason r)
{
    switch (r) {
    case NaReason::NoNpu:
        return QStringLiteral("No supported NPU found on this machine");
    case NaReason::DriverMissing:
        return QStringLiteral("an %1 NPU is present but its driver isn't installed");
    case NaReason::RuntimeMissing:
        return QStringLiteral("the %1 runtime isn't installed");
    case NaReason::ModelMissing:
        return QStringLiteral("the %1 model pack isn't downloaded (offline or blocked)");
    case NaReason::UnsupportedVendor:
        return QStringLiteral("%1 isn't supported on %2");
    case NaReason::RuntimeLoadFailed:
        return QStringLiteral("the %1 runtime failed to load: %3");
    case NaReason::None:
        break;
    }
    return {};
}

bool qualcommSupported()
{
#if defined(Q_OS_WIN) && (defined(Q_PROCESSOR_ARM_64) || defined(_M_ARM64))
    return true;   // QNN/QAIRT runs on Windows on Snapdragon (ARM64) only (N4)
#else
    return false;
#endif
}

// Intel runs on both Windows and Linux (D1b): the same OpenVINO engine compiles
// for .dll (Windows) and .so (Linux). Intel-on-Linux is BUILD-ONLY and
// unvalidated — Nivedellir, the only Intel validation machine, is Windows (N4),
// so the Linux branch compiles and is fixture-tested but has never run a real
// NPU. Qualcomm stays Windows-on-ARM only; AMD runs on both (D1).
bool intelSupported()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

// The vendor a snapshot NPU entry is, per §3.2. Each branch matches by
// vendor string, driver string and/or name, because the real collectors leave
// different fields set per OS and silicon: a Windows ComputeAccelerator entry
// carries a pciVendorName() vendor ("AMD", "Intel"), an SPDRP_MFG manufacturer
// ("Advanced Micro Devices, Inc.", "Qualcomm Technologies, Inc."), an inf
// service name ("IpuMcdmDriver", "intel_vpu", "qcnspmcdm") and a friendly name,
// while Linux sysfs only sets the driver ("amdxdna"). A vendor-only match with
// an empty name/driver is still a real Windows entry (e.g. driverless
// PCI\VEN_1022), not Null. Anything else → Null.
NpuVendor vendorOf(const QJsonObject &npu)
{
    const QString vendor = npu.value(QStringLiteral("vendor")).toString();
    const QString driver = npu.value(QStringLiteral("driver")).toString();
    const QString name = npu.value(QStringLiteral("name")).toString();
    if (vendor.startsWith(QLatin1StringView("Intel"), Qt::CaseInsensitive)
        || driver == QLatin1StringView("intel_vpu")
        || name.contains(QLatin1StringView("Intel"), Qt::CaseInsensitive))
        return NpuVendor::Intel;
    if (vendor == QLatin1StringView("AMD")
        || vendor.contains(QLatin1StringView("Advanced Micro Devices"), Qt::CaseInsensitive)
        || driver == QLatin1StringView("amdxdna")
        || driver.compare(QLatin1StringView("IpuMcdmDriver"), Qt::CaseInsensitive) == 0
        || name.contains(QLatin1StringView("AMD XDNA"), Qt::CaseInsensitive))
        return NpuVendor::Amd;
    if (name.contains(QLatin1StringView("Qualcomm"), Qt::CaseInsensitive)
        || driver == QLatin1StringView("qcnspmcdm")
        || vendor.contains(QLatin1StringView("Qualcomm"), Qt::CaseInsensitive))
        return NpuVendor::Qualcomm;
    return NpuVendor::Null;
}

// The first NPU of the given vendor, or {} (Intel → AMD → Qualcomm order, N3).
QJsonObject firstOf(const QJsonArray &npus, NpuVendor vendor)
{
    for (const QJsonValue &v : npus) {
        const QJsonObject o = v.toObject();
        if (vendorOf(o) == vendor)
            return o;
    }
    return {};
}

} // namespace

QString reasonKey(NaReason r)
{
    switch (r) {
    case NaReason::NoNpu:
        return keyNoNpu();
    case NaReason::DriverMissing:
        return keyDriverMissing();
    case NaReason::RuntimeMissing:
        return keyRuntimeMissing();
    case NaReason::ModelMissing:
        return keyModelMissing();
    case NaReason::UnsupportedVendor:
        return keyUnsupported();
    case NaReason::RuntimeLoadFailed:
        return keyLoadFailed();
    case NaReason::None:
        break;
    }
    return {};
}

QString reasonString(NaReason r, const QStringList &args)
{
    QString s = templateFor(r);
    if (s.isEmpty())
        return {};
    // RuntimeLoadFailed's error lands in the %3 slot, but the common call is
    // {vendor, error} (2-arg) rather than {vendor, platform, error} (3-arg).
    const auto value = [&](int slot) {
        if (r == NaReason::RuntimeLoadFailed && slot == 3)
            return args.size() >= 3 ? args.value(2) : args.value(1);
        return args.value(slot - 1);
    };
    // Substitute through one-use sentinels so an argument's own "%1"/"%2"/"%3"
    // text is never rewritten by a later .replace() (and a 2-arg LoadFailed
    // still yields the error, not an empty tail).
    const QStringList sent{QStringLiteral("\x01"), QStringLiteral("\x02"), QStringLiteral("\x03")};
    for (int i = 1; i <= 3; ++i)
        s.replace(QStringLiteral("%") + QString::number(i), sent.at(i - 1));
    for (int i = 1; i <= 3; ++i)
        s.replace(sent.at(i - 1), value(i));
    return s;
}

NpuDetect detect(const SystemSnapshot &snapshot)
{
    return detect(snapshot, QString());
}

NpuDetect detect(const SystemSnapshot &snapshot, const QString &root)
{
    const QJsonArray npus = snapshot.data.value(QStringLiteral("npus")).toArray();

    // The primary vendor, in Intel → AMD → Qualcomm order (N3: first hit wins).
    NpuVendor vendor = NpuVendor::Null;
    QJsonObject npu;
    for (NpuVendor v : {NpuVendor::Intel, NpuVendor::Amd, NpuVendor::Qualcomm}) {
        npu = firstOf(npus, v);
        if (!npu.isEmpty()) {
            vendor = v;
            break;
        }
    }
    if (vendor == NpuVendor::Null)
        return {NpuVendor::Null, {}, {}, {}, {}, NaReason::NoNpu};

    NpuDetect out;
    out.vendor = vendor;
    out.deviceName = npu.value(QStringLiteral("name")).toString();
    out.driver = npu.value(QStringLiteral("driver")).toString();

    // Qualcomm only runs on Windows-on-ARM (N4): unsupported everywhere else.
    if (vendor == NpuVendor::Qualcomm && !qualcommSupported()) {
        out.reason = NaReason::UnsupportedVendor;
        return out;
    }

    // Intel runs on Windows and Linux (D1b: AMD XDNA is no longer the sole Linux
    // path, but Intel-on-Linux is build-only/unvalidated). Gate before the driver
    // check so an unsupported OS never reports DriverMissing.
    if (vendor == NpuVendor::Intel && !intelSupported()) {
        out.reason = NaReason::UnsupportedVendor;
        return out;
    }

    // A recognised vendor with no bound driver (a Windows ComputeAccelerator
    // entry with a problem, §3.2).
    if (out.driver.isEmpty()) {
        out.reason = NaReason::DriverMissing;
        return out;
    }

    return probeRuntime(snapshot, vendor, npu, root);
}

} // namespace sb::npu

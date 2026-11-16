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

// The vendor a snapshot NPU entry is (by the vendor string, or its driver).
NpuVendor vendorOf(const QJsonObject &npu)
{
    const QString vendor = npu.value(QStringLiteral("vendor")).toString();
    const QString driver = npu.value(QStringLiteral("driver")).toString();
    if (vendor == QLatin1StringView("Intel") || driver == QLatin1StringView("intel_vpu"))
        return NpuVendor::Intel;
    if (vendor == QLatin1StringView("AMD") || driver == QLatin1StringView("amdxdna"))
        return NpuVendor::Amd;
    if (vendor == QLatin1StringView("Qualcomm"))
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
    if (s.contains(QStringLiteral("%1")))
        s.replace(QStringLiteral("%1"), args.value(0));
    if (s.contains(QStringLiteral("%2")))
        s.replace(QStringLiteral("%2"), args.value(1));
    if (s.contains(QStringLiteral("%3")))
        s.replace(QStringLiteral("%3"), args.value(2));
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

    // A recognised vendor with no bound driver (a Windows ComputeAccelerator
    // entry with a problem, §3.2).
    if (out.driver.isEmpty()) {
        out.reason = NaReason::DriverMissing;
        return out;
    }

    return probeRuntime(snapshot, vendor, npu, root);
}

} // namespace sb::npu

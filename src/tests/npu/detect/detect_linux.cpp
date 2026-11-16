// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux runtime probe (Phase 4c design §3.2, D1a): AMD XDNA is the only
// Linux path — the amdxdna kernel driver already bound the device into
// /sys/class/accel (N1), and the user-space runtime is XRT at /opt/xilinx/xrt
// (N2). Intel is Windows-only (D1a, gated in detect.cpp before this is reached);
// Qualcomm has no desktop-QNN runtime on Linux (N4, also gated).
#include "tests/npu/detect/detect.h"

#include <QFileInfo>
#include <QJsonObject>

namespace sb::npu {

namespace {

// `root` prefixes absolute paths when set (tst_npu_detect's synthetic tree).
QString at(const QString &root, const QString &absPath)
{
    return root.isEmpty() ? absPath : root + absPath;
}

bool exists(const QString &root, const QString &absPath)
{
    return QFileInfo::exists(at(root, absPath));
}

} // namespace

NpuDetect probeRuntime(const SystemSnapshot &, NpuVendor vendor, const QJsonObject &npu, const QString &root)
{
    NpuDetect out;
    out.vendor = vendor;
    out.deviceName = npu.value(QStringLiteral("name")).toString();
    out.driver = npu.value(QStringLiteral("driver")).toString();

    switch (vendor) {
    case NpuVendor::Intel:
        out.reason = NaReason::UnsupportedVendor;   // D1a: Intel-on-Linux is build-only/unvalidated
        return out;
    case NpuVendor::Amd:
        // The AMD runtime is XRT (the amdxdna kernel driver already bound the
        // device, N2). Missing → N/A with the AMD link (surfaced in probe()).
        if (exists(root, QStringLiteral("/opt/xilinx/xrt"))) {
            out.runtimePath = at(root, QStringLiteral("/opt/xilinx/xrt"));
            out.reason = NaReason::None;
        } else {
            out.reason = NaReason::RuntimeMissing;
        }
        return out;
    case NpuVendor::Qualcomm:
        out.reason = NaReason::UnsupportedVendor;   // N4: no desktop QNN runtime on Linux
        return out;
    case NpuVendor::Null:
        out.reason = NaReason::NoNpu;
        return out;
    }
    out.reason = NaReason::NoNpu;
    return out;
}

} // namespace sb::npu

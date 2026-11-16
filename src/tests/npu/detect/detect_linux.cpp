// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux runtime probe (Phase 4c design §3.2, D1b): AMD XDNA via the amdxdna
// kernel driver + XRT at /opt/xilinx/xrt (N2), and Intel via OpenVINO — the same
// engine compiled for `.so`, so the probe checks the exact lib names the engine
// dlopens (engine_openvino.cpp coreName/cApiName/pluginName). Intel-on-Linux is
// BUILD-ONLY and unvalidated: Nivedellir, the only Intel validation machine, is
// Windows (N4), so this branch compiles and is fixture-tested but has never run
// a real NPU. Qualcomm has no desktop-QNN runtime on Linux (N4, gated in
// detect.cpp before this is reached).
#include "tests/npu/detect/detect.h"

#include <QDir>
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

// The three `.so` files the OpenVINO engine dlopens on Linux
// (engine_openvino.cpp coreName/cApiName/pluginName): the C++ core, the C API
// (ov_* symbols) and the Intel NPU plugin. All three must sit together in one
// directory for the engine to initialise, so the probe requires the whole set —
// fail-closed: any one missing → RuntimeMissing.
QStringList openVinoLibs()
{
    return {
        QStringLiteral("libopenvino.so"),
        QStringLiteral("libopenvino_c.so"),
        QStringLiteral("openvino_intel_npu_plugin.so"),
    };
}

bool dirHasOpenVinoLibs(const QString &root, const QString &dir)
{
    for (const QString &lib : openVinoLibs())
        if (!exists(root, dir + QStringLiteral("/") + lib))
            return false;
    return true;
}

// Subdirectories of the absolute `parent` whose name starts with `prefix`,
// most-specific (newest) first. Returns absolute, un-prefixed paths (callers
// pass them back through at()/exists()); it enumerates `root`+`parent` on disk,
// so the seam stays hermetic. An empty prefix matches every entry; callers loop
// and verify the runtime is actually on disk rather than trusting the first
// reversed name (a "logs"/"uninstall" sibling sorts above a numeric version dir).
QStringList dirsWithPrefix(const QString &root, const QString &parent, const QString &prefix)
{
    QStringList out;
    const QDir d(at(root, parent));
    if (!d.exists())
        return out;
    const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                            QDir::Name | QDir::Reversed);
    for (const QString &e : entries)
        if (prefix.isEmpty() || e.startsWith(prefix, Qt::CaseInsensitive))
            out.append(parent + QStringLiteral("/") + e);
    return out;
}

// OpenVINO (Intel) on Linux: find a directory holding all three of the engine's
// libs, checking the common install layouts in turn. Fail-closed — a directory
// is only accepted when every lib is actually on disk (§3.2).
bool findOpenVino(const QString &root, QString *dir)
{
    const auto accept = [&](const QString &d) {
        if (dirHasOpenVinoLibs(root, d)) {
            *dir = at(root, d);
            return true;
        }
        return false;
    };

    // 1. The system-wide and fixed archive install locations.
    for (const QString &d : {
             QStringLiteral("/opt/intel/openvino/runtime/lib/intel64"),
             QStringLiteral("/opt/intel/openvino/runtime/lib"),
             QStringLiteral("/usr/lib/x86_64-linux-gnu"),
             QStringLiteral("/usr/lib/aarch64-linux-gnu"),
             QStringLiteral("/usr/lib64"),
             QStringLiteral("/usr/lib"),
         }) {
        if (accept(d))
            return true;
    }

    // 2. A versioned /opt/intel/openvino_<ver> install (tarball/runtime package).
    for (const QString &base : dirsWithPrefix(root, QStringLiteral("/opt/intel"), QStringLiteral("openvino")))
        for (const QString &sub : {QStringLiteral("/runtime/lib/intel64"), QStringLiteral("/runtime/lib")})
            if (accept(base + sub))
                return true;

    // 3. A pip-installed OpenVINO: <python>/...site-packages/openvino/libs.
    for (const QString &libbase : {
             QStringLiteral("/usr/local/lib"),
             QStringLiteral("/usr/lib"),
             QStringLiteral("/usr/lib64"),
         }) {
        for (const QString &py : dirsWithPrefix(root, libbase, QStringLiteral("python"))) {
            if (accept(py + QStringLiteral("/dist-packages/openvino/libs")))
                return true;
            if (accept(py + QStringLiteral("/site-packages/openvino/libs")))
                return true;
        }
    }
    return false;
}

} // namespace

NpuDetect probeRuntime(const SystemSnapshot &, NpuVendor vendor, const QJsonObject &npu, const QString &root)
{
    NpuDetect out;
    out.vendor = vendor;
    out.deviceName = npu.value(QStringLiteral("name")).toString();
    out.driver = npu.value(QStringLiteral("driver")).toString();

    switch (vendor) {
    case NpuVendor::Intel: {
        // Intel-on-Linux is build-only/unvalidated (D1b): the probe mirrors the
        // Windows OpenVINO check for the .so lib names the engine dlopens.
        QString dir;
        if (findOpenVino(root, &dir)) {
            out.runtimePath = dir;
            out.reason = NaReason::None;
        } else {
            out.reason = NaReason::RuntimeMissing;
        }
        return out;
    }
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

// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows runtime probe (Phase 4c design §3.2): Intel via OpenVINO
// (openvino.dll + openvino_intel_npu_plugin.dll), AMD via Ryzen AI Software
// (RYZEN_AI_INSTALLATION_PATH / C:\Program Files\RyzenAI\… deployment DLLs),
// Qualcomm via the QAIRT/QNN SDK. This file is Windows-only (P-WIN): it is
// compiled on Windows and A-W syntax-checked on Linux; it never runs here.
#include "tests/npu/detect/detect.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcessEnvironment>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <array>

namespace sb::npu {

namespace {

// `root` prefixes the default-install paths when set (the tst_npu_detect seam;
// on real Windows it is empty). Env vars and registry are real either way.
QString at(const QString &root, const QString &absPath)
{
    if (absPath.isEmpty())
        return {};
    return root.isEmpty() ? absPath : root + QStringLiteral("/") + absPath;
}

bool exists(const QString &root, const QString &path)
{
    const QString p = at(root, path);
    return !p.isEmpty() && (QFileInfo::exists(p) || QDir(p).exists());
}

QString programFiles(const QString &root)
{
    const QString e = qEnvironmentVariable("ProgramFiles");
    return root.isEmpty() ? e : root + QStringLiteral("/Program Files");
}

// The first subdirectory of `parent` matching the glob-ish prefix, or "".
QString firstDirWithPrefix(const QString &parent, const QString &prefix)
{
    const QDir d(parent);
    if (!d.exists())
        return {};
    const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &e : entries)
        if (e.startsWith(prefix, Qt::CaseInsensitive))
            return d.filePath(e);
    return {};
}

// OpenVINO (Intel): the core + the NPU plugin must be findable (Program Files,
// INTEL_OPENVINO_DIR, or a pip site-packages). Returns true and sets *dir.
bool findOpenVino(const QString &root, QString *dir)
{
    auto ok = [&](const QString &d) {
        if (!exists(root, d + QStringLiteral("/openvino.dll")))
            return false;
        if (!exists(root, d + QStringLiteral("/openvino_intel_npu_plugin.dll")))
            return false;
        *dir = at(root, d);
        return true;
    };

    if (const QString env = qEnvironmentVariable("INTEL_OPENVINO_DIR"); !env.isEmpty()) {
        for (const QString &sub : {QStringLiteral("/runtime/bin/intel64/Release"),
                                   QStringLiteral("/runtime/bin/intel64/Debug"), QString()})
            if (ok(env + sub))
                return true;
    }
    const QString pf = programFiles(root);
    const QString base = firstDirWithPrefix(pf, QStringLiteral("Intel"));
    if (!base.isEmpty()) {
        const QString runtime = firstDirWithPrefix(base, QStringLiteral("openvino"));
        if (!runtime.isEmpty() && ok(runtime + QStringLiteral("/runtime/bin/intel64/Release")))
            return true;
    }
    // A pip-installed openvino: site-packages/openvino/libs/openvino.dll.
    for (const QString &sp : {
             QStringLiteral("%1/AppData/Local/Programs/Python/Python312/Lib/site-packages/openvino/libs"),
             QStringLiteral("%1/AppData/Local/Programs/Python/Python311/Lib/site-packages/openvino/libs")}) {
        if (ok(sp.arg(qEnvironmentVariable("LOCALAPPDATA"))))
            return true;
    }
    return false;
}

// AMD Ryzen AI: RYZEN_AI_INSTALLATION_PATH or the default install's deployment
// folder has onnxruntime.dll + the Vitis AI EP DLL (N2). Returns true + *dir.
bool findRyzenAi(const QString &root, QString *dir)
{
    const QString env = qEnvironmentVariable("RYZEN_AI_INSTALLATION_PATH");
    const QDir d(at(root, env));
    if (!env.isEmpty() && d.exists()) {
        const QString dep = d.filePath(QStringLiteral("deployment"));
        if (exists(root, dep + QStringLiteral("/onnxruntime.dll"))
            && exists(root, dep + QStringLiteral("/vitisai_ep.dll"))) {
            *dir = dep;
            return true;
        }
    }
    const QString ryzen = firstDirWithPrefix(programFiles(root) + QStringLiteral("/RyzenAI"), QStringLiteral("ryzen-ai-"));
    if (!ryzen.isEmpty() && exists(root, ryzen + QStringLiteral("/deployment/onnxruntime.dll"))
        && exists(root, ryzen + QStringLiteral("/deployment/vitisai_ep.dll"))) {
        *dir = ryzen + QStringLiteral("/deployment");
        return true;
    }
    return false;
}

// Qualcomm QAIRT/QNN: the SDK path or a registry install. For the desktop
// QNN runtime we look for the core QnnHtp library.
bool findQnn(const QString &root, QString *dir)
{
    const QString env = qEnvironmentVariable("QNN_SDK_ROOT");
    if (!env.isEmpty() && QDir(at(root, env)).exists()) {
        *dir = at(root, env);
        return true;
    }
    const QString qairt = firstDirWithPrefix(programFiles(root) + QStringLiteral("/Qualcomm"), QStringLiteral("AI"));
    if (!qairt.isEmpty()) {
        *dir = qairt;
        return true;
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

    QString dir;
    switch (vendor) {
    case NpuVendor::Intel:
        if (findOpenVino(root, &dir)) {
            out.runtimePath = dir;
            out.reason = NaReason::None;
        } else {
            out.reason = NaReason::RuntimeMissing;
        }
        return out;
    case NpuVendor::Amd:
        if (findRyzenAi(root, &dir)) {
            out.runtimePath = dir;
            out.reason = NaReason::None;
        } else {
            out.reason = NaReason::RuntimeMissing;
        }
        return out;
    case NpuVendor::Qualcomm:
        if (findQnn(root, &dir)) {
            out.runtimePath = dir;
            out.reason = NaReason::None;
        } else {
            out.reason = NaReason::RuntimeMissing;
        }
        return out;
    case NpuVendor::Null:
        out.reason = NaReason::NoNpu;
        return out;
    }
    out.reason = NaReason::NoNpu;
    return out;
}

} // namespace sb::npu

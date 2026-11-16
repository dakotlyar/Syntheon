// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows runtime probe (Phase 4c design §3.2): Intel via OpenVINO
// (openvino.dll + openvino_c.dll + openvino_intel_npu_plugin.dll), AMD via Ryzen
// AI Software (RYZEN_AI_INSTALLATION_PATH / C:\Program Files\RyzenAI\…
// deployment DLLs), Qualcomm via the QAIRT/QNN SDK (a QnnHtp.dll + QnnSystem.dll
// pair actually present). This file is Windows-only (P-WIN): it is compiled on
// Windows and A-W syntax-checked on Linux; it never runs here.
#include "tests/npu/detect/detect.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace sb::npu {

namespace {

// The `root` test seam (tst_npu_detect) is empty on real Windows. When it is
// set, the probe is hermetic: it only looks under `root` at the *default*
// install layout and never reads INTEL_OPENVINO_DIR / LOCALAPPDATA /
// RYZEN_AI_INSTALLATION_PATH / QNN_SDK_ROOT / QAIRT_SDK_ROOT, so a
// QTemporaryDir fixture fully determines the outcome.

bool fileExists(const QString &path)
{
    return !path.isEmpty() && QFileInfo::exists(path);
}

// A host env var, read only on real Windows ("" when the seam is active).
QString env(const char *name, const QString &root)
{
    return root.isEmpty() ? qEnvironmentVariable(name) : QString();
}

// "Program Files": %ProgramFiles% on real Windows, "<root>/Program Files"
// under the seam. Every later path derives from this (already root-carrying)
// base via QDir/QString, so nothing is ever re-prefixed.
QString programFiles(const QString &root)
{
    return root.isEmpty() ? qEnvironmentVariable("ProgramFiles")
                          : root + QStringLiteral("/Program Files");
}

// All subdirectories of `parent` matching the glob-ish prefix (an empty
// prefix matches any), most-specific first (QDir::Name reversed so a leftover
// older SDK dir never shadows a newer one). Callers must loop over these and
// verify the runtime is actually on disk before accepting one: a single
// reversed name is not enough, since a non-SDK sibling ("Intel Corporation",
// "uninstall", "logs") can sort above the real SDK dir, while a numeric
// version dir ("2.45.0") sorts below it.
QStringList dirsWithPrefix(const QString &parent, const QString &prefix)
{
    QStringList out;
    const QDir d(parent);
    if (!d.exists())
        return out;
    const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                            QDir::Name | QDir::Reversed);
    for (const QString &e : entries)
        if (prefix.isEmpty() || e.startsWith(prefix, Qt::CaseInsensitive))
            out.append(d.filePath(e));
    return out;
}

// OpenVINO (Intel): the core + the C API + the NPU plugin must sit together
// (Program Files default, INTEL_OPENVINO_DIR, or a pip site-packages).
bool findOpenVino(const QString &root, QString *dir)
{
    const auto hasDlls = [&](const QString &d) {
        return fileExists(d + QStringLiteral("/openvino.dll"))
               && fileExists(d + QStringLiteral("/openvino_c.dll"))
               && fileExists(d + QStringLiteral("/openvino_intel_npu_plugin.dll"));
    };

    if (const QString e = env("INTEL_OPENVINO_DIR", root); !e.isEmpty()) {
        for (const QString &sub : {QStringLiteral("/runtime/bin/intel64/Release"),
                                   QStringLiteral("/runtime/bin/intel64/Debug"), QString()})
            if (hasDlls(e + sub)) {
                *dir = e + sub;
                return true;
            }
    }

    const QString pf = programFiles(root);
    // The exact "Intel" dir is probed first so it always beats a longer
    // "Intel*" sibling ("Intel Corporation" from standard Intel/Killer/IPF
    // drivers) that sorts above it under reversed name order. Every candidate
    // is verified to actually hold the runtime before it is accepted.
    const auto openvinoUnder = [&](const QString &base) {
        const QStringList runtimes = dirsWithPrefix(base, QStringLiteral("openvino"));
        for (const QString &runtime : runtimes) {
            const QString d = runtime + QStringLiteral("/runtime/bin/intel64/Release");
            if (hasDlls(d)) {
                *dir = d;
                return true;
            }
        }
        return false;
    };
    if (openvinoUnder(pf + QStringLiteral("/Intel")))
        return true;
    for (const QString &base : dirsWithPrefix(pf, QStringLiteral("Intel")))
        if (openvinoUnder(base))
            return true;

    // A pip-installed openvino (real Windows only): site-packages/openvino/libs.
    // LOCALAPPDATA is already C:\Users\<user>\AppData\Local, so the Python
    // runtime sits at <local>/Programs/Python/Python3* — scan newest first.
    if (const QString local = env("LOCALAPPDATA", root); !local.isEmpty()) {
        const QDir pybase(local + QStringLiteral("/Programs/Python"));
        if (pybase.exists()) {
            const QStringList pys = pybase.entryList(QStringList{QStringLiteral("Python3*")},
                                                     QDir::Dirs | QDir::NoDotAndDotDot,
                                                     QDir::Name | QDir::Reversed);
            for (const QString &p : pys) {
                const QString libs = pybase.filePath(p) + QStringLiteral("/Lib/site-packages/openvino/libs");
                if (hasDlls(libs)) {
                    *dir = libs;
                    return true;
                }
            }
        }
    }
    return false;
}

// AMD Ryzen AI: RYZEN_AI_INSTALLATION_PATH (real Windows) or the default
// install's deployment folder has onnxruntime.dll + the Vitis AI EP DLL (N2).
bool findRyzenAi(const QString &root, QString *dir)
{
    const auto hasDlls = [&](const QString &d) {
        return fileExists(d + QStringLiteral("/onnxruntime.dll"))
               && fileExists(d + QStringLiteral("/vitisai_ep.dll"));
    };

    if (const QString e = env("RYZEN_AI_INSTALLATION_PATH", root); !e.isEmpty()) {
        const QString dep = e + QStringLiteral("/deployment");
        if (hasDlls(dep)) {
            *dir = dep;
            return true;
        }
    }

    const QStringList ryzens = dirsWithPrefix(programFiles(root) + QStringLiteral("/RyzenAI"),
                                              QStringLiteral("ryzen-ai-"));
    for (const QString &ryzen : ryzens) {
        const QString dep = ryzen + QStringLiteral("/deployment");
        if (hasDlls(dep)) {
            *dir = dep;
            return true;
        }
    }
    return false;
}

// The exact directory holding BOTH QnnHtp.dll and QnnSystem.dll under `sdk`, or
// "" when either is missing (design §3.2: a QNN runtime is "present" only when
// the Htp + System DLL pair is actually on disk). The pair may sit under
// lib/<arch>-windows-msvc/ (ARM64) or bin/ — directly under the SDK root, or one
// version subdirectory deeper. Returns the DLL-containing directory itself so the
// engine can load the pair by exact path (matching the OpenVINO/RyzenAI style).
QString qnnDllDir(const QString &sdk)
{
    if (sdk.isEmpty())
        return {};
    const auto hasPair = [&](const QString &d) {
        return fileExists(d + QStringLiteral("/QnnHtp.dll"))
               && fileExists(d + QStringLiteral("/QnnSystem.dll"));
    };
    const QStringList subdirs = {
        QStringLiteral("lib/aarch64-windows-msvc"),
        QStringLiteral("lib/arm64x-windows-msvc"),
        QStringLiteral("bin"),
    };
    // `sdk` itself first, then its subdirectories most-specific first (QDir::Name
    // reversed). The empty prefix matches every dir, so loop and check rather than
    // trust the first reversed name: "uninstall"/"logs" sort above a numeric
    // version dir like "2.45.0", which is only reached later in the ordering.
    QStringList roots = dirsWithPrefix(sdk, QString());
    roots.prepend(sdk);
    for (const QString &base : roots) {
        if (base.isEmpty())
            continue;
        for (const QString &sub : subdirs) {
            const QString d = base + QStringLiteral("/") + sub;
            if (hasPair(d))
                return d;
        }
        if (hasPair(base))
            return base;
    }
    return {};
}

// Qualcomm QAIRT/QNN: an env SDK root with a QNN DLL pair, the Program Files
// QAIRT SDK, or the default C:\Qualcomm\AIStack\QAIRT\<ver> (N2). An empty
// QNN_SDK_ROOT must NOT count as present. On success *dir is the exact directory
// holding the Htp+System pair (H1), not the SDK root.
bool findQnn(const QString &root, QString *dir)
{
    const auto accept = [&](const QString &sdk) {
        const QString d = qnnDllDir(sdk);
        if (d.isEmpty())
            return false;
        *dir = d;
        return true;
    };

    if (const QString qnn = env("QNN_SDK_ROOT", root); !qnn.isEmpty() && accept(qnn))
        return true;
    if (const QString qairt = env("QAIRT_SDK_ROOT", root); !qairt.isEmpty() && accept(qairt))
        return true;

    const QStringList ais = dirsWithPrefix(programFiles(root) + QStringLiteral("/Qualcomm"),
                                           QStringLiteral("AI"));
    for (const QString &ai : ais) {
        if (accept(ai))
            return true;
    }

    const QString qairtBase = root.isEmpty() ? QStringLiteral("C:/Qualcomm/AIStack/QAIRT")
                                             : root + QStringLiteral("/Qualcomm/AIStack/QAIRT");
    const QStringList vers = dirsWithPrefix(qairtBase, QString());
    for (const QString &ver : vers) {
        if (accept(ver))
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

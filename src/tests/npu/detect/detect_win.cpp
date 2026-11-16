// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows runtime probe (Phase 4c design §3.2): Intel via OpenVINO
// (openvino.dll + openvino_intel_npu_plugin.dll), AMD via Ryzen AI Software
// (RYZEN_AI_INSTALLATION_PATH / C:\Program Files\RyzenAI\… deployment DLLs),
// Qualcomm via the QAIRT/QNN SDK (a QnnHtp.dll / QnnSystem.dll actually
// present). This file is Windows-only (P-WIN): it is compiled on Windows and
// A-W syntax-checked on Linux; it never runs here.
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

// The first subdirectory of `parent` matching the glob-ish prefix (an empty
// prefix matches any and returns the first by name), or "".
QString firstDirWithPrefix(const QString &parent, const QString &prefix)
{
    const QDir d(parent);
    if (!d.exists())
        return {};
    const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &e : entries)
        if (prefix.isEmpty() || e.startsWith(prefix, Qt::CaseInsensitive))
            return d.filePath(e);
    return {};
}

// OpenVINO (Intel): the core + the NPU plugin must sit together (Program Files
// default, INTEL_OPENVINO_DIR, or a pip site-packages).
bool findOpenVino(const QString &root, QString *dir)
{
    const auto hasDlls = [&](const QString &d) {
        return fileExists(d + QStringLiteral("/openvino.dll"))
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
    const QString base = firstDirWithPrefix(pf, QStringLiteral("Intel"));
    if (!base.isEmpty()) {
        const QString runtime = firstDirWithPrefix(base, QStringLiteral("openvino"));
        if (!runtime.isEmpty()) {
            const QString d = runtime + QStringLiteral("/runtime/bin/intel64/Release");
            if (hasDlls(d)) {
                *dir = d;
                return true;
            }
        }
    }

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

    const QString ryzen = firstDirWithPrefix(programFiles(root) + QStringLiteral("/RyzenAI"),
                                             QStringLiteral("ryzen-ai-"));
    if (!ryzen.isEmpty()) {
        const QString dep = ryzen + QStringLiteral("/deployment");
        if (hasDlls(dep)) {
            *dir = dep;
            return true;
        }
    }
    return false;
}

// A QNN runtime is only "present" if a QnnHtp.dll / QnnSystem.dll is actually
// on disk (design §3.2: "runtime present (registry or default SDK path)"). The
// DLL may sit under lib/<arch>-windows-msvc/ (ARM64) or a bin/ dir — directly
// under the SDK root, or one version subdirectory deeper.
bool qnnRuntimeUnder(const QString &sdk)
{
    if (sdk.isEmpty())
        return false;
    const QStringList rels = {
        QStringLiteral("lib/aarch64-windows-msvc/QnnHtp.dll"),
        QStringLiteral("lib/arm64x-windows-msvc/QnnHtp.dll"),
        QStringLiteral("lib/aarch64-windows-msvc/QnnSystem.dll"),
        QStringLiteral("lib/arm64x-windows-msvc/QnnSystem.dll"),
        QStringLiteral("bin/QnnHtp.dll"),
        QStringLiteral("bin/QnnSystem.dll"),
        QStringLiteral("QnnHtp.dll"),
    };
    const QStringList roots{sdk, firstDirWithPrefix(sdk, QString())};
    for (const QString &base : roots) {
        if (base.isEmpty())
            continue;
        for (const QString &r : rels)
            if (fileExists(base + QStringLiteral("/") + r))
                return true;
    }
    return false;
}

// Qualcomm QAIRT/QNN: an env SDK root with a QNN DLL, the Program Files QAIRT
// SDK, or the default C:\Qualcomm\AIStack\QAIRT\<ver> (N2). An empty
// QNN_SDK_ROOT must NOT count as present.
bool findQnn(const QString &root, QString *dir)
{
    if (const QString qnn = env("QNN_SDK_ROOT", root); !qnn.isEmpty() && qnnRuntimeUnder(qnn)) {
        *dir = qnn;
        return true;
    }
    if (const QString qairt = env("QAIRT_SDK_ROOT", root); !qairt.isEmpty() && qnnRuntimeUnder(qairt)) {
        *dir = qairt;
        return true;
    }

    const QString ai = firstDirWithPrefix(programFiles(root) + QStringLiteral("/Qualcomm"),
                                          QStringLiteral("AI"));
    if (!ai.isEmpty() && qnnRuntimeUnder(ai)) {
        *dir = ai;
        return true;
    }

    const QString qairtBase = root.isEmpty() ? QStringLiteral("C:/Qualcomm/AIStack/QAIRT")
                                             : root + QStringLiteral("/Qualcomm/AIStack/QAIRT");
    const QString ver = firstDirWithPrefix(qairtBase, QString());
    if (!ver.isEmpty() && qnnRuntimeUnder(ver)) {
        *dir = ver;
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

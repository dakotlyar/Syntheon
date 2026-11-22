// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/engine/genai_runtime.h"

#include <QDir>
#include <QFileInfo>

namespace sb::npu {

// The sibling implementation in engine_openvino.cpp (runtimeFile) resolves the
// OpenVINO core/plugin the same way; this copy serves the GenAI second runtime
// (DD-1 §5.4) and is kept here so the unit tests can probe it without the
// engine TU.
QString genAiRuntimeFile(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return {};
    const QDir d(dir);
    if (!d.exists())
        return {};
    const QString flat = d.filePath(name);
    if (QFileInfo::exists(flat))
        return flat;
    // SONAME form name.<ver> (the runtime packages / pip wheels keep only the
    // versioned file; the unversioned .so is a -dev namelink).
    const QStringList sonames = d.entryList({name + QStringLiteral(".*")}, QDir::Files, QDir::Name | QDir::Reversed);
    return sonames.isEmpty() ? QString() : d.filePath(sonames.first());
}

QString findGenAiModule(const QString &runtimePath, const QString &packageSubdir, const QString &name)
{
    // Flat in the runtime dir (toolkit installs).
    if (const QString flat = genAiRuntimeFile(runtimePath, name); !flat.isEmpty())
        return flat;
    // The pip layout: runtimePath = <py>/site-packages/openvino/libs; the genai
    // (or tokenizers) package is a sibling — <py>/site-packages/openvino_genai/.
    // The two packages differ: openvino_genai keeps its module FLAT in the
    // package dir (openvino_genai/libopenvino_genai.so, no lib/ subdir), while
    // openvino_tokenizers keeps it under lib/ (openvino_tokenizers/lib/
    // libopenvino_tokenizers.so) — probe BOTH at every ancestor walked up from
    // runtimePath (a depth cap so a weird path can't walk to the filesystem
    // root and accept an unrelated openvino_genai).
    QDir d(runtimePath);
    for (int i = 0; i < 6; ++i) {
        const QString pkgRoot = QDir(d.path()).filePath(packageSubdir);
        for (const QString &sub : {QStringLiteral("lib"), QString()}) {
            const QString dir = sub.isEmpty() ? pkgRoot : QDir(pkgRoot).filePath(sub);
            if (const QString f = genAiRuntimeFile(dir, name); !f.isEmpty())
                return f;
        }
        if (!d.cdUp())
            break;
    }
    return {};
}

} // namespace sb::npu

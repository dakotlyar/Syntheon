// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The OpenVINO GenAI "second runtime" probe (DD-1 §5.4 Option A): finds the
// openvino_genai runtime module (and the openvino_tokenizers extension it
// needs) beside the OpenVINO runtime dir that detect resolved — the engine's
// LLM path loads them lazily from the USER's install. Pure Qt and SDK-free, so
// this compiles on every build/CI box and the unit tests exercise it with
// fixture dirs (tst_npu_engine.cpp). Mirrors detect's loop-and-verify style
// and the engine's runtimeFile() helper: only an existing file is accepted,
// versioned SONAMEs included (pip wheels drop the unversioned .so).
#include <QString>

namespace sb::npu {

// The absolute file for `name` under `dir` — the unversioned file, else a
// `name.*` SONAME (most-specific first) — or "" when neither exists.
QString genAiRuntimeFile(const QString &dir, const QString &name);

// The absolute GenAI module for `name` beside the OpenVINO runtime dir: flat
// in `runtimePath` (toolkit installs ship openvino_genai in the same bin dir
// as openvino.dll), else under `<ancestor>/<packageSubdir>/lib/` walking up
// the tree from runtimePath (pip installs: runtimePath is
// <py>/site-packages/openvino/libs, the genai/tokenizers packages are
// siblings). "" when not found. Callers pass packageSubdir "openvino_genai"
// or "openvino_tokenizers" and the per-platform module name.
QString findGenAiModule(const QString &runtimePath, const QString &packageSubdir, const QString &name);

} // namespace sb::npu

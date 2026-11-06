// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OpenGL on the chosen GPU (Phase 4a design §4.2, §4.3, P13, D17, task 23): the
// environment the GL worker and the GL probe run in, and the GUI side's
// verification of the probe's answer. Pure apart from the file-exists check.

#include "tests/gpu/model/gpu_identity.h"
#include "tests/gpu/model/protocol.h"

#include <QProcessEnvironment>
#include <QString>
#include <QVector>

#include <chrono>
#include <functional>
#include <optional>

namespace sb::gpu {

bool defaultFileExists(const QString &path);

// §4.3. Linux, a Mesa driver (the kernel driver is amdgpu/i915/xe/nouveau/msm/panfrost, or the
// GPU's Vulkan driver says Mesa): DRI_PRIME=pci-<slot>, __GLX_VENDOR_LIBRARY_NAME=mesa and
// __EGL_VENDOR_LIBRARY_FILENAMES=.../50_mesa.json when that file exists. Linux, NVIDIA
// proprietary (kernel driver nvidia): __NV_PRIME_RENDER_OFFLOAD=1,
// __GLX_VENDOR_LIBRARY_NAME=nvidia and the 10_nvidia.json. Windows: nothing (Windows picks the
// GPU itself; Syntheon never writes the per-app GPU preference).
QProcessEnvironment glEnvironment(const GpuEntry &gpu, const std::function<bool(const QString &)> &fileExists = defaultFileExists);
// The same for a given OS (tests): windows = true gives an empty environment.
QProcessEnvironment glEnvironmentFor(const GpuEntry &gpu, bool windows, const std::function<bool(const QString &)> &fileExists);

// §4.2, D22: the OpenGL probe over a GPU list, within a shared deadline. `order` is the order to
// probe in (the default GPU first); a GPU that already has an `opengl` entry is left as it is. A
// prober that gives no answer (nullopt: the budget ran out, the worker didn't answer) stops the whole
// loop and stores NO entry for that GPU or any later one: the OpenGL worker checks it when the test
// starts. Linux ARM stores the fixed reason without probing.
using GlProber = std::function<std::optional<protocol::GlEvent>(const GpuEntry &gpu, std::chrono::milliseconds left)>;
void probeOpenGlOver(QVector<GpuEntry> &list, const QVector<int> &order, std::chrono::steady_clock::time_point deadline, bool linuxArm,
                     bool windowsArm, const GlProber &prober, bool waylandSession = false);

// "0000:79:00.0" -> "pci-0000_79_00_0".
QString driPrimeValue(const QString &pciSlot);

// §4.3's verification of a probe-gl answer against the chosen GPU and the machine's list: ok with
// the version, driver (the renderer) and a note, or the exact reason (§4.2). linuxArm: "OpenGL isn't
// tested on Linux ARM" without looking at the answer. windowsArm: a GDI Generic renderer is the
// missing compatibility pack (reason + hint URL).
// waylandSession (Linux): on a Wayland session a hardware GPU that drives no enabled display output is N/A
// ("OpenGL's test window can't be shown from this GPU on this Wayland session (it doesn't drive a display)"):
// the compositor can't show the EGL buffers another GPU renders. Unknown (no DRM card) is not N/A.
ApiAvailability verifyGl(const protocol::GlEvent &gl, const GpuEntry &chosen, const QVector<GpuEntry> &all, bool linuxArm, bool windowsArm = false,
                         bool waylandSession = false);

} // namespace sb::gpu

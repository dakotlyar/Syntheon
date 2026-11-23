// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU test's vendor + runtime detection (Phase 4c design §3, §3.3 D7).
// The hardware is already in the snapshot's `npus[]` (sysinfo, N1); here the
// test re-checks the *runtime* — whether the vendor's runtime is installed and
// loadable — and maps the outcome onto the stable N/A-reasons taxonomy below.
// QtCore-only and pure over strings/paths where possible (the registry / env /
// file-existence probes are the only OS calls), so it is unit-testable with
// fixture snapshots and a synthetic `root` (the sysinfo `root` pattern).

#include "core/result.h"
#include "tests/npu/ids.h"

#include <QString>
#include <QStringList>

namespace sb::npu {

// The N/A-reasons taxonomy (§3.3, D7): a canonical reason → key → user string.
// `None` is the internal "available / no problem" sentinel — it is never
// stored or shown and has an empty key.
enum class NaReason {
    None,
    NoNpu,             // "no supported NPU found on this machine"
    DriverMissing,     // "… NPU is present but its driver isn't installed"
    RuntimeMissing,    // "the … runtime isn't installed"
    ModelMissing,      // "the … model pack isn't downloaded (offline or blocked)"
    UnsupportedVendor, // "… isn't supported on …"
    RuntimeLoadFailed, // "the … runtime failed to load: …"
};
QString reasonKey(NaReason);
// The user string, with %1/%2/%3 substituted from `args` (vendor name, platform,
// error detail). NoNpu needs no args; DriverMissing/ModelMissing/RuntimeMissing
// take %1 (vendor); UnsupportedVendor takes %1 (vendor) + %2 (platform);
// RuntimeLoadFailed takes %1 (vendor) + %3 (error).
QString reasonString(NaReason, const QStringList &args = {});

// AMD and Qualcomm have detection but no working inference engine yet (their
// engines fail "deferred to §16"). Until they do, the test is N/A for them
// before any model download. Not a taxonomy reason: a plain user string.
bool engineNotYetSupported(NpuVendor vendor);
QString engineNotYetSupportedText(NpuVendor vendor);

struct NpuDetect {
    NpuVendor vendor = NpuVendor::Null;   // the primary vendor (§3.2, Intel → AMD → Qualcomm)
    QString deviceName;                   // from the snapshot ("AMD XDNA NPU")
    QString driver;                       // "amdxdna" / "intel_vpu" / …
    QString runtimePath;                  // where the runtime was found ("" if missing)
    QString runtimeVersion = QString();   // resolved from the runtime, "" if unknown
    NaReason reason = NaReason::None;     // None = present and runnable
};

// Detect the primary NPU and probe its runtime against the real filesystem.
NpuDetect detect(const SystemSnapshot &snapshot);

// The test seam: as detect(), but the runtime probe resolves under `root`
// ("" = the real filesystem; tst_npu_detect points it at a QTemporaryDir).
NpuDetect detect(const SystemSnapshot &snapshot, const QString &root);

// Internal (one compiled per OS, detect_win.cpp / detect_linux.cpp): fills
// runtimePath/runtimeVersion and sets `reason` (None when the runtime is ready).
NpuDetect probeRuntime(const SystemSnapshot &snapshot, NpuVendor vendor, const QJsonObject &npu, const QString &root);

} // namespace sb::npu

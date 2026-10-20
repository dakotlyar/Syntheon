// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The memory configuration readout (Phase 3 design §7, P16, D11, D12): what
// the run's SystemSnapshot says about the memory, from SMBIOS type 17 only
// (Linux: the helper's tables, or udev's copy; Windows: GetSystemFirmwareTable),
// turned into the RAM test's metrics so the Detail page renders from the
// result alone. Never a serial number or an asset tag (the snapshot has none,
// P23). Pure.

#include "core/result.h"

#include <QString>
#include <QVector>

#include <optional>

namespace sb::ram {

struct MemoryReadout {
    bool available = false;
    // Why not (§7): "the SMBIOS helper wasn't authorised and udev has no
    // memory data", "the firmware lists no memory devices", ...
    QString reason;
    // speed, channels, config.* and config.module.N, in that order.
    QVector<Metric> metrics;
    // The theoretical peak (configured MT/s x bus width / 8) in GB/s, when
    // every populated device reports its data width and a speed (D12).
    std::optional<double> peakGBs;
    // "DDR5-6000 · 2 × 16 GB · 2 channels (SMBIOS)", for the option tab.
    QString summary;
};

// The readout of `system` (its "memory" object). `windows` words the
// reason when there is nothing (no helper on Windows).
MemoryReadout memoryReadout(const SystemSnapshot &system, bool windows);

// One config.module.N value: its fields joined by " · ", always seven of
// them, "—" for an unknown one:
//   slot · size · type · speed · ranks · maker · part number
// e.g. "DIMMA2 (P0 CHANNEL A) · 16 GB · DDR5 · 6,000 MT/s · 2R · G.Skill ·
// F5-6000J3038F16G". The Detail page's Modules table splits it back.
inline constexpr int kModuleFields = 7;
QStringList moduleFields(const QString &value);

// "16 GB", "32 GB", "1.5 GB": GiB with the SI-looking label System info
// uses, one decimal unless whole.
QString memorySizeText(double bytes);

} // namespace sb::ram

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NPU test's stable ids and metric keys (Phase 4c design §2, §9). Like
// storage/ids.h, these keys are forever: they are stored in every result. Pure
// (QtCore only).
//
// The three sub-test ids (classify / detect / llm) are new — the Phase 1 stub
// declared none (design §1.1). `namespace sb::npu` is the new lower-case metric
// namespace (the sysinfo `sb::NpuInfo` struct is untouched and never included
// here).

#include <QString>
#include <QStringView>

#include <array>
#include <optional>

namespace sb::npu {

// The three workloads/sub-tests, in run order (§2, §7).
enum class Workload { Classify, Detect, Llm };
inline constexpr std::array<Workload, 3> kWorkloads{Workload::Classify, Workload::Detect, Workload::Llm};

// The field a metric key names (§9): the headline throughputs (Imgs/Tps), the
// LLM's time-to-first-token (Ttft), and the three per-window raw numbers
// (Img0..Img2, Tps0..Tps2 — stored so the repeatability script and Detail page
// can show spread, §2.1).
enum class Field { Imgs, Tps, Ttft, Img0, Img1, Img2, Tps0, Tps1, Tps2 };

QString workloadId(Workload);       // "classify" "detect" "llm" (the sub-test id)
QString workloadLabel(Workload);    // "Classify" "Detect" "LLM"
QString key(Workload);              // the headline key: "classify.imgs" "detect.imgs" "llm.tps"
QString key(Workload, Field);       // "classify.imgs" … "llm.tps" "llm.ttft" "classify.img0" …
QString label(Workload, Field);     // "Classification throughput" … "per-window images/s (raw)"
QString unit(Workload, Field);      // "img/s", "tok/s", "ms" (canonical, §9)
bool lowerIsBetter(Workload, Field);  // only llm.ttft

std::optional<Workload> workloadFromId(QStringView);

// The vendor ids (detect + engine share the same four, design §3, §4.1).
enum class NpuVendor { Intel, Amd, Qualcomm, Null };
QString vendorId(NpuVendor);        // "intel" "amd" "qualcomm" "null" (the manifest's vendor tag)
QString vendorName(NpuVendor);      // "Intel" "AMD" "Qualcomm" "Null"
std::optional<NpuVendor> vendorFromId(QStringView);  // "null" is not a real pack vendor → nullopt

// The neutral, sub-test-less keys (§9): stored on every result, never scored.
inline const QString kDeviceVendor = QStringLiteral("device.vendor");
inline const QString kDeviceName = QStringLiteral("device.name");
inline const QString kDeviceDriver = QStringLiteral("device.driver");
inline const QString kDeviceRuntime = QStringLiteral("device.runtime");
inline const QString kEngineName = QStringLiteral("engine.name");
inline const QString kModelClassify = QStringLiteral("model.classify");
inline const QString kModelDetect = QStringLiteral("model.detect");
inline const QString kModelLlm = QStringLiteral("model.llm");
inline const QString kModelsPackVersion = QStringLiteral("models.pack_version");
inline const QString kStabilitySpread = QStringLiteral("stability.spread");
inline const QString kPowerSource = QStringLiteral("power.source");

} // namespace sb::npu

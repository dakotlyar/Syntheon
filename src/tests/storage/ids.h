// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Storage test's pattern ids and its stable metric keys (Phase 4b design
// §10). The ids and keys are forever: they are stored in every result. Pure
// (QtCore only).
//
// The six sub-test ids are the ones the Phase 1 stub already declared (design
// N8): seq1m_q8t1, seq1m_q1t1, rnd4k_q32t1, rnd4k_q1t1, latency, mixed. The
// metric keys below never rename one of them.

#include <QString>
#include <QStringView>

#include <array>
#include <optional>

namespace sb::storage {

// The six patterns, in run order (§8).
enum class Pattern { Seq1mQ8T1, Seq1mQ1T1, Rnd4kQ32T1, Rnd4kQ1T1, Latency, Mixed };
inline constexpr std::array<Pattern, 6> kPatterns{Pattern::Seq1mQ8T1, Pattern::Seq1mQ1T1, Pattern::Rnd4kQ32T1,
                                                  Pattern::Rnd4kQ1T1, Pattern::Latency, Pattern::Mixed};

// The field a metric key names (§10). A pattern only uses a subset.
enum class Field { Read, Write, ReadIops, WriteIops, Throughput, Iops, P50, P99, P999 };

QString patternId(Pattern);             // "seq1m_q8t1" … "latency" "mixed" (the sub-test id)
QString patternLabel(Pattern);          // "Seq 1M Q8T1" … "Mixed 70/30 random"
QString key(Pattern, Field);            // "seq1m_q8t1.read" … "latency.p999" "mixed.throughput"
QString label(Pattern, Field);          // "Seq 1M Q8T1 read" … "p99.9 latency" "Mixed throughput"
QString unit(Pattern, Field);           // "MB/s", "IOPS", "µs" (canonical, §10)
bool fieldIsIops(Field);                // ReadIops / WriteIops / Iops report as IOPS
bool lowerIsBetter(Pattern, Field);     // only the three latency percentiles

std::optional<Pattern> patternFromId(QStringView);
std::optional<Field> fieldFromKey(QStringView suffix);  // "read" / "write" / "read_iops" / ...

// The neutral, sub-test-less keys (§10): stored on every result, never scored.
inline const QString kModelIo = QStringLiteral("model.io");
inline const QString kTargetDrive = QStringLiteral("target.drive");
inline const QString kTargetFilesystem = QStringLiteral("target.filesystem");
inline const QString kTargetFreeBytes = QStringLiteral("target.free_bytes");
inline const QString kTargetKind = QStringLiteral("target.kind");
inline const QString kFileSize = QStringLiteral("file_size");
inline const QString kDirectAlignment = QStringLiteral("direct.alignment");
inline const QString kPrepSeconds = QStringLiteral("prep.seconds");
inline const QString kWearWriteGb = QStringLiteral("wear.write_gb");
inline const QString kStabilitySpread = QStringLiteral("stability.spread");

} // namespace sb::storage
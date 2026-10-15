// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <optional>

namespace sb {

// Outcome of one test (or sub-test). NotAvailable is expected (no NPU) and is
// never an error; Error means something broke. Neither has a score.
enum class Status { Ok, Partial, NotAvailable, Error, Cancelled, TimedOut, Interrupted, NotRun };

enum class Direction { HigherIsBetter, LowerIsBetter, Neutral };

enum class Exclusivity { Shared, RunsAlone };          // Battery = RunsAlone
enum class RecoveryMode { None, FromCheckpoints };      // Battery = FromCheckpoints

enum class RunKind { Standard, Exclusive };
enum class RunStatus { Running, Completed, Stopped, Interrupted, Recovered };

enum class LogLevel { Debug, Info, Warning, Error };

// Visual tone shared by option Info rows, notes and pills.
enum class Tone { Neutral, Good, Warn, Bad, Na };

// Stable lower-case names, used in the database and (Phase 9) in JSON.
// These strings are part of the stored format: never rename, only add.
QString toKey(Status);
QString toKey(Direction);
QString toKey(RunKind);
QString toKey(RunStatus);
QString toKey(LogLevel);
QString toKey(Tone);

std::optional<Status> statusFromKey(const QString &);
std::optional<Direction> directionFromKey(const QString &);
std::optional<RunKind> runKindFromKey(const QString &);
std::optional<RunStatus> runStatusFromKey(const QString &);
std::optional<LogLevel> logLevelFromKey(const QString &);
std::optional<Tone> toneFromKey(const QString &);

} // namespace sb

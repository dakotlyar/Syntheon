#include "core/enums.h"

#include <array>
#include <utility>

namespace sb {

namespace {

template <typename E, std::size_t N>
QString nameOf(const std::array<std::pair<E, const char *>, N> &table, E value)
{
    for (const auto &[e, name] : table) {
        if (e == value)
            return QString::fromLatin1(name);
    }
    return {};
}

template <typename E, std::size_t N>
std::optional<E> valueOf(const std::array<std::pair<E, const char *>, N> &table, const QString &s)
{
    for (const auto &[e, name] : table) {
        if (s == QLatin1StringView(name))
            return e;
    }
    return std::nullopt;
}

constexpr std::array<std::pair<Status, const char *>, 8> kStatus{{
    {Status::Ok, "ok"},
    {Status::Partial, "partial"},
    {Status::NotAvailable, "not_available"},
    {Status::Error, "error"},
    {Status::Cancelled, "cancelled"},
    {Status::TimedOut, "timed_out"},
    {Status::Interrupted, "interrupted"},
    {Status::NotRun, "not_run"},
}};

constexpr std::array<std::pair<Direction, const char *>, 3> kDirection{{
    {Direction::HigherIsBetter, "higher"},
    {Direction::LowerIsBetter, "lower"},
    {Direction::Neutral, "neutral"},
}};

constexpr std::array<std::pair<RunKind, const char *>, 2> kRunKind{{
    {RunKind::Standard, "standard"},
    {RunKind::Exclusive, "exclusive"},
}};

constexpr std::array<std::pair<RunStatus, const char *>, 5> kRunStatus{{
    {RunStatus::Running, "running"},
    {RunStatus::Completed, "completed"},
    {RunStatus::Stopped, "stopped"},
    {RunStatus::Interrupted, "interrupted"},
    {RunStatus::Recovered, "recovered"},
}};

constexpr std::array<std::pair<LogLevel, const char *>, 4> kLogLevel{{
    {LogLevel::Debug, "debug"},
    {LogLevel::Info, "info"},
    {LogLevel::Warning, "warning"},
    {LogLevel::Error, "error"},
}};

constexpr std::array<std::pair<Tone, const char *>, 5> kTone{{
    {Tone::Neutral, "neutral"},
    {Tone::Good, "good"},
    {Tone::Warn, "warn"},
    {Tone::Bad, "bad"},
    {Tone::Na, "na"},
}};

} // namespace

QString toKey(Status v) { return nameOf(kStatus, v); }
QString toKey(Direction v) { return nameOf(kDirection, v); }
QString toKey(RunKind v) { return nameOf(kRunKind, v); }
QString toKey(RunStatus v) { return nameOf(kRunStatus, v); }
QString toKey(LogLevel v) { return nameOf(kLogLevel, v); }
QString toKey(Tone v) { return nameOf(kTone, v); }

std::optional<Status> statusFromKey(const QString &s) { return valueOf(kStatus, s); }
std::optional<Direction> directionFromKey(const QString &s) { return valueOf(kDirection, s); }
std::optional<RunKind> runKindFromKey(const QString &s) { return valueOf(kRunKind, s); }
std::optional<RunStatus> runStatusFromKey(const QString &s) { return valueOf(kRunStatus, s); }
std::optional<LogLevel> logLevelFromKey(const QString &s) { return valueOf(kLogLevel, s); }
std::optional<Tone> toneFromKey(const QString &s) { return valueOf(kTone, s); }

} // namespace sb

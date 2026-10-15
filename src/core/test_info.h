// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/enums.h"

#include <QList>
#include <QMap>
#include <QString>

#include <optional>

namespace sb {

// IDs (test, sub-test, option and metric keys) are stable forever: they become
// JSON keys and PTS result keys in Phase 9. Never rename, only deprecate.

struct SubTestInfo {
    QString id;    // "int_fp", "seq1m_q8t1"
    QString name;  // "Integer + FP + primes"
    bool userSelectable = false;  // shows as a checkbox in the option tab
    bool defaultOn = true;
};

struct TestInfo {
    QString id;          // cpu|ram|storage|gpu|ai|npu|battery
    QString name;        // "CPU"
    QString summary;     // picker row description
    QString colorToken;  // "t-cpu" (theme token, not a colour)
    int testVersion = 0;     // bump when the measurement changes (0 = stub)
    int formulaVersion = 0;  // bump when the score formula changes (0 = stub)
    Exclusivity exclusivity = Exclusivity::Shared;
    RecoveryMode recovery = RecoveryMode::None;
    bool countsTowardOverall = true;
    QList<SubTestInfo> subTests;
    // privilegedOps (QList<PrivOp>) joins this struct with the PrivilegedOps
    // interface in task 9.
};

struct Hint {
    QString text;
    QString url;
    bool operator==(const Hint &) const = default;
};

struct CapabilityItem {
    QString label;  // "Vulkan 1.4"
    Tone tone = Tone::Neutral;
    bool operator==(const CapabilityItem &) const = default;
};

struct Availability {
    enum class State { Available, Partial, Unavailable };

    State state = State::Available;
    QString reason;                              // "No supported NPU found"
    std::optional<Hint> hint;                    // e.g. Phase 6 "Get the AI model pack"
    QMap<QString, QString> subTestUnavailable;   // sub-test id -> reason
    QList<CapabilityItem> capabilities;          // feeds System info + first-run

    static Availability available() { return {}; }
    static Availability unavailable(const QString &reason)
    {
        Availability a;
        a.state = State::Unavailable;
        a.reason = reason;
        return a;
    }
    bool operator==(const Availability &) const = default;
};

} // namespace sb

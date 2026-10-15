// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/enums.h"

#include <QDateTime>
#include <QString>

namespace sb {

// One live/stored run-log line (D16: saved with the run, viewable later).
struct LogLine {
    QDateTime timestamp;
    LogLevel level = LogLevel::Info;
    QString testId;  // empty for run-level lines
    QString message;

    bool operator==(const LogLine &) const = default;
};

} // namespace sb

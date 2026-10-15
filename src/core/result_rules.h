// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/result.h"

#include <QString>

#include <functional>

namespace sb {

class ITest;

// Rules every result goes through before it's stored, whether it came from
// run() (orchestrator) or recover() (startup recovery):
//  - F3 (Dave): a NaN or infinite metric makes it Status::Error, reason
//    "produced an invalid value for <metric>", with no score;
//  - only Ok/Partial results are scored, by the test's pure score() (P8);
//    sub-scores come from the breakdown, never from what run() put there;
//  - a score() that throws leaves the result unscored and calls onError.
void applyResultRules(const ITest &test, TestResult &result,
                      const std::function<void(const QString &message)> &onError);

} // namespace sb

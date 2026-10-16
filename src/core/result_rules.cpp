// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/result_rules.h"

#include "core/itest.h"

#include <cmath>
#include <exception>

namespace sb {

void applyResultRules(const ITest &test, TestResult &r, const std::function<void(const QString &)> &onError)
{
    // AP5-R2-7: the metric rule only applies to results that claim a
    // measurement (Ok/Partial). A Stopped, Cancelled, TimedOut, Interrupted
    // or NotAvailable test keeps its status.
    const bool claimsResult = r.status == Status::Ok || r.status == Status::Partial;
    if (claimsResult) {
        for (const Metric &m : std::as_const(r.metrics)) {
            if (m.isNumeric() && !std::isfinite(std::get<double>(m.value))) {
                r.status = Status::Error;
                r.statusReason =
                    QStringLiteral("produced an invalid value for %1").arg(m.label.isEmpty() ? m.key : m.label);
                break;
            }
        }
    }

    r.score.reset();
    for (SubResult &sub : r.subResults)
        sub.score.reset();
    if (r.status != Status::Ok && r.status != Status::Partial)
        return;
    try {
        const ScoreBreakdown b = test.score(r);
        // A NaN/infinite score or sub-score is the same kind of failure.
        bool invalid = b.total && !std::isfinite(*b.total);
        for (const double v : b.subScores)
            invalid = invalid || !std::isfinite(v);
        if (invalid) {
            r.status = Status::Error;
            r.statusReason = QStringLiteral("produced an invalid score");
            return;
        }
        r.score = b.total;
        for (SubResult &sub : r.subResults) {
            const auto it = b.subScores.constFind(sub.id);
            if (it != b.subScores.cend())
                sub.score = *it;
        }
    } catch (const std::exception &ex) {
        if (onError)
            onError(QStringLiteral("Scoring failed: %1").arg(QString::fromLocal8Bit(ex.what())));
    } catch (...) {
        if (onError)
            onError(QStringLiteral("Scoring failed"));
    }
}

} // namespace sb

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/npu_test.h"

#include <QJsonArray>
#include <QJsonObject>

namespace sb::tests {

namespace {

StubSpec makeSpec()
{
    StubSpec s;
    TestInfo &i = s.info;
    i.id = QStringLiteral("npu");
    i.name = QStringLiteral("NPU");
    i.summary = QStringLiteral("Neural processing unit inference");
    i.colorToken = QStringLiteral("t-npu");
    s.estimate = std::chrono::seconds(0);

    // D15: the Phase 1 NPU stub is always N/A. If a device is present, say so;
    // the real test is Phase 7.
    s.probe = [](const ProbeContext &ctx) {
        const QJsonArray npus = ctx.system().data.value(QStringLiteral("npus")).toArray();
        if (!npus.isEmpty()) {
            const QString name = npus.first().toObject().value(QStringLiteral("name")).toString();
            return Availability::unavailable(
                QStringLiteral("%1 detected; the NPU test arrives in Phase 7").arg(name.isEmpty() ? QStringLiteral("An NPU") : name));
        }
        return Availability::unavailable(QStringLiteral("No supported NPU found on this machine"));
    };
    s.detail = [](const TestResult &r) {
        DetailLayout d;
        d.subtitle = QStringLiteral("Not available on this machine");
        const QString why = r.statusReason.isEmpty() ? QStringLiteral("No supported NPU found") : r.statusReason;
        d.blocks.append({{}, NaStateBlock{QStringLiteral("No supported NPU found"),
                                          QStringLiteral("SynBench looks for Intel AI Boost, AMD XDNA (Ryzen AI) and Qualcomm "
                                                         "Hexagon. %1. The NPU test has no CPU fallback.")
                                              .arg(why)}});
        return d;
    };
    return s;
}

} // namespace

NpuTest::NpuTest() : StubTest(makeSpec()) {}

} // namespace sb::tests

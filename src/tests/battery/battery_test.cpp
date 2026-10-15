// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/battery/battery_test.h"

namespace sb::tests {

namespace {

TestInfo makeInfo()
{
    TestInfo info;
    info.id = QStringLiteral("battery");
    info.name = QStringLiteral("Battery life");
    info.summary = QStringLiteral("Runs alone, looping tests until the battery is empty");
    info.colorToken = QStringLiteral("t-bat");
    info.testVersion = 0;     // stub
    info.formulaVersion = 0;  // stub
    info.exclusivity = Exclusivity::RunsAlone;
    info.recovery = RecoveryMode::FromCheckpoints;
    // Whether Battery counts toward the overall score is decided in Phase 8;
    // the skeleton leaves it out until then.
    info.countsTowardOverall = false;
    return info;
}

} // namespace

BatteryTest::BatteryTest() : SkeletonTest(makeInfo()) {}

} // namespace sb::tests

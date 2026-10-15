// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/npu_test.h"

namespace sb::tests {

namespace {

TestInfo makeInfo()
{
    TestInfo info;
    info.id = QStringLiteral("npu");
    info.name = QStringLiteral("NPU");
    info.summary = QStringLiteral("Neural processing unit inference");
    info.colorToken = QStringLiteral("t-npu");
    info.testVersion = 0;     // stub
    info.formulaVersion = 0;  // stub
    return info;
}

} // namespace

NpuTest::NpuTest() : SkeletonTest(makeInfo()) {}

} // namespace sb::tests

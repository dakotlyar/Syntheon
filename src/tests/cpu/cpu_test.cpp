// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/cpu_test.h"

namespace sb::tests {

namespace {

TestInfo makeInfo()
{
    TestInfo info;
    info.id = QStringLiteral("cpu");
    info.name = QStringLiteral("CPU");
    info.summary = QStringLiteral("Integer, FP, SIMD, compression, encryption, sorting, physics");
    info.colorToken = QStringLiteral("t-cpu");
    info.testVersion = 0;     // stub
    info.formulaVersion = 0;  // stub
    return info;
}

} // namespace

CpuTest::CpuTest() : SkeletonTest(makeInfo()) {}

} // namespace sb::tests

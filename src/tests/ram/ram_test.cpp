#include "tests/ram/ram_test.h"

namespace sb::tests {

namespace {

TestInfo makeInfo()
{
    TestInfo info;
    info.id = QStringLiteral("ram");
    info.name = QStringLiteral("RAM");
    info.summary = QStringLiteral("Latency curve, timings, bandwidth, loaded latency");
    info.colorToken = QStringLiteral("t-ram");
    info.testVersion = 0;     // stub
    info.formulaVersion = 0;  // stub
    return info;
}

} // namespace

RamTest::RamTest() : SkeletonTest(makeInfo()) {}

} // namespace sb::tests

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/storage/storage_test.h"

namespace sb::tests {

namespace {

TestInfo makeInfo()
{
    TestInfo info;
    info.id = QStringLiteral("storage");
    info.name = QStringLiteral("Storage");
    info.summary = QStringLiteral("Sequential and random read/write, latency, mixed");
    info.colorToken = QStringLiteral("t-sto");
    info.testVersion = 0;     // stub
    info.formulaVersion = 0;  // stub
    return info;
}

} // namespace

StorageTest::StorageTest() : SkeletonTest(makeInfo()) {}

} // namespace sb::tests

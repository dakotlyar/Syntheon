// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/gpu_test.h"

namespace sb::tests {

namespace {

TestInfo makeInfo()
{
    TestInfo info;
    info.id = QStringLiteral("gpu");
    info.name = QStringLiteral("GPU");
    info.summary = QStringLiteral("2D and 3D on Vulkan, DirectX 12, DirectX 11, OpenGL");
    info.colorToken = QStringLiteral("t-gpu");
    info.testVersion = 0;     // stub
    info.formulaVersion = 0;  // stub
    return info;
}

} // namespace

GpuTest::GpuTest() : SkeletonTest(makeInfo()) {}

} // namespace sb::tests

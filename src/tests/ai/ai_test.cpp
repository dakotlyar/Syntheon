// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ai/ai_test.h"

namespace sb::tests {

namespace {

TestInfo makeInfo()
{
    TestInfo info;
    info.id = QStringLiteral("ai");
    info.name = QStringLiteral("AI inference");
    info.summary = QStringLiteral("LLM, vision, image generation, speech");
    info.colorToken = QStringLiteral("t-ai");
    info.testVersion = 0;     // stub
    info.formulaVersion = 0;  // stub
    return info;
}

} // namespace

AiTest::AiTest() : SkeletonTest(makeInfo()) {}

} // namespace sb::tests

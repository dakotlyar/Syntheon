// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tests/common/stub_test.h"

namespace sb::tests {

// Phase 1 stub (D15). Phase 2 replaces this folder with the real test.
class CpuTest : public StubTest
{
public:
    CpuTest();
};

} // namespace sb::tests

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tests/common/stub_test.h"

namespace sb::tests {

// Phase 1 stub (D15). Phase 5 replaces this folder with the real test.
class GpuTest : public StubTest
{
public:
    GpuTest();
};

} // namespace sb::tests

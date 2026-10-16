// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tests/common/stub_test.h"

namespace sb::tests {

// Phase 1 stub (D15). Phase 7 replaces this folder with the real test.
class NpuTest : public StubTest
{
public:
    NpuTest();
};

} // namespace sb::tests

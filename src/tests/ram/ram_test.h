// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tests/common/stub_test.h"

namespace sb::tests {

// Phase 1 stub (D15). Phase 3 replaces this folder with the real test.
class RamTest : public StubTest
{
public:
    RamTest();
};

} // namespace sb::tests

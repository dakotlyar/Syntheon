// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sb {

class Registry;

// Adds every built-in test to `registry`, in canonical order. Defined in the
// builtin_tests.cpp that CMake generates from SYNBENCH_TESTS.
void registerBuiltinTests(Registry &registry);

} // namespace sb

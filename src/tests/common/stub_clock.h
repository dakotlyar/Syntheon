// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sb::tests {

// Phase 1 stubs take about 10-30 s each (P20). SYNBENCH_STUB_SCALE (a factor,
// e.g. 0.05) shortens that for tests; SYNBENCH_SMOKE=1 implies 0.03. The real
// tests of later phases don't use this.
double stubTimeScale();
void setStubTimeScale(double scale);  // tests

// A simulated hang (Simulate = Hang) ignores Stop on purpose. It ends when
// released (tests, so the stuck thread can be joined) or after 15 minutes.
void releaseStubHangs();
unsigned stubHangGeneration();
// Blocks like a hung test: no cancel check, until released or 15 minutes.
void stubHang();

} // namespace sb::tests

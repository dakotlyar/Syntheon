// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>

#include <optional>

class QString;

namespace sb::tests {

// A SYNBENCH_STUB_SCALE value: a plain finite number in (0, 100], else nothing.
std::optional<double> parseStubScale(const QString &text);

// Phase 1 stubs take about 10-30 s each (P20). SYNBENCH_STUB_SCALE (a factor,
// e.g. 0.05) shortens that for tests; SYNBENCH_SMOKE=1 implies 0.03. The real
// tests of later phases don't use this.
double stubTimeScale();
void setStubTimeScale(double scale);  // tests

// The time limit a stub gets is max(20 s, 2 x its estimate) x the time scale,
// but never less than this floor when the scale is below 1. Real scheduler
// and sleep overhead doesn't shrink with the scale: on a loaded or slow CI
// runner (Windows sleeps in ~16 ms ticks) a 0.7 s stub can take seconds, and
// that must never look like a timeout. Default 15 s; the tests that exercise
// the timeout itself lower it.
std::chrono::seconds stubTimeLimitFloor();
void setStubTimeLimitFloor(std::chrono::seconds floor);
void resetStubTimeLimitFloor();

// A simulated hang (Simulate = Hang) ignores Stop on purpose. It ends when
// released (tests, so the stuck thread can be joined) or after 15 minutes.
void releaseStubHangs();
unsigned stubHangGeneration();
// Blocks like a hung test: no cancel check, until released or 15 minutes.
void stubHang();

} // namespace sb::tests

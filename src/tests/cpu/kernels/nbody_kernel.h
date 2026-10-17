// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The physics kernel (Phase 2 design §5.7, P13): `physics.nbody`, in pair
// interactions per second. N = 4096 bodies, FP64, softened gravity
// (eps^2 = 10^-3, G = 1, masses 1/N), leapfrog with a fixed dt.
//
// One unit is an episode of 8 steps from the fixed initial state, so every
// episode ends in the same state, whose hash (of the positions' bits) is the
// golden value. All the pool's threads work on the same episode
// (Kernel::collectiveThreads): in each step they take chunks of 64 bodies
// from an atomic counter; for each body i the accelerations from every j
// are summed in j order, then its velocity and its next position (written
// to the other of two position buffers) follow; then a barrier. Each body's
// arithmetic is serial in j and the same whoever does it, so the result is
// bit-identical for any thread count, and on every platform (P7: only + - x
// / and sqrt, no contraction, no auto-vectorisation).
//
// The barrier's completion (one thread, while the others wait) resets the
// chunk counter, swaps the buffers, decides for all threads whether a Stop
// ends the episode early (so they never disagree on how many barriers come),
// and after the 8th step hashes the positions and resets the state for the
// next episode.

#include "tests/cpu/engine/kernel.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace sb::cpu {

inline constexpr int kBodies = 4096;
inline constexpr int kNbodySteps = 8;
inline constexpr int kNbodyChunk = 64;
inline constexpr double kNbodyInteractionsPerEpisode = double(kBodies) * kBodies * kNbodySteps;

// Structure of arrays, kBodies each.
struct NbodyState {
    std::vector<double> x, y, z, vx, vy, vz;
};
NbodyState nbodyInitialState();

// Advances bodies [begin, end) by one step: accelerations from the positions
// (fx, fy, fz), velocities (vx, vy, vz) updated in place, the new positions
// into (tx, ty, tz).
void nbodyStepBodies(int begin, int end, const double *fx, const double *fy, const double *fz, double *tx, double *ty,
                     double *tz, double *vx, double *vy, double *vz);
// The hash of a state's positions (the bits of every coordinate).
std::uint64_t nbodyHash(const double *x, const double *y, const double *z);
// One episode on this thread alone, from the initial state: its hash (the
// reference for golden/nbody.inc and the thread-count tests).
std::uint64_t nbodyReferenceEpisode();

class NbodyKernel : public Kernel
{
public:
    // For a pool of exactly `threads` threads.
    explicit NbodyKernel(int threads);
    ~NbodyKernel() override;
    std::string name() const override { return "Physics (n-body)"; }
    double workPerUnit() const override { return kNbodyInteractionsPerEpisode; }
    // P14: one interaction per cycle per thread (it does about 0.2: each has
    // a square root and a division).
    double maxWorkPerCyclePerThread() const override { return 1; }
    int collectiveThreads() const override { return m_threads; }
    std::unique_ptr<KernelThread> makeThread(int thread) const override;
    std::optional<std::uint64_t> golden(std::uint64_t seed) const override;

private:
    struct Shared;
    int m_threads;
    std::unique_ptr<Shared> m_shared;
};

} // namespace sb::cpu

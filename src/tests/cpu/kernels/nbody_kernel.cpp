// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/cpu/kernels/nbody_kernel.h"

#include "tests/cpu/engine/opaque.h"
#include "tests/cpu/golden/golden.h"
#include "tests/cpu/kernels/kernel_common.h"
#include "tests/cpu/kernels/prng.h"

#include <atomic>
#include <barrier>
#include <bit>
#include <cmath>
#include <cstddef>

SB_KERNEL_TU

namespace sb::cpu {

namespace {

constexpr double kEps2 = 1e-3;
constexpr double kMass = 1.0 / kBodies;  // 2^-12, exact
constexpr double kDt = 1e-3;
constexpr int kChunks = kBodies / kNbodyChunk;

// A value in [0, 1) from 20 random bits: exact.
double unitValue(Xoshiro256ss &rng)
{
    const std::uint64_t bits = rng.below(1u << 20);
    return double(bits) * 0x1p-20;
}

} // namespace

NbodyState nbodyInitialState()
{
    NbodyState s;
    for (std::vector<double> *v : {&s.x, &s.y, &s.z, &s.vx, &s.vy, &s.vz})
        v->resize(kBodies);
    Xoshiro256ss rng(0x4E424F4459535441ull);  // "NBODYSTA"; one draw per statement
    for (int i = 0; i < kBodies; ++i) {
        s.x[std::size_t(i)] = unitValue(rng);
        s.y[std::size_t(i)] = unitValue(rng);
        s.z[std::size_t(i)] = unitValue(rng);
        s.vx[std::size_t(i)] = (unitValue(rng) - 0.5) * 0.25;
        s.vy[std::size_t(i)] = (unitValue(rng) - 0.5) * 0.25;
        s.vz[std::size_t(i)] = (unitValue(rng) - 0.5) * 0.25;
    }
    return s;
}

SB_NOINLINE void nbodyStepBodies(int begin, int end, const double *fx, const double *fy, const double *fz, double *tx,
                                 double *ty, double *tz, double *vx, double *vy, double *vz)
{
    opaque(fx);
    SB_NO_VECTOR
    for (int i = begin; i < end; ++i) {
        const double xi = fx[i], yi = fy[i], zi = fz[i];
        double ax = 0, ay = 0, az = 0;
        SB_NO_VECTOR
        for (int j = 0; j < kBodies; ++j) {
            const double dx = fx[j] - xi;
            const double dy = fy[j] - yi;
            const double dz = fz[j] - zi;
            const double r2 = dx * dx + dy * dy + dz * dz + kEps2;
            const double inv = 1.0 / std::sqrt(r2);
            const double s = kMass * (inv * inv * inv);
            ax += dx * s;
            ay += dy * s;
            az += dz * s;
        }
        vx[i] += ax * kDt;
        vy[i] += ay * kDt;
        vz[i] += az * kDt;
        tx[i] = xi + vx[i] * kDt;
        ty[i] = yi + vy[i] * kDt;
        tz[i] = zi + vz[i] * kDt;
    }
}

std::uint64_t nbodyHash(const double *x, const double *y, const double *z)
{
    std::uint64_t h = 0;
    for (int i = 0; i < kBodies; ++i) {
        h = foldChecksum(h, std::bit_cast<std::uint64_t>(x[i]));
        h = foldChecksum(h, std::bit_cast<std::uint64_t>(y[i]));
        h = foldChecksum(h, std::bit_cast<std::uint64_t>(z[i]));
    }
    return h;
}

std::uint64_t nbodyReferenceEpisode()
{
    NbodyState s = nbodyInitialState();
    std::vector<double> nx(kBodies), ny(kBodies), nz(kBodies);
    for (int step = 0; step < kNbodySteps; ++step) {
        nbodyStepBodies(0, kBodies, s.x.data(), s.y.data(), s.z.data(), nx.data(), ny.data(), nz.data(), s.vx.data(),
                        s.vy.data(), s.vz.data());
        s.x.swap(nx);
        s.y.swap(ny);
        s.z.swap(nz);
    }
    return nbodyHash(s.x.data(), s.y.data(), s.z.data());
}

struct NbodyKernel::Shared {
    struct StepDone {
        Shared *s;
        void operator()() noexcept { s->stepDone(); }
    };

    NbodyState initial = nbodyInitialState();
    std::vector<double> px[2], py[2], pz[2], vx, vy, vz;
    std::atomic<int> nextChunk{0};
    // Written only by the barrier's completion, read by the threads after it.
    int cur = 0;
    int step = 0;
    bool abort = false;
    std::uint64_t hash = 0;
    // The engine's cancel flag (the same for every thread's KernelThread).
    std::atomic<const std::atomic<bool> *> cancel{nullptr};
    std::barrier<StepDone> barrier;

    explicit Shared(int threads) : barrier(threads, StepDone{this})
    {
        for (int b = 0; b < 2; ++b) {
            px[b].resize(kBodies);
            py[b].resize(kBodies);
            pz[b].resize(kBodies);
        }
        reset();
    }

    // The initial state in the current buffer.
    void reset()
    {
        px[cur] = initial.x;
        py[cur] = initial.y;
        pz[cur] = initial.z;
        vx = initial.vx;
        vy = initial.vy;
        vz = initial.vz;
        step = 0;
    }

    void stepDone()
    {
        nextChunk.store(0, std::memory_order_relaxed);
        cur ^= 1;
        const std::atomic<bool> *c = cancel.load();
        abort = c && c->load();
        if (abort) {
            reset();  // the episode is abandoned
        } else if (++step == kNbodySteps) {
            hash = nbodyHash(px[cur].data(), py[cur].data(), pz[cur].data());
            reset();
        }
    }
};

NbodyKernel::NbodyKernel(int threads) : m_threads(threads), m_shared(std::make_unique<Shared>(threads)) {}

NbodyKernel::~NbodyKernel() = default;

std::unique_ptr<KernelThread> NbodyKernel::makeThread(int) const
{
    struct T : KernelThread {
        Shared *s;
        std::uint64_t unit(std::uint64_t) override
        {
            s->cancel.store(cancelFlag());
            for (int k = 0; k < kNbodySteps; ++k) {
                const int c = s->cur;
                for (;;) {
                    const int chunk = s->nextChunk.fetch_add(1, std::memory_order_relaxed);
                    if (chunk >= kChunks)
                        break;
                    nbodyStepBodies(chunk * kNbodyChunk, (chunk + 1) * kNbodyChunk, s->px[c].data(), s->py[c].data(),
                                    s->pz[c].data(), s->px[c ^ 1].data(), s->py[c ^ 1].data(), s->pz[c ^ 1].data(),
                                    s->vx.data(), s->vy.data(), s->vz.data());
                }
                s->barrier.arrive_and_wait();
                if (s->abort)
                    return 0;  // cancelled: discarded by the engine
            }
            return s->hash;
        }
    };
    auto t = std::make_unique<T>();
    t->s = m_shared.get();
    return t;
}

std::optional<std::uint64_t> NbodyKernel::golden(std::uint64_t) const
{
    return golden::kNbody;
}

} // namespace sb::cpu

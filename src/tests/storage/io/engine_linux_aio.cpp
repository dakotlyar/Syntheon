// SPDX-License-Identifier: GPL-3.0-or-later
// The libaio engine (Phase 4b design §4.1, D7): the io_uring fallback on
// Linux when liburing headers/libs are absent. One io_context with a bounded
// window (never more than maxInFlight in flight), io_submit + io_getevents
// reap with the CancelFn polled on each 50 ms timeout (P-STOP), and per-op
// read latency. The completed iocb is recovered from io_event.obj (pointer
// arithmetic into the slot pool), so no per-op heap boilerplate.
#include "tests/storage/io/engine.h"

#include <libaio.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace sb::storage {

namespace {

constexpr std::size_t kDepth = 32;   // the max pattern queue depth (Q32)

class AioEngine final : public IStorageIoEngine
{
public:
    explicit AioEngine(std::size_t depth)
        : m_depth(depth)
        , m_iocbs(depth)
        , m_free(depth, true)
        , m_viewFor(depth, std::size_t(-1))
    {
        m_ready = (::io_setup(static_cast<unsigned>(depth), &m_ctx) >= 0);
    }

    ~AioEngine() override
    {
        if (m_ready)
            ::io_destroy(m_ctx);
    }

    QString backendName() const override { return QStringLiteral("libaio"); }

    std::size_t maxInFlight() const override { return m_depth; }

    bool runBatch(const IoView *views, std::size_t count, IoOp op,
                  int fd, IoStats &out, const CancelFn &cancel,
                  QString *error) override
    {
        if (!m_ready) {
            if (error)
                *error = QStringLiteral("libaio: io_setup failed");
            return false;
        }

        const bool isRead = (op == IoOp::Read);
        std::vector<std::chrono::steady_clock::time_point> started(count);
        std::vector<std::size_t> slotOf(count, std::size_t(-1));
        std::size_t nextView = 0;    // next view to prepare
        std::size_t submitted = 0;   // iocbs the kernel has accepted
        std::size_t completed = 0;  // events reaped
        QString why;

        auto now = [] { return std::chrono::steady_clock::now(); };

        auto submitWindow = [&] {
            if (!why.isEmpty())
                return;
            std::vector<struct iocb *> toSubmit(m_depth);
            std::vector<std::size_t> viewOf(m_depth);
            std::size_t nsub = 0;
            while (nextView < count && submitted - completed < m_depth) {
                if (cancel())
                    break;
                std::size_t slot = std::size_t(-1);
                for (std::size_t s = 0; s < m_depth; ++s) {
                    if (m_free[s]) {
                        slot = s;
                        break;
                    }
                }
                if (slot == std::size_t(-1))
                    break;
                const IoView &v = views[nextView];
                m_free[slot] = false;
                m_viewFor[slot] = nextView;
                slotOf[nextView] = slot;
                struct iocb *cb = &m_iocbs[slot];
                std::memset(cb, 0, sizeof(*cb));
                if (isRead)
                    ::io_prep_pread(cb, fd, v.data, v.length, static_cast<long long>(v.offset));
                else
                    ::io_prep_pwrite(cb, fd, v.data, v.length, static_cast<long long>(v.offset));
                started[nextView] = now();
                toSubmit[nsub] = cb;
                viewOf[nsub] = nextView;
                ++nsub;
                ++nextView;
            }
            if (nsub == 0)
                return;
            const long rc = ::io_submit(m_ctx, static_cast<long>(nsub), toSubmit.data());
            std::size_t accepted = 0;
            if (rc < 0) {
                if (why.isEmpty())
                    why = QStringLiteral("libaio io_submit: %1").arg(QString::fromLatin1(std::strerror(-rc)));
            } else {
                accepted = std::size_t(rc);
            }
            // Roll back the slots of any not-accepted iocb (a partial submit):
            // their views stay pending and are re-prepared on the next window.
            for (std::size_t i = accepted; i < nsub; ++i) {
                const std::size_t view = viewOf[i];
                m_free[slotOf[view]] = true;
                m_viewFor[slotOf[view]] = std::size_t(-1);
                --nextView;   // re-queue this view
            }
            submitted += accepted;
        };

        submitWindow();
        std::vector<struct io_event> events(m_depth);
        while (completed < submitted) {
            struct timespec timeout{};
            timeout.tv_nsec = 50 * 1000 * 1000;   // 50 ms (P-STOP)
            const long n = ::io_getevents(m_ctx, 1, static_cast<long>(m_depth), events.data(), &timeout);
            if (n < 0) {
                if (n == -EINTR)
                    continue;   // interrupted: retry
                if (why.isEmpty())
                    why = QStringLiteral("libaio io_getevents: %1").arg(QString::fromLatin1(std::strerror(-n)));
                break;   // the context no longer reaps reliably
            }
            if (n == 0) {
                submitWindow();   // timeout: poll cancel and top the batch up
                continue;
            }
            // Process every dequeued event — even after one has failed, so the
            // remaining in-flight ops are never left touching freed buffers.
            for (long i = 0; i < n; ++i) {
                const struct iocb *obj = events[std::size_t(i)].obj;
                const std::size_t slot = static_cast<std::size_t>(obj - m_iocbs.data());
                const std::size_t idx = m_viewFor[slot];
                const long res = static_cast<long>(events[std::size_t(i)].res);
                m_free[slot] = true;
                m_viewFor[slot] = std::size_t(-1);
                ++completed;
                if (res < 0) {
                    if (why.isEmpty())
                        why = QStringLiteral("libaio op failed: %1").arg(QString::fromLatin1(std::strerror(-res)));
                    continue;
                }
                if (static_cast<std::uint64_t>(res) != views[idx].length) {
                    if (why.isEmpty())
                        why = QStringLiteral("libaio short op: %1 of %2 bytes").arg(res).arg(views[idx].length);
                    continue;
                }
                out.ops += 1;
                out.bytes += static_cast<std::uint64_t>(res);
                if (isRead)
                    out.latenciesUs.push_back(
                        std::chrono::duration<double, std::micro>(now() - started[idx]).count());
            }
            submitWindow();
        }

        if (!why.isEmpty()) {
            if (error)
                *error = why;
            return false;
        }
        return true;
    }

private:
    io_context_t m_ctx = 0;
    std::size_t m_depth = 0;
    bool m_ready = false;
    std::vector<struct iocb> m_iocbs;        // one slot per in-flight op
    std::vector<bool> m_free;                // slot is reusable
    std::vector<std::size_t> m_viewFor;      // slot -> view index
};

} // namespace

std::unique_ptr<IStorageIoEngine> makeAioEngine()
{
    return std::make_unique<AioEngine>(kDepth);
}

} // namespace sb::storage

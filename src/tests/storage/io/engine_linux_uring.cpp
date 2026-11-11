// SPDX-License-Identifier: GPL-3.0-or-later
// The io_uring engine (Phase 4b design §4.1): Linux x64, kernel >= 5.1, the
// preferred async backend (D7). One ring (io_uring_queue_init), a bounded
// window of SQEs (never more than maxInFlight() in flight), io_uring_submit +
// an io_uring_wait_cqe_timeout reap loop with the CancelFn polled on every
// 50 ms timeout (P-STOP), and per-op read latency.
#include "tests/storage/io/engine.h"

#include <liburing.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace sb::storage {

namespace {

constexpr std::size_t kDepth = 32;   // the max pattern queue depth (Q32)

class UringEngine final : public IStorageIoEngine
{
public:
    explicit UringEngine(std::size_t depth)
        : m_depth(depth)
        , m_ready(::io_uring_queue_init(static_cast<unsigned>(depth), &m_ring, 0) >= 0)
    {
    }

    ~UringEngine() override
    {
        if (m_ready)
            ::io_uring_queue_exit(&m_ring);
    }

    QString backendName() const override { return QStringLiteral("io_uring"); }

    std::size_t maxInFlight() const override { return m_depth; }

    bool runBatch(const IoView *views, std::size_t count, IoOp op,
                  int fd, IoStats &out, const CancelFn &cancel,
                  QString *error) override
    {
        if (!m_ready) {
            if (error)
                *error = QStringLiteral("io_uring: ring initialisation failed");
            return false;
        }

        const bool isRead = (op == IoOp::Read);
        std::vector<std::chrono::steady_clock::time_point> started(count);
        std::size_t submitted = 0;   // views handed to the ring so far
        std::size_t completed = 0;   // CQEs reaped so far
        QString why;

        auto now = [] { return std::chrono::steady_clock::now(); };

        // Fill the ring up to the in-flight cap, then io_uring_submit once.
        auto submitWindow = [&] {
            bool any = false;
            while (submitted < count && submitted - completed < m_depth) {
                if (cancel())
                    break;   // stop submitting; the in-flight ops drain below
                struct io_uring_sqe *sqe = ::io_uring_get_sqe(&m_ring);
                if (!sqe)
                    break;
                const IoView &v = views[submitted];
                started[submitted] = now();
                if (isRead)
                    ::io_uring_prep_read(sqe, fd, v.data, static_cast<unsigned>(v.length), v.offset);
                else
                    ::io_uring_prep_write(sqe, fd, v.data, static_cast<unsigned>(v.length), v.offset);
                // The raw user_data field, not io_uring_sqe_set_data64: that
                // helper arrived after liburing 2.1 (Ubuntu 22.04).
                sqe->user_data = static_cast<std::uint64_t>(submitted);
                ++submitted;
                any = true;
            }
            if (any) {
                const int rc = ::io_uring_submit(&m_ring);
                if (rc < 0)
                    why = QStringLiteral("io_uring_submit: %1").arg(QString::fromLatin1(std::strerror(-rc)));
            }
        };

        submitWindow();
        while (why.isEmpty() && completed < submitted) {
            struct __kernel_timespec ts{};
            ts.tv_nsec = 50 * 1000 * 1000;   // 50 ms (P-STOP)
            struct io_uring_cqe *cqe = nullptr;
            const int rc = ::io_uring_wait_cqe_timeout(&m_ring, &cqe, &ts);
            if (rc == -ETIME) {
                submitWindow();   // polls cancel and tops the ring back up
                continue;
            }
            if (rc < 0) {
                why = QStringLiteral("io_uring_wait: %1").arg(QString::fromLatin1(std::strerror(-rc)));
                break;
            }
            // Reap every ready CQE (a batch may complete several at once).
            for (;;) {
                if (!cqe) {
                    const int p = ::io_uring_peek_cqe(&m_ring, &cqe);
                    if (p == -EAGAIN)
                        break;
                    if (p < 0) {
                        why = QStringLiteral("io_uring_peek: %1").arg(QString::fromLatin1(std::strerror(-p)));
                        break;
                    }
                }
                const std::size_t idx = static_cast<std::size_t>(cqe->user_data);
                const int res = cqe->res;
                ::io_uring_cqe_seen(&m_ring, cqe);
                cqe = nullptr;
                if (res < 0) {
                    why = QStringLiteral("io_uring op failed: %1").arg(QString::fromLatin1(std::strerror(-res)));
                    break;
                }
                out.ops += 1;
                out.bytes += static_cast<std::uint64_t>(res);
                if (isRead)
                    out.latenciesUs.push_back(
                        std::chrono::duration<double, std::micro>(now() - started[idx]).count());
                ++completed;
            }
            if (why.isEmpty())
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
    struct io_uring m_ring{};
    std::size_t m_depth = 0;
    bool m_ready = false;
};

} // namespace

std::unique_ptr<IStorageIoEngine> makeUringEngine()
{
    return std::make_unique<UringEngine>(kDepth);
}

} // namespace sb::storage

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
        std::size_t prepared = 0;   // SQEs filled into the ring (their view index)
        std::size_t submitted = 0; // SQEs the kernel has accepted
        std::size_t completed = 0; // CQEs reaped
        QString why;               // the first error, recorded while still draining

        auto now = [] { return std::chrono::steady_clock::now(); };

        // One CQE's result. Always counted as completed — success OR failure —
        // so an error still drains the rest instead of leaving kernel ops in
        // flight against buffers the caller is free to release.
        auto reapOne = [&](struct io_uring_cqe *cqe) {
            const std::size_t idx = static_cast<std::size_t>(cqe->user_data);
            const int res = cqe->res;
            ::io_uring_cqe_seen(&m_ring, cqe);
            ++completed;
            if (res < 0) {
                if (why.isEmpty())
                    why = QStringLiteral("io_uring op failed: %1")
                              .arg(QString::fromLatin1(std::strerror(-res)));
                return;
            }
            if (static_cast<std::uint64_t>(res) != views[idx].length) {
                if (why.isEmpty())
                    why = QStringLiteral("io_uring short op: %1 of %2 bytes")
                              .arg(res).arg(views[idx].length);
                return;
            }
            out.ops += 1;
            out.bytes += static_cast<std::uint64_t>(res);
            if (isRead)
                out.latenciesUs.push_back(
                    std::chrono::duration<double, std::micro>(now() - started[idx]).count());
        };

        // Fill free SQEs (bounded by the in-flight caps), then hand them to the
        // kernel, resubmitting until no prepared SQE is left (a partial
        // io_uring_submit returns the count it actually took).
        auto kick = [&] {
            while (prepared < count
                   && prepared - completed < m_depth) {   // true in-flight: every prepared SQE is submitted
                if (cancel())
                    break;
                if (!why.isEmpty())
                    break;   // an error: stop submitting; drain what's in flight
                struct io_uring_sqe *sqe = ::io_uring_get_sqe(&m_ring);
                if (!sqe)
                    break;
                const IoView &v = views[prepared];
                if (isRead)
                    ::io_uring_prep_read(sqe, fd, v.data, static_cast<unsigned>(v.length), v.offset);
                else
                    ::io_uring_prep_write(sqe, fd, v.data, static_cast<unsigned>(v.length), v.offset);
                // The raw user_data field, not io_uring_sqe_set_data64: that
                // helper arrived after liburing 2.1 (Ubuntu 22.04).
                sqe->user_data = static_cast<std::uint64_t>(prepared);
                started[prepared] = now();
                ++prepared;
            }
            while (prepared > submitted) {
                const int rc = ::io_uring_submit(&m_ring);
                if (rc == -EINTR)
                    continue;   // interrupted: retry, don't treat as an error
                if (rc < 0) {
                    if (why.isEmpty())
                        why = QStringLiteral("io_uring_submit: %1")
                                  .arg(QString::fromLatin1(std::strerror(-rc)));
                    break;   // SQEs stay prepared; reaped on the next kick
                }
                if (rc == 0) {
                    // Prepared SQEs are pending but the kernel took none: this
                    // must fail loudly, or the drain loop below (completed ==
                    // submitted) would return success with ops < count and
                    // unsubmitted SQEs still pointing at caller buffers.
                    if (why.isEmpty())
                        why = QStringLiteral("io_uring_submit: accepted 0 of %1 pending SQEs")
                                  .arg(prepared - submitted);
                    break;
                }
                submitted += static_cast<std::size_t>(rc);
            }
        };

        kick();
        while (completed < submitted) {
            struct __kernel_timespec ts{};
            ts.tv_nsec = 50 * 1000 * 1000;   // 50 ms (P-STOP)
            struct io_uring_cqe *cqe = nullptr;
            const int rc = ::io_uring_wait_cqe_timeout(&m_ring, &cqe, &ts);
            if (rc == -ETIME) {
                kick();
                continue;
            }
            if (rc == -EINTR)
                continue;   // interrupted: retry
            if (rc < 0) {
                if (why.isEmpty())
                    why = QStringLiteral("io_uring_wait: %1")
                              .arg(QString::fromLatin1(std::strerror(-rc)));
                break;   // the ring no longer reaps reliably
            }
            // Reap every CQE already available (a batch can complete several).
            for (;;) {
                if (!cqe) {
                    const int p = ::io_uring_peek_cqe(&m_ring, &cqe);
                    if (p == -EAGAIN)
                        break;
                    if (p < 0) {
                        if (why.isEmpty())
                            why = QStringLiteral("io_uring_peek: %1")
                                      .arg(QString::fromLatin1(std::strerror(-p)));
                        cqe = nullptr;
                        break;
                    }
                }
                reapOne(cqe);
                cqe = nullptr;
            }
            kick();
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

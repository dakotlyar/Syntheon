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
        std::vector<struct iocb *> toSubmit(m_depth);
        std::size_t submitted = 0;   // views handed to the kernel so far
        std::size_t completed = 0;   // events reaped so far
        QString why;

        auto now = [] { return std::chrono::steady_clock::now(); };

        // Prepare up to the in-flight cap into free slots and io_submit them.
        auto submitWindow = [&] {
            if (!why.isEmpty())
                return;
            std::size_t nsub = 0;
            while (submitted < count && submitted - completed < m_depth) {
                if (cancel())
                    break;   // stop submitting; the in-flight ops drain below
                std::size_t slot = std::size_t(-1);
                for (std::size_t s = 0; s < m_depth; ++s) {
                    if (m_free[s]) {
                        slot = s;
                        break;
                    }
                }
                if (slot == std::size_t(-1))
                    break;
                const IoView &v = views[submitted];
                m_free[slot] = false;
                m_viewFor[slot] = submitted;
                slotOf[submitted] = slot;
                struct iocb *cb = &m_iocbs[slot];
                std::memset(cb, 0, sizeof(*cb));
                if (isRead)
                    ::io_prep_pread(cb, fd, v.data, v.length, static_cast<long long>(v.offset));
                else
                    ::io_prep_pwrite(cb, fd, v.data, v.length, static_cast<long long>(v.offset));
                started[submitted] = now();
                toSubmit[nsub++] = cb;
                ++submitted;
            }
            if (nsub) {
                const long rc = ::io_submit(m_ctx, static_cast<long>(nsub), toSubmit.data());
                if (rc < 0)
                    why = QStringLiteral("libaio io_submit: %1").arg(QString::fromLatin1(std::strerror(-rc)));
                else if (rc != static_cast<long>(nsub))
                    why = QStringLiteral("libaio io_submit: partial submit (%1 of %2)")
                              .arg(rc).arg(nsub);
            }
        };

        submitWindow();
        std::vector<struct io_event> events(m_depth);
        while (why.isEmpty() && completed < submitted) {
            struct timespec timeout{};
            timeout.tv_nsec = 50 * 1000 * 1000;   // 50 ms (P-STOP)
            const long n = ::io_getevents(m_ctx, 1, static_cast<long>(m_depth), events.data(), &timeout);
            if (n < 0) {
                why = QStringLiteral("libaio io_getevents: %1").arg(QString::fromLatin1(std::strerror(-n)));
                break;
            }
            if (n == 0) {
                submitWindow();   // timeout: poll cancel and top the batch up
                continue;
            }
            for (long i = 0; i < n; ++i) {
                const struct iocb *obj = events[std::size_t(i)].obj;
                const std::size_t slot = static_cast<std::size_t>(obj - m_iocbs.data());
                const std::size_t idx = m_viewFor[slot];
                const long res = static_cast<long>(events[std::size_t(i)].res);
                m_free[slot] = true;
                m_viewFor[slot] = std::size_t(-1);
                if (res < 0) {
                    why = QStringLiteral("libaio op failed: %1").arg(QString::fromLatin1(std::strerror(-res)));
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/emit.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace sb::gpu {

namespace {
std::mutex g_mutex;

void write(const protocol::Event &e)
{
    const QByteArray line = protocol::encode(e) + '\n';
    std::fwrite(line.constData(), 1, std::size_t(line.size()), stdout);
    std::fflush(stdout);
}
} // namespace

void emitLine(const protocol::Event &e)
{
    const std::lock_guard lock(g_mutex);
    write(e);
}

void emitLineAndExit(const protocol::Event &e, int code)
{
    g_mutex.lock();  // never released: the process ends here
    write(e);
    std::fflush(stderr);
    std::_Exit(code);
}

} // namespace sb::gpu

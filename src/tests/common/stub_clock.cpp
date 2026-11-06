// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/common/stub_clock.h"

#include <QString>
#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <cmath>

namespace sb::tests {

std::optional<double> parseStubScale(const QString &text)
{
    bool ok = false;
    const double s = text.trimmed().toDouble(&ok);
    if (ok && std::isfinite(s) && s > 0 && s <= 100)
        return s;
    return std::nullopt;
}

namespace {

std::atomic<double> &override()
{
    static std::atomic<double> value{-1.0};
    return value;
}

// The default scale: real time. Developer hooks only: SYNTHEON_STUB_SCALE (a
// plain finite number in (0, 100]; anything else, including inf, NaN, huge or
// negative, is ignored) and SYNTHEON_SMOKE (0.03). A packaged build compiles
// these out.
double fromEnvironment()
{
#ifdef SYNTHEON_DEV_HOOKS
    static const double v = [] {
        if (const auto s = parseStubScale(qEnvironmentVariable("SYNTHEON_STUB_SCALE")))
            return *s;
        return qEnvironmentVariableIntValue("SYNTHEON_SMOKE") != 0 ? 0.03 : 1.0;
    }();
    return v;
#else
    return 1.0;
#endif
}

} // namespace

double stubTimeScale()
{
    const double o = override().load();
    return o > 0 ? o : fromEnvironment();
}

void setStubTimeScale(double scale)
{
    override() = scale;
}

static std::atomic<unsigned> &hangGeneration()
{
    static std::atomic<unsigned> g{0};
    return g;
}

static std::atomic<long long> &limitFloor()
{
    static std::atomic<long long> f{-1};  // -1 = the default
    return f;
}

std::chrono::seconds stubTimeLimitFloor()
{
    const long long f = limitFloor().load();
    return std::chrono::seconds(f >= 0 ? f : 15);
}

void setStubTimeLimitFloor(std::chrono::seconds floor)
{
    limitFloor() = floor.count();
}

void resetStubTimeLimitFloor()
{
    limitFloor() = -1;
}

void releaseStubHangs()
{
    hangGeneration()++;
}

unsigned stubHangGeneration()
{
    return hangGeneration().load();
}

void stubHang()
{
    const unsigned generation = stubHangGeneration();
    for (int i = 0; i < 15 * 60 * 50 && stubHangGeneration() == generation; ++i)
        QThread::msleep(20);
}

} // namespace sb::tests

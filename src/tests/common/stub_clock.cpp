// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/common/stub_clock.h"

#include <QString>
#include <QtGlobal>

#include <atomic>

namespace sb::tests {

namespace {

std::atomic<double> &override()
{
    static std::atomic<double> value{-1.0};
    return value;
}

double fromEnvironment()
{
    static const double v = [] {
        bool ok = false;
        const double s = qEnvironmentVariable("SYNBENCH_STUB_SCALE").toDouble(&ok);
        if (ok && s > 0)
            return s;
        return qEnvironmentVariableIntValue("SYNBENCH_SMOKE") != 0 ? 0.03 : 1.0;
    }();
    return v;
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

} // namespace sb::tests

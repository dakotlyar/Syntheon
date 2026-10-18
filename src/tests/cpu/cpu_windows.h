// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The CPU test's measurement windows as stored in the telemetry.windows
// series (x: the window's start in s, y: its code), shared by run() and
// detail(). The codes are stored data: stable forever, like metric keys.

#include "tests/cpu/compress/codec.h"

#include <QString>

namespace sb::tests {

enum WindowCode {
    kWinSingleInt = 1,
    kWinSingleFp = 2,
    kWinSingleCompress = 3,
    kWinInt = 10,
    kWinFp = 11,
    kWinPrimes = 12,
    kWinSimd = 13,
    kWinCompress = 20,  // + 2 x codec index, + 1 for decompression (20..27)
    kWinAesHw = 30,
    kWinShaHw = 31,
    kWinAesSw = 32,
    kWinShaSw = 33,
    kWinSortU64 = 40,
    kWinSortStr = 41,
    kWinPhysics = 50,
};

// The name of a multi-thread measurement window (telemetry.windows codes),
// for throttle.detail and the Detail page: "xz compression".
inline QString windowName(int code)
{
    switch (code) {
    case kWinInt: return QStringLiteral("Integer");
    case kWinFp: return QStringLiteral("Floating point");
    case kWinPrimes: return QStringLiteral("Primes");
    case kWinSimd: return QStringLiteral("SIMD");
    case kWinAesHw: return QStringLiteral("AES-256-GCM");
    case kWinShaHw: return QStringLiteral("SHA-256");
    case kWinAesSw: return QStringLiteral("AES-256-GCM (software)");
    case kWinShaSw: return QStringLiteral("SHA-256 (software)");
    case kWinSortU64: return QStringLiteral("64-bit sort");
    case kWinSortStr: return QStringLiteral("string sort");
    case kWinPhysics: return QStringLiteral("Physics");
    default: break;
    }
    if (code >= kWinCompress && code < kWinCompress + 2 * int(cpu::kCodecCount)) {
        const int i = (code - kWinCompress) / 2;
        return QString::fromLatin1(cpu::codecName(cpu::kCodecs[i]))
               + ((code - kWinCompress) % 2 ? QStringLiteral(" decompression") : QStringLiteral(" compression"));
    }
    return {};
}

} // namespace sb::tests

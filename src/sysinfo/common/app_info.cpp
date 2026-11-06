// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/collector.h"

#include "syntheon_version.h"

#include <QtGlobal>

namespace sb::sysinfo {

AppInfo appInfo()
{
    AppInfo a;
    a.version = QStringLiteral(SYNTHEON_DISPLAY_VERSION);
    a.build = QStringLiteral(SYNTHEON_BUILD_ID);
    a.gitSha = QStringLiteral(SYNTHEON_GIT_SHA);
    a.qtVersion = QString::fromLatin1(qVersion());
#if defined(_MSC_FULL_VER)
    a.compiler = QStringLiteral("MSVC %1").arg(_MSC_FULL_VER);
#elif defined(__clang__)
    a.compiler = QStringLiteral("Clang %1").arg(QString::fromLatin1(__clang_version__));
#elif defined(__GNUC__)
    a.compiler = QStringLiteral("GCC %1").arg(QString::fromLatin1(__VERSION__));
#endif
    return a;
}

} // namespace sb::sysinfo

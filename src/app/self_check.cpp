// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/self_check.h"

#include "privileged/linux_client/linux_helper_client.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

namespace sb {

int runSelfCheck(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    const LinuxHelperClient::Environment env = LinuxHelperClient::Environment::current();
    const LinuxHelperClient::LaunchPlan plan = LinuxHelperClient::planLaunch(env);

    out << "SynBench self-check\n";
    out << "  APPIMAGE = " << (env.appImage.isEmpty() ? QStringLiteral("(not set)") : env.appImage) << "\n";
    out << "  APPDIR   = " << (env.appDir.isEmpty() ? QStringLiteral("(not set)") : env.appDir) << "\n";
    out << "  binary   = " << env.applicationFilePath << "\n";
    out << "  layout   = " << (plan.kind == LinuxHelperClient::LaunchPlan::Kind::AppImage ? "AppImage" : "not an AppImage (dev or install tree)") << "\n";
    if (!plan.mountDetail.isEmpty())
        out << "  " << plan.mountDetail << "\n";

    bool ok = false;
    if (plan.kind == LinuxHelperClient::LaunchPlan::Kind::AppImage)
        ok = plan.mountVerified;
    else
        ok = !plan.helperPath.isEmpty();
    out << (ok ? "PASS: " : "FAIL: ") << plan.message << "\n";
    out.flush();
    return ok ? 0 : 1;
}

} // namespace sb

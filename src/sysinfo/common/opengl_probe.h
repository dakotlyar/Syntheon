// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace sb::sysinfo {

struct OpenGlInfo {
    bool ok = false;
    QString version, renderer, vendor;
    bool isEs = false;
    QString error;
};

// The one QtGui use in sysinfo (§1): an offscreen OpenGL context, asking for
// the newest core profile. Must run on the GUI thread with a QGuiApplication.
OpenGlInfo probeOpenGl();

} // namespace sb::sysinfo

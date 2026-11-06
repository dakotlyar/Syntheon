// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sb {

// `Syntheon --syntheon-selfcheck` (Linux): a tiny, read-only, unprivileged check
// of how this process would start the root helper. It runs the SAME C++ decision
// the real launch uses (LinuxHelperClient::planLaunch) on the live environment
// ($APPIMAGE, $APPDIR, /proc/self/mountinfo) and prints PASS or FAIL with the
// reason. In an AppImage it passes only if the image's mount is verified; in
// any other layout, only if the helper binary is found. It needs no root, no
// polkit and no window, and changes nothing. Exit status 0 or 1.
int runSelfCheck(int argc, char *argv[]);

} // namespace sb

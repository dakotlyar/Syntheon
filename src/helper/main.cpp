// SPDX-License-Identifier: GPL-3.0-or-later
//
// synbench-helper: the Linux root helper (§6, P11). Started once per GUI
// launch with pkexec; talks to its parent over stdin/stdout only; lives
// exactly as long as the GUI (exits on EOF); never daemonises. No Qt, no
// plugins, no dlopen, no exec/system: every op is a fixed function in
// helper_core.cpp.

#include "helper/helper_core.h"

#include "synbench_version.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

bool isPipe(int fd)
{
    struct stat st {};
    return fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
}

void closeFdsAbove2()
{
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0)
        return;
#endif
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 65536)
        max = 65536;
    for (int fd = 3; fd < max; ++fd)
        close(fd);
}

} // namespace

int main(int argc, char **)
{
    // Hardening.
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    // (d) A crash of a root process must not leave a core dump (the SMBIOS
    // table sits in memory) or let the user attach to it.
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    const struct rlimit noCore = {0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    umask(077);
    if (chdir("/") != 0)
        return 10;
    std::signal(SIGPIPE, SIG_IGN);
    closeFdsAbove2();

    if (argc != 1) {
        std::fputs("synbench-helper: takes no arguments\n", stderr);
        return 2;
    }
    if (!isPipe(0) || !isPipe(1)) {
        std::fputs("synbench-helper: stdin and stdout must be pipes from SynBench\n", stderr);
        return 11;
    }

    // pkexec already scrubs the environment; keep only who asked, then drop it.
    const char *pkexecUid = std::getenv("PKEXEC_UID");
    const std::string requester = pkexecUid ? pkexecUid : "unknown";
    clearenv();
    std::fprintf(stderr, "synbench-helper: started (euid %u) for uid %s\n", unsigned(geteuid()), requester.c_str());

    sb::helper::Config config;
    config.buildId = std::string(SYNBENCH_DISPLAY_VERSION) + " " + SYNBENCH_BUILD_ID;
    config.euid = geteuid();
    sb::helper::Core core(config);

    return sb::helper::serve(core, 0, 1);
}

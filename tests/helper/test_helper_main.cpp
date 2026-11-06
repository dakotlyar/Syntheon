// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST-ONLY helper binary: the real helper's request handling, but reading
// SMBIOS from paths given on the command line, so READ_SMBIOS can be tested
// end to end without root. Never installed or packaged; the shipped
// syntheon-helper takes no arguments and only reads its fixed paths.
//
//   syntheon-helper-test <entry_point> <table> [build id]

#include "helper/helper_core.h"

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    sb::helper::Config config;
    config.entryPointPath = argv[1];
    config.tablePath = argv[2];
    config.buildId = argc > 3 ? argv[3] : "test";
    config.euid = 0;  // pretend, so a client with requireRoot accepts it
    sb::helper::Core core(config);
    return sb::helper::serve(core, 0, 1);
}

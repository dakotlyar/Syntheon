// SPDX-License-Identifier: GPL-3.0-or-later
// A fake synbench-cpucrypto for tst_cpu_crypto (through
// CryptoClient::setWorkerForTests): it prints a well-formed run, or a
// planted defect, and exits. No OpenSSL, no Qt.
//   fake-cpucrypto --fake ok|wrongsum|absurd|nonl|crash [--checksum 0x...] <the worker's usual arguments>
//     ok        a well-formed run with the given checksum
//     wrongsum  the same with checksum 0x1234
//     absurd    the same with every rate 1e12 MB/s
//     nonl      the same, the last line without its newline
//     crash     a ready line, then a crash (an access violation)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    std::string mode, checksum = "0x0000000000000000";
    int reps = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--fake"))
            mode = argv[++i];
        else if (!std::strcmp(argv[i], "--checksum"))
            checksum = argv[++i];
        else if (!std::strcmp(argv[i], "--reps"))
            reps = std::atoi(argv[++i]);
    }
    const bool masked = std::getenv("OPENSSL_ia32cap") || std::getenv("OPENSSL_armcap");
    std::printf("{\"ev\":\"ready\",\"openssl\":\"3.5.9\",\"features\":\"fake\",\"masked\":%s}\n", masked ? "true" : "false");
    std::fflush(stdout);
    if (mode == "crash") {
        volatile int *nowhere = nullptr;
        *nowhere = 1;
        return 0;
    }
    const char *rate = mode == "absurd" ? "1000000000000.000" : "1000.000";
    if (mode == "wrongsum")
        checksum = "0x0000000000001234";
    for (int i = 1; i <= reps; ++i)
        std::printf("{\"ev\":\"rep\",\"i\":%d,\"mbps\":%s}\n", i, rate);
    std::printf("{\"ev\":\"result\",\"median_mbps\":%s,\"spread\":0.000,\"checksum\":\"%s\"}%s", rate, checksum.c_str(),
                mode == "nonl" ? "" : "\n");
    std::fflush(stdout);
    return 0;
}

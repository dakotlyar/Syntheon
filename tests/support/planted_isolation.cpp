// SPDX-License-Identifier: GPL-3.0-or-later
//
// A planted test binary for the isolation guard's self-test: it turns Qt's
// test mode back off before its QCoreApplication exists, as a careless test
// might. test_isolation.cpp must abort it (exit 3, "not a Qt test location")
// before main() goes on; reaching the end is the failure.
#include <QCoreApplication>
#include <QStandardPaths>

#include <cstdio>

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(false);
    QCoreApplication app(argc, argv);
    std::puts("planted test ran on real paths");
    return 0;
}

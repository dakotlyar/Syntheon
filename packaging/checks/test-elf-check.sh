#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Proves elf-check.py FAILS on bad input (task 23): a binary of the wrong
# architecture, one needing a library nobody ships, and one needing a newer
# glibc than the baseline. Uses binaries of the host, so it needs a x86_64 host
# whose glibc is newer than 2.35 for the third case (otherwise that case is skipped).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ok=0
expect_fail() {  # <label> <args...>
    local label=$1; shift
    if python3 "$here/elf-check.py" "$@" >/dev/null 2>"$tmp/err"; then
        echo "FAIL: elf-check passed on: $label" >&2; ok=1
    else
        echo "ok: elf-check rejects: $label ($(head -1 "$tmp/err"))"
    fi
}
# The floor is per architecture: ARM64 is glibc 2.39 (24.04), x64 stays 2.35.
[[ $(python3 "$here/elf-check.py" x --arch aarch64 --print-limits) == "aarch64: glibc 2.39, GLIBCXX 3.4.33, CXXABI 1.3.15" ]] \
    || { echo "FAIL: wrong aarch64 limits" >&2; ok=1; }
[[ $(python3 "$here/elf-check.py" x --arch x86_64 --print-limits) == "x86_64: glibc 2.35, GLIBCXX 3.4.30, CXXABI 1.3.13" ]] \
    || { echo "FAIL: wrong x86_64 limits" >&2; ok=1; }
mkdir -p "$tmp/wrongarch"; cp /bin/true "$tmp/wrongarch/true"
expect_fail "an x86_64 binary in an aarch64 tree" "$tmp/wrongarch" --arch aarch64
mkdir -p "$tmp/missinglib"; cp /bin/true "$tmp/missinglib/true"
: > "$tmp/empty-allowlist"   # nothing counts as a system library
expect_fail "a library that is neither bundled nor a system library" "$tmp/missinglib" --arch x86_64 --allowlist "$tmp/empty-allowlist"
glibc=$(getconf GNU_LIBC_VERSION | awk '{print $2}')
if python3 -c "import sys; sys.exit(0 if tuple(map(int,'$glibc'.split('.')))>(2,35) else 1)"; then
    mkdir -p "$tmp/newglibc"; cp /bin/true "$tmp/newglibc/true"
    if objdump -T /bin/true | grep -qE 'GLIBC_2\.(3[6-9]|[4-9][0-9])'; then
        expect_fail "a binary needing glibc newer than 2.35" "$tmp/newglibc" --arch x86_64
    else
        echo "skip: /bin/true needs nothing newer than glibc 2.35 here"
    fi
else
    echo "skip: this host's glibc is not newer than 2.35"
fi
exit $ok

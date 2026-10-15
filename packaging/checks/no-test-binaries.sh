#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fails if a test-only binary ended up in a tree that ships (install
# prefix, AppDir, deploy folder). In particular synbench-helper-test reads
# SMBIOS from paths on its command line, so it must never sit where pkexec
# could be pointed at it.
#
#   packaging/checks/no-test-binaries.sh <tree> [<tree>...]
set -euo pipefail
[[ $# -ge 1 ]] || { echo "usage: $0 <tree>..." >&2; exit 2; }
found=0
for tree in "$@"; do
    [[ -d $tree ]] || continue
    while IFS= read -r -d '' f; do
        echo "test-only binary in a shipping tree: $f" >&2
        found=1
    done < <(find "$tree" \( -iname 'synbench-helper-test*' -o -iname 'test_helper*' -o -iname 'tst_*' \) -print0)
done
if [[ $found -ne 0 ]]; then
    exit 1
fi
echo "no test-only binaries in: $*"

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Proves a PACKAGED build carries none of the developer hooks (SYNBENCH_SMOKE,
# SYNBENCH_TOUR*, SYNBENCH_STUB_SCALE, SYNBENCH_DATA_DIR, ...): no binary in the
# tree may contain the text "SYNBENCH_" at all, in 8-bit or UTF-16 form.
#
#   packaging/checks/no-dev-hooks.sh <tree>...
# Counterpart for Windows: no-dev-hooks.ps1.
set -euo pipefail
[[ $# -ge 1 ]] || { echo "usage: $0 <tree>..." >&2; exit 2; }
bad=0
for tree in "$@"; do
    while IFS= read -r -d '' f; do
        # (no `grep -q` in a pipeline: under pipefail its early exit makes `strings` fail)
        text=$(strings -a "$f"; strings -a -el "$f")
        if grep -q 'SYNBENCH_' <<<"$text"; then
            echo "developer hook text found in a packaged binary: $f" >&2
            grep 'SYNBENCH_' <<<"$text" | head -5 >&2 || true
            bad=1
        fi
    done < <(find "$tree" -type f \( -perm -u+x -o -name '*.so*' \) -print0)
done
[[ $bad -eq 0 ]] || exit 1
echo "no developer hook names in: $*"

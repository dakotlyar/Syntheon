#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The packaged app's first launch on a home with a history from before the rename:
# the history and the settings must arrive, and the old files must not change.
# Runs in a throwaway home, with every XDG folder inside it.
#
#   packaging/checks/test-migration-linux.sh <AppImage>
set -euo pipefail
[ "${GITHUB_ACTIONS:-}" = true ] || { echo "CI only"; exit 2; }
[[ $# -eq 1 ]] || { echo "usage: $0 <AppImage>" >&2; exit 2; }
appimage=$(readlink -f "$1")
checks=$(dirname "$(readlink -f "$0")")

t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
export HOME="$t/home"
export XDG_DATA_HOME="$HOME/.local/share"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_RUNTIME_DIR="$t/run"
mkdir -p "$XDG_DATA_HOME/SynBench" "$XDG_CONFIG_HOME/SynBench" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

old_db="$XDG_DATA_HOME/SynBench/synbench.db"
old_ini="$XDG_CONFIG_HOME/SynBench/settings.ini"
new_db="$XDG_DATA_HOME/Syntheon/syntheon.db"
new_ini="$XDG_CONFIG_HOME/Syntheon/settings.ini"
python3 -I "$checks/make-legacy-db.py" "$old_db"
printf '[ui]\ntheme=dark\n' > "$old_ini"
before=$(sha256sum "$old_db" "$old_ini")

# timeout exits 124 when the app was still running, as in the launch check before this.
set +e
QT_QPA_PLATFORM=offscreen timeout 15 "$appimage"
code=$?
set -e
[ "$code" -eq 124 ] || { echo "::error::Syntheon exited with $code within 15 s"; exit 1; }

[ -f "$new_db" ] || { echo "::error::no $new_db after the first launch"; exit 1; }
runs=$(python3 -I -c 'import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute("select count(*) from runs").fetchone()[0])' "$new_db")
[ "$runs" = 3 ] || { echo "::error::the new history has $runs runs, expected 3"; exit 1; }
grep -qx 'theme=dark' "$new_ini" || { echo "::error::the theme didn't reach $new_ini"; exit 1; }
[ "$(sha256sum "$old_db" "$old_ini")" = "$before" ] || { echo "::error::a file from before the rename changed"; exit 1; }
echo "the history and settings were copied, the old files are unchanged"

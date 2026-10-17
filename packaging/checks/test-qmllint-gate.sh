#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Negative control for the qmllint gate: a check that can't fail proves
# nothing. Runs the real qmllint on a planted defect and on a clean file.
#   test-qmllint-gate.sh <qmllint> <qmllint-gate.py>
set -u
qmllint=$1 gate=$2
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

# An unqualified access to an outer object's property (what task 2 removed).
cat > "$d/Bad.qml" <<'QML'
import QtQuick
Item {
    property int side: 4
    Item { width: side }
}
QML
# An unused import (qmllint says "info"; the gate fails it anyway).
cat > "$d/Unused.qml" <<'QML'
import QtQuick
import QtQuick.Layouts
Item {
}
QML
cat > "$d/Good.qml" <<'QML'
import QtQuick
Item {
    id: root
    property int side: 4
    Item { width: root.side }
}
QML
"$qmllint" --json "$d/bad.json" "$d/Bad.qml" > /dev/null 2>&1
"$qmllint" --json "$d/good.json" "$d/Good.qml" > /dev/null 2>&1
python3 "$gate" "$d/bad.json" > "$d/out.txt" 2>&1 && fail "the gate passed a planted unqualified access"
grep -q 'Bad.qml:4' "$d/out.txt" || fail "the gate did not name the planted warning: $(cat "$d/out.txt")"
"$qmllint" --json "$d/unused.json" "$d/Unused.qml" > /dev/null 2>&1
python3 "$gate" "$d/unused.json" > "$d/out.txt" 2>&1 && fail "the gate passed an unused import"
grep -q 'unused-imports' "$d/out.txt" || fail "the gate did not name the unused import: $(cat "$d/out.txt")"
python3 "$gate" "$d/good.json" > /dev/null 2>&1 || fail "the gate failed a clean file"
echo '{"files": []}' > "$d/empty.json"
python3 "$gate" "$d/empty.json" > /dev/null 2>&1 && fail "the gate passed a report with no files"
python3 "$gate" "$d/missing.json" > /dev/null 2>&1 && fail "the gate passed a missing report"
echo "PASS: the qmllint gate fails on a planted warning, an unused import, an empty and a missing report, and passes a clean file"

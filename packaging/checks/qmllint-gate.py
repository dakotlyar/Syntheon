#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The qmllint gate (Phase 2 design §13): no QML warning gets in.

  qmllint-gate.py <report.json> [<report.json> ...]

Each report is qmllint's --json output (the all_qmllint_json target writes
<build>/synbench_ui_qmllint.json; CI writes one for tests/qml). Every finding
of type warning or worse is printed as file:line:col and fails the gate, and
so does a file qmllint marked unsuccessful. Info-level findings are printed
but don't fail.

Why the JSON and not qmllint's own --max-warnings: qmllint 6.12.0's text
output leaves out some warnings that it still counts (block-scoped var,
unqualified access inside functions, seen 2026-10-16), so a failing
"--max-warnings 0" run can print nothing at all. The JSON has them all.

A report that is missing, unreadable or lists no files fails too: a lint
step that silently checked nothing must not pass.
"""
import json
import sys
from pathlib import Path

FAILING = {"warning", "critical", "error", "fatal"}


def main(paths):
    if not paths:
        print("usage: qmllint-gate.py <report.json> [...]", file=sys.stderr)
        return 2
    failures = 0
    files_seen = 0
    for path in paths:
        try:
            report = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            print(f"::error::{path}: not a qmllint JSON report ({e})")
            failures += 1
            continue
        files = report.get("files", [])
        if not files:
            print(f"::error::{path}: the report lists no QML files (did the lint run?)")
            failures += 1
            continue
        for f in files:
            files_seen += 1
            name = f.get("filename", "?")
            findings = f.get("warnings", [])
            for w in findings:
                kind = w.get("type", "warning")
                where = f"{name}:{w.get('line', 0)}:{w.get('column', 0)}"
                message = " ".join(str(w.get("message", "")).split())
                line = f"{where}: {kind}: {message} [{w.get('id', '?')}]"
                if kind in FAILING:
                    print(f"::error::{line}")
                    failures += 1
                else:
                    print(f"notice: {line}")
            if not f.get("success", True) and not any(w.get("type", "warning") in FAILING for w in findings):
                print(f"::error::{name}: qmllint reports it as failed")
                failures += 1
    print(f"qmllint gate: {files_seen} files, {failures} failing findings")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A history as a build from before the rename leaves it, for the packaged
first-launch tests (test-migration-linux.sh, test-install.ps1): the shipped
schema script, WAL mode, user_version 1, three runs with one test result each,
and the WAL folded in, as after a clean quit. Standard library only: the
Windows job runs it with python -I.

  make-legacy-db.py <out.db>
"""
import pathlib
import sqlite3
import sys

SCHEMA = pathlib.Path(__file__).resolve().parents[2] / "src/storage/migrations/0001_init.sql"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = pathlib.Path(sys.argv[1])
    if out.exists():
        print(f"{out} already exists", file=sys.stderr)
        return 2
    con = sqlite3.connect(out)
    con.execute("PRAGMA journal_mode = WAL")
    con.executescript(SCHEMA.read_text(encoding="utf-8"))
    con.execute("PRAGMA user_version = 1")
    for n in range(1, 4):
        started = f"2026-10-24T14:0{n}:03.456Z"
        run = con.execute(
            "INSERT INTO runs (uuid, kind, status, started_at, finished_at, app_version, app_build,"
            " machine_label, overall_score, system_json) VALUES (?, 'standard', 'completed', ?, ?,"
            " '2026.11.0', 'ci abc1234', 'CI', ?, '{}')",
            (f"00000000-0000-4000-8000-00000000000{n}", started, started, 1000.0 + n),
        ).lastrowid
        con.execute(
            "INSERT INTO test_results (run_id, position, test_id, test_version, formula_version,"
            " status, score, options_json, started_at, duration_ms) VALUES (?, 0, 'cpu', 1, 1, 'ok',"
            " ?, '{}', ?, 1000)",
            (run, 1000.0 + n, started),
        )
    con.commit()
    busy = con.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()[0]
    con.close()
    if busy != 0:
        print("the WAL could not be folded in", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

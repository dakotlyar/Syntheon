#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CTest for tools/cpu-repeatability.py.

Builds small histories with SynBench's own schema (src/storage/migrations/*.sql,
applied in order, as the app does) in a temporary folder, then runs the script
on them:
  * five steady runs: every CV within 3%, exit 0, and the CVs the script prints
    match an independent computation;
  * one run 10% off: the CPU score's CV is flagged, exit 1;
  * the last N picks the newest scored results; an unscored (Partial) result
    is skipped by default and shown, without counting, with --runs;
  * the conditions are reported: battery, throttling, background load;
  * the database is opened read-only: its bytes don't change, and a read-only
    file works; with a rollback journal nothing is left behind, and with WAL
    (SynBench's own mode) only a -shm file and an empty -wal file are;
  * usage and database problems exit 2.

  test_cpu_repeatability.py <source dir>
"""
import hashlib
import os
import pathlib
import sqlite3
import statistics
import subprocess
import sys
import tempfile

SRC = pathlib.Path(sys.argv[1]).resolve()
SCRIPT = SRC / "tools" / "cpu-repeatability.py"
MIGRATIONS = sorted((SRC / "src" / "storage" / "migrations").glob("*.sql"))
GROUPS = ["int", "fp", "simd", "compress", "crypto", "sort", "physics"]
WEIGHTS = [0.25, 0.25, 0.15, 0.10, 0.10, 0.10, 0.05]

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def make_db(path, runs, wal=False):
    con = sqlite3.connect(path)
    if wal:
        con.execute("PRAGMA journal_mode=WAL")
    for i, m in enumerate(MIGRATIONS, start=1):
        con.executescript(m.read_text(encoding="utf-8"))
        con.execute(f"PRAGMA user_version = {i}")
    for i, r in enumerate(runs, start=1):
        con.execute("INSERT INTO runs (id, uuid, kind, status, started_at, finished_at, app_version, app_build, "
                    "machine_label, overall_score, system_json) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    (i, f"00000000-0000-0000-0000-{i:012d}", "standard", r.get("run_status", "completed"),
                     f"2026-10-18T1{i}:00:00.000Z", f"2026-10-18T1{i}:04:30.000Z", "2026.11.0", "linux-x64 test",
                     r.get("machine", "Test machine"), r.get("score"), "{}"))
        con.execute("INSERT INTO test_results (id, run_id, position, test_id, test_version, formula_version, status, "
                    "score, options_json) VALUES (?,?,?,?,?,?,?,?,?)",
                    (i, i, 0, "cpu", 1, r.get("formula", 1), r.get("status", "ok"), r.get("score"), "{}"))
        for g, s in zip(GROUPS, r["subs"]):
            con.execute("INSERT INTO sub_results (test_result_id, sub_id, status, score) VALUES (?,?,?,?)",
                        (i, g, "ok" if s is not None else "error", s))
        pos = 0

        def m(key, num=None, text=None):
            nonlocal pos
            con.execute("INSERT INTO metrics (test_result_id, key, label, value_num, value_text, direction, position) "
                        "VALUES (?,?,?,?,?,?,?)", (i, key, key, num, text, "neutral", pos))
            pos += 1

        m("background.load", num=r.get("bg", 0.8))
        m("power.source", text=r.get("power", "AC"))
        m("power.profile", text=r.get("profile", "balanced"))
        m("single.mark", num=r["mark"])
        m("throttle", text=r.get("throttle", "Not detected"))
    con.commit()
    con.close()


def geo(subs):
    return round(2 ** sum(w * __import__("math").log2(s) for w, s in zip(WEIGHTS, subs)))


def run(*args):
    p = subprocess.run([sys.executable, "-I", str(SCRIPT), *map(str, args)], capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def steady(k):
    subs = [1000 + d for d in (k, -k, 2 * k, 0, k, -2 * k, 0)]
    return {"subs": subs, "score": geo(subs), "mark": 1000 + k}


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)

    # 1. Five steady runs.
    good = tmp / "good.db"
    runs = [steady(k) for k in (0, 2, -1, 3, -2)]
    make_db(good, runs)
    before = hashlib.sha256(good.read_bytes()).hexdigest()
    rc, out, err = run(good)
    print(out, err)
    check(rc == 0, "steady set: exit 0")
    check("RESULT: PASS" in out, "steady set: PASS")
    want = statistics.stdev([r["score"] for r in runs]) / statistics.fmean([r["score"] for r in runs]) * 100
    check(f"CPU score            {want:5.2f}%" in out, f"steady set: the CPU score's CV is {want:.2f}%")
    want_mark = statistics.stdev([r["mark"] for r in runs]) / statistics.fmean([r["mark"] for r in runs]) * 100
    check(f"Single-thread mark   {want_mark:5.2f}%" in out, f"steady set: the mark's CV is {want_mark:.2f}%")
    check(sum(l.rstrip().endswith("  ok") and "%" in l for l in out.splitlines()) == 9, "steady set: nine CVs (score, seven groups, mark), all ok")
    check(hashlib.sha256(good.read_bytes()).hexdigest() == before, "the database's bytes are unchanged")
    check(not (tmp / "good.db-wal").exists() and not (tmp / "good.db-journal").exists(),
          "rollback-journal history: no journal or WAL file made")
    os.chmod(good, 0o444)
    rc, out, _ = run(good)
    check(rc == 0, "a read-only database file works")
    os.chmod(good, 0o644)

    # 1b. A WAL-mode history, as SynBench writes it, with the app closed (its
    # -wal checkpointed away). A read-only connection may create a -shm file and
    # an empty -wal file (which ones depends on the SQLite version; SQLite
    # 3.54 leaves both); the history itself must not change.
    waldir = tmp / "wal"
    waldir.mkdir()
    waldb = waldir / "synbench.db"
    make_db(waldb, [steady(k) for k in (0, 2, -1, 3, -2)], wal=True)
    probe = sqlite3.connect(waldb)
    check(probe.execute("PRAGMA journal_mode").fetchone()[0] == "wal", "WAL fixture: the history is in WAL mode")
    probe.close()
    check(sorted(p.name for p in waldir.iterdir()) == ["synbench.db"], "WAL fixture: only the database before the run")
    before = hashlib.sha256(waldb.read_bytes()).hexdigest()
    rc, out, err = run(waldb)
    check(rc == 0 and "RESULT: PASS" in out, "WAL history: read, PASS")
    check(hashlib.sha256(waldb.read_bytes()).hexdigest() == before, "WAL history: the database's bytes are unchanged")
    left = sorted(p.name for p in waldir.iterdir())
    check(set(left) <= {"synbench.db", "synbench.db-shm", "synbench.db-wal"}, f"WAL history: nothing but -shm/-wal left beside it ({left})")
    wal = waldir / "synbench.db-wal"
    check(not wal.exists() or wal.stat().st_size == 0, "WAL history: any -wal left behind is empty (nothing was written)")
    check("synbench.db" in left, "WAL history: the database is still there")

    # 2. One run 10% low: the score's CV is above 3%.
    bad = tmp / "bad.db"
    runs = [steady(k) for k in (0, 2, -1, 3)]
    low = [s * 0.9 for s in runs[0]["subs"]]
    runs.append({"subs": low, "score": geo(low), "mark": 900, "power": "Battery", "throttle": "Detected", "bg": 18.0})
    make_db(bad, runs)
    rc, out, err = run(bad)
    print(out, err)
    check(rc == 1, "10% outlier: exit 1")
    check("RESULT: FAIL" in out, "10% outlier: FAIL")
    check(any(l.strip().startswith("CPU score") and l.rstrip().endswith("FAIL") for l in out.splitlines()),
          "10% outlier: the CPU score line is flagged")
    check("run 5: power Battery, background load 18%, throttling detected" in out, "the outlier's conditions are listed")

    # 3. The last N, and unscored results.
    mix = tmp / "mix.db"
    runs = [steady(k) for k in (5, 0, 1, -1, 2, 0)]
    runs.insert(3, {"subs": [1000, None, 1000, 1000, 1000, 1000, 1000], "score": None, "mark": 1000, "status": "partial"})
    make_db(mix, runs)  # run 4 is the Partial one
    rc, out, _ = run(mix, "-n", 3)
    print(out)
    check("3 run(s)" in out, "-n 3: three runs")
    ids = [l.split()[0] for l in out.splitlines()[2:5]]
    check(ids == ["5", "6", "7"], f"-n 3: the newest scored runs 5, 6, 7 (got {ids})")
    rc, out, _ = run(mix)
    ids = [l.split()[0] for l in out.splitlines()[2:7]]
    check(ids == ["2", "3", "5", "6", "7"], f"default: the last 5 scored runs, the Partial one skipped (got {ids})")
    rc, out, _ = run(mix, "--runs", "3,4,5")
    print(out)
    check("run 4: status partial" in out, "--runs: the Partial run is shown and named")
    check(any(l.strip().startswith("CPU score") and "(n=2," in l for l in out.splitlines()),
          "--runs: its missing score isn't counted")
    check(any(l.strip().startswith("FP + primes") and "(n=2," in l for l in out.splitlines()),
          "--runs: its missing group isn't counted")

    # 4. Mixed formula versions are pointed out.
    mixed = tmp / "mixed.db"
    runs = [steady(k) for k in (0, 1, 2)]
    runs[0]["formula"] = 0
    make_db(mixed, runs)
    rc, out, _ = run(mixed)
    check("only 2 comparable scored result(s), of the 5 asked for" in out, "fewer results than asked for are pointed out")
    check("2 run(s)" in out and "different CPU test or formula versions" not in out,
          "by default an older formula version's result is left out")
    rc, out, _ = run(mixed, "--runs", "1,2,3")
    check("different CPU test or formula versions" in out, "--runs: mixed formula versions are pointed out")

    # 5. Problems.
    check(run(tmp / "missing.db")[0] == 2, "a missing file: exit 2")
    check(not (tmp / "missing.db").exists(), "a missing file isn't created")
    junk = tmp / "junk.db"
    junk.write_bytes(b"not a database" * 100)
    check(run(junk)[0] == 2, "not a database: exit 2")
    empty = tmp / "empty.db"
    make_db(empty, [])
    check(run(empty)[0] == 2, "no CPU results: exit 2")
    check(run(good, "--runs", "9")[0] == 2, "an unknown run id: exit 2")
    check(run(good, "--runs", "x")[0] == 2, "a bad --runs: exit 2")
    check(run(good, "-n", "0")[0] == 2, "-n 0: exit 2")
    check(run()[0] == 2, "no arguments: exit 2")

print(f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)

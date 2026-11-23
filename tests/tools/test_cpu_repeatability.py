#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CTest for tools/cpu-repeatability.py.

Builds small histories with Syntheon's own schema (src/storage/migrations/*.sql,
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
    (Syntheon's own mode) only a -shm file and an empty -wal file are;
  * usage and database problems exit 2;
  * --test ram (Phase 3 §14.3): the RAM score, its three groups and every
    scored metric; a metric no run has (latency.l3 without an L3) is "not
    measured" and doesn't fail the set; low huge-page coverage is reported;
    a history without RAM results exits 2.
  * --anchors (Phase 3 §15.1, §10.3): the medians at 3 significant figures
    as formula_v1.cpp's constants (whose names the tool must match), every
    run re-scored with them (the median run 1,000), the gate on status and
    huge pages >= 90%; not for --test cpu.
  * --test gpu (Phase 4a task 35, §17.4) on the history that tst_gpu_test's
    repeatabilityFixture slot writes (five runs scored by the C++ formula):
    the CVs of the score, the three groups and the ten scored metrics; the
    tool's formula and its table of formula_v1.cpp's current constants
    equal the C++ file (the Phase 3 8c95648 rule): every run re-scored with
    the current constants equals the C++ score, the group scores, score.3d
    and score.2d that the fixture stores; --anchors prints the ten constants
    in formula_v1.cpp's order, 3-significant-figure medians, the re-scores
    (about 1,000 at the median), and gates on status ok (a Partial run is
    reported and fails the gate); a real Partial run with no score is named by
    the default selection (no --runs), and fewer than N Ok runs fail; only the
    score's CV gates under --test gpu (a noisy metric is reported).

  test_cpu_repeatability.py <source dir> <GPU fixture syntheon.db>   (both required; the fixture is not optional)
"""
import hashlib
import importlib.util
import os
import re
import shutil
import pathlib
import sqlite3
import statistics
import subprocess
import sys
import tempfile

if len(sys.argv) != 3 or not sys.argv[1].strip() or not sys.argv[2].strip():
    # An empty argument would resolve to the current directory: refuse it instead of testing the wrong thing.
    print("usage: test_cpu_repeatability.py <source dir> <GPU fixture syntheon.db> (neither may be empty)", file=sys.stderr)
    sys.exit(2)
SRC = pathlib.Path(sys.argv[1]).resolve()
GPU_FIXTURE = pathlib.Path(sys.argv[2]).resolve()
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
                     f"2026-10-18T1{i}:00:00.000Z", f"2026-10-18T1{i}:04:30.000Z", "0.4.3", "linux-x64 test",
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


RAM_GROUPS = ["latency", "bandwidth", "loaded"]
RAM_METRICS = ["latency.dram", "latency.l3", "latency.l2", "latency.l1", "bandwidth.read", "bandwidth.write",
               "bandwidth.copy", "bandwidth.st.read", "bandwidth.st.write", "bandwidth.st.copy", "loaded.latency"]


def make_ram_db(path, runs):
    con = sqlite3.connect(path)
    for i, m in enumerate(MIGRATIONS, start=1):
        con.executescript(m.read_text(encoding="utf-8"))
        con.execute(f"PRAGMA user_version = {i}")
    for i, r in enumerate(runs, start=1):
        con.execute("INSERT INTO runs (id, uuid, kind, status, started_at, finished_at, app_version, app_build, "
                    "machine_label, overall_score, system_json) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    (i, f"00000000-0000-0000-0000-{i:012d}", "standard", "completed", f"2026-10-20T1{i}:00:00.000Z",
                     f"2026-10-20T1{i}:02:30.000Z", "0.4.3", "linux-x64 test", "Test machine", r["score"], "{}"))
        con.execute("INSERT INTO test_results (id, run_id, position, test_id, test_version, formula_version, status, "
                    "score, options_json) VALUES (?,?,?,?,?,?,?,?,?)", (i, i, 0, "ram", 0, 0, "ok", r["score"], "{}"))
        for g, sc in zip(RAM_GROUPS, r["subs"]):
            con.execute("INSERT INTO sub_results (test_result_id, sub_id, status, score) VALUES (?,?,?,?)", (i, g, "ok", sc))
        pos = 0
        for key, num, text in [("background.load", 1.0, None), ("power.source", None, "AC"), ("power.profile", None, "balanced"),
                               ("pages.huge", r.get("huge", 100.0), None)] + [(k, v, None) for k, v in r["metrics"].items()]:
            con.execute("INSERT INTO metrics (test_result_id, key, label, value_num, value_text, direction, position) "
                        "VALUES (?,?,?,?,?,?,?)", (i, key, key, num, text, "neutral", pos))
            pos += 1
    con.commit()
    con.close()


def ram_run(k, l3=True, huge=100.0):
    metrics = {key: 50.0 * (1 + 0.005 * k) for key in RAM_METRICS}
    if not l3:
        del metrics["latency.l3"]
    return {"score": 1000 + k, "subs": [1000 + k, 1000 - k, 1000 + 2 * k], "metrics": metrics, "huge": huge}


def geo(subs):
    return round(2 ** sum(w * __import__("math").log2(s) for w, s in zip(WEIGHTS, subs)))


def run(*args):
    p = subprocess.run([sys.executable, "-I", str(SCRIPT), *map(str, args)], capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def steady(k):
    subs = [1000 + d for d in (k, -k, 2 * k, 0, k, -2 * k, 0)]
    return {"subs": subs, "score": geo(subs), "mark": 1000 + k}


def load_tool():
    spec = importlib.util.spec_from_file_location("cpu_repeatability_under_test", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def gpu_tests(tmp):
    tool = load_tool()
    gpu_cpp = (SRC / "src" / "tests" / "gpu" / "formula_v1.cpp").read_text(encoding="utf-8")
    ro = sqlite3.connect(GPU_FIXTURE.as_uri() + "?mode=ro", uri=True)
    stored = {rid: score for rid, score in ro.execute("SELECT run_id, score FROM test_results WHERE test_id = 'gpu' ORDER BY run_id")}
    check(len(stored) == 5 and all(v for v in stored.values()), f"GPU fixture: five scored runs ({stored})")
    metric_rows = {}
    for rid, key, num in ro.execute("SELECT t.run_id, m.key, m.value_num FROM metrics m JOIN test_results t ON t.id = m.test_result_id "
                                    "WHERE t.test_id = 'gpu' AND m.value_num IS NOT NULL"):
        metric_rows.setdefault(rid, {})[key] = num
    ro.close()
    scored_keys = [k for k, _ in tool.TESTS["gpu"]["metrics"]]

    # The script's table of formula_v1.cpp's constants IS that file's, in its order, and its formula is the C++ one:
    # re-scored with them, every run equals the score, the group scores, score.3d and score.2d the C++ code stored.
    cpp_consts = re.findall(r"\b(k[A-Z]\w+) = ([0-9.]+)", gpu_cpp.split("const QVector<GeometricGroup>")[0])  # the anchor block
    check([n for n, _ in cpp_consts] == [n for n, _, _ in tool.GPU_ANCHOR_NAMES],
          "GPU: the script's constants are formula_v1.cpp's, in its order")
    cur = {key: tool.GPU_CURRENT_ANCHORS[key] for _, key, _ in tool.GPU_ANCHOR_NAMES}
    check([float(v) for _, v in cpp_consts] == [cur[key] for _, key, _ in tool.GPU_ANCHOR_NAMES],
          "GPU: the script's current anchors are formula_v1.cpp's values")
    weights = re.findall(r'\{QStringLiteral\("((?:scenes3d|native2d|fill2d)\.[a-z.]+)"\), k\w+, ([0-9.]+)\}', gpu_cpp)
    want_w = {k: float(w) for k, w in weights}
    got_w = {k: w for _, _, items in tool.GPU_FORMULA for k, w in items}
    check(got_w == want_w and len(want_w) == 10, "GPU: the script's weights are formula_v1.cpp's")
    group_w = [float(x) for x in re.findall(r'QStringLiteral\("(?:scenes3d|native2d|fill2d)"\), ([0-9.]+),', gpu_cpp)]
    check(group_w == [w for _, w, _ in tool.GPU_FORMULA], "GPU: the script's group weights are formula_v1.cpp's")
    con = sqlite3.connect(GPU_FIXTURE.as_uri() + "?mode=ro", uri=True)
    for rid in stored:
        total, subs, two_d = tool.gpu_rescore(metric_rows[rid], cur)
        sub_stored = {sid: sc for sid, sc in con.execute(
            "SELECT sub_id, score FROM sub_results WHERE test_result_id = (SELECT id FROM test_results WHERE run_id = ? AND test_id = 'gpu')", (rid,))}
        check(total == stored[rid] and subs == sub_stored and subs["scenes3d"] == metric_rows[rid]["score.3d"]
              and two_d == metric_rows[rid]["score.2d"],
              f"GPU run {rid}: the script re-scores to the C++ score {stored[rid]:,.0f}, groups {sub_stored}, 3D and 2D")
    con.close()

    rc, out, err = run(GPU_FIXTURE, "--test", "gpu")
    print(out, err)
    check(rc == 0 and "RESULT: PASS" in out, "GPU steady set: exit 0, PASS")
    check(out.startswith("GPU repeatability: 5 run(s)"), "GPU: the heading names the test")
    vals = list(stored.values())
    want = statistics.stdev(vals) / statistics.fmean(vals) * 100
    check(f"GPU score            {want:5.2f}%" in out, f"GPU: the score's CV is {want:.2f}%")
    for label in ("3D scenes", "Native 2D", "2D fill-rate", "Geometry (fps)", "Particles (fps)", "Blits (MOps/s)", "Quads (Gpx/s)", "Sprites (MOps/s)"):
        check(any(l.strip().startswith(label + " ") and "%" in l for l in out.splitlines()), f"GPU: a CV line for {label}")
    check(sum("%  (n=5" in l for l in out.splitlines()) == 14, "GPU: fourteen CVs (score, three groups, ten metrics)")

    # --anchors: the gate, the constants in formula_v1.cpp's order at 3 significant figures, the re-scores.
    rc, out, err = run(GPU_FIXTURE, "--test", "gpu", "--anchors")
    print(out, err)
    check(rc == 0 and "RESULT: PASS" in out, "GPU --anchors on a steady set: PASS")
    printed = re.findall(r"constexpr double (k\w+) = ([0-9.]+);", out)
    check([n for n, _ in printed] == [n for n, _ in cpp_consts], "GPU --anchors: the constants are formula_v1.cpp's, in its order")
    want_a = {key: float(f"{statistics.median([metric_rows[r][key] for r in stored]):.3g}") for key in scored_keys}
    got = dict(printed)
    check(all(float(got[n]) == want_a[key] for n, key, _ in tool.GPU_ANCHOR_NAMES), "GPU --anchors: medians to 3 significant figures")
    ok_all = True
    for rid in stored:
        total, subs, two_d = tool.gpu_rescore(metric_rows[rid], want_a)
        ok_all = ok_all and f"run {rid}: {total:,.0f} (stored {stored[rid]:,.0f})" in out
    check(ok_all, "GPU --anchors: each run re-scored with the printed anchors")
    med = statistics.median([tool.gpu_rescore(metric_rows[r], want_a)[0] for r in stored])
    check(abs(med - 1000) <= 5 and f"median {med:,.0f}" in out, f"GPU --anchors: the re-scored median is about 1,000 ({med:,.0f})")
    check(out.count("  equal") == 5 and "differs" not in out, "GPU --anchors: with formula_v1.cpp's current constants every run equals its stored score")
    check("5 run(s), every one status ok" in out and "Calibration gate (§17.4)" in out, "GPU --anchors: the gate is named")

    # A Partial run fails the gate and is reported; a run stored under other anchors "differs" without failing the gate.
    part = tmp / "gpu-partial.db"
    shutil.copyfile(GPU_FIXTURE, part)
    c = sqlite3.connect(part)
    c.execute("UPDATE test_results SET status = 'partial' WHERE run_id = 3 AND test_id = 'gpu'")
    c.commit()
    c.close()
    rc, out, _ = run(part, "--test", "gpu", "--anchors", "--runs", "1,2,3,4,5")
    print(out)
    check(rc == 1 and "run 3: status partial  FAIL" in out, "GPU --anchors: a Partial run fails the gate")
    check("run 3: partial, no reason stored, doesn't count" in out and "Conditions to check" in out,
          "GPU: the Partial run is reported under the conditions")

    # A real Partial run (no score, a reason), default selection with no --runs: it is named, doesn't count, and fewer than
    # N Ok runs is a clear failure (AP-5 High).
    real = tmp / "gpu-real-partial.db"
    shutil.copyfile(GPU_FIXTURE, real)
    c = sqlite3.connect(real)
    c.execute("UPDATE test_results SET status = 'partial', score = NULL, "
              "status_reason = 'OpenGL failed during Shading: too few frames' WHERE run_id = 3 AND test_id = 'gpu'")
    c.execute("DELETE FROM sub_results WHERE test_result_id = (SELECT id FROM test_results WHERE run_id = 3 AND test_id = 'gpu')")
    c.commit()
    c.close()
    rc, out, _ = run(real, "--test", "gpu")
    print(out)
    check(rc == 1 and "RESULT: FAIL" in out, "GPU: a real Partial run in the default selection fails")
    check("run 3: partial, OpenGL failed during Shading: too few frames, doesn't count" in out, "GPU: the Partial run is named with its reason")
    check("not enough Ok runs: 4 of the 5 asked for  FAIL (not counted: run 3 partial)" in out, "GPU: 'not enough Ok runs' lists the Partial one")
    check(any(l.split()[:1] == ["3"] and "partial" in l for l in out.splitlines()), "GPU: the Partial run is a row of the table")
    check(any(l.strip().startswith("GPU score") and "(n=4," in l for l in out.splitlines()), "GPU: its missing score isn't counted")
    rc, out, _ = run(real, "--test", "gpu", "--anchors")
    check(rc == 1 and "run 3: status partial  FAIL" in out and "only 4 of the 5" not in out, "GPU --anchors: the same, in the gate")
    # With -n 2 the Partial run is outside the selection: the newest two Ok runs gate on their own.
    rc, out, _ = run(real, "--test", "gpu", "-n", "2")
    check(rc == 0 and "run 3" not in out.split("CV (sample")[1], "GPU: a Partial run outside the last N is not selected")

    # Only the score's CV gates: a noisy metric (Blits at ~3.4%) with a steady score passes, and is reported.
    noisy = tmp / "gpu-noisy.db"
    shutil.copyfile(GPU_FIXTURE, noisy)
    c = sqlite3.connect(noisy)
    for rid, d in zip(range(1, 6), (-0.034, 0.034, -0.034, 0.034, 0.0)):
        c.execute("UPDATE metrics SET value_num = ? WHERE key = 'native2d.blits' AND test_result_id = "
                  "(SELECT id FROM test_results WHERE run_id = ? AND test_id = 'gpu')", (100.0 * (1 + d), rid))
    c.commit()
    c.close()
    rc, out, _ = run(noisy, "--test", "gpu")
    print(out)
    blits = [l for l in out.splitlines() if l.strip().startswith("Blits")][0]
    check(rc == 0 and "RESULT: PASS" in out, "GPU: a metric at ~3.4% with a steady score passes")
    check("3.40%" in blits and "above target (2%)" in blits and "above 3%" in blits and "FAIL" not in blits, f"GPU: the noisy metric is reported, not failed ({blits.strip()})")
    check("FAIL" not in out.split("Conditions")[0], "GPU: nothing in the CV list fails")
    other = tmp / "gpu-other-anchors.db"
    shutil.copyfile(GPU_FIXTURE, other)
    c = sqlite3.connect(other)
    c.execute("UPDATE test_results SET score = score + 3 WHERE run_id = 2 AND test_id = 'gpu'")
    c.commit()
    c.close()
    rc, out, _ = run(other, "--test", "gpu", "--anchors")
    check(out.count("  equal") == 4 and out.count("  differs") == 1, "GPU --anchors: a stored score that differs is reported as differs")
    check(rc == 0, "GPU --anchors: differs is a report, not a gate failure")
    # One run 10% low on the 3D scenes: the score's CV is flagged.
    bad = tmp / "gpu-bad.db"
    shutil.copyfile(GPU_FIXTURE, bad)
    c = sqlite3.connect(bad)
    c.execute("UPDATE test_results SET score = ROUND(score * 0.8) WHERE run_id = 5 AND test_id = 'gpu'")
    c.commit()
    c.close()
    rc, out, _ = run(bad, "--test", "gpu")
    check(rc == 1 and any(l.strip().startswith("GPU score") and l.rstrip().endswith("FAIL") for l in out.splitlines()),
          "GPU: an outlier's score CV above 3% fails")
    check(run(good_db, "--test", "gpu")[0] == 2, "--test gpu on a CPU-only history: exit 2")
    check(run(good_db, "--anchors")[0] == 2, "--anchors with --test cpu: exit 2")


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)

    # 1. Five steady runs.
    good = tmp / "good.db"
    good_db = good
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

    # 1b. A WAL-mode history, as Syntheon writes it, with the app closed (its
    # -wal checkpointed away). A read-only connection may create a -shm file and
    # an empty -wal file (which ones depends on the SQLite version; SQLite
    # 3.54 leaves both); the history itself must not change.
    waldir = tmp / "wal"
    waldir.mkdir()
    waldb = waldir / "syntheon.db"
    make_db(waldb, [steady(k) for k in (0, 2, -1, 3, -2)], wal=True)
    probe = sqlite3.connect(waldb)
    check(probe.execute("PRAGMA journal_mode").fetchone()[0] == "wal", "WAL fixture: the history is in WAL mode")
    probe.close()
    check(sorted(p.name for p in waldir.iterdir()) == ["syntheon.db"], "WAL fixture: only the database before the run")
    before = hashlib.sha256(waldb.read_bytes()).hexdigest()
    rc, out, err = run(waldb)
    check(rc == 0 and "RESULT: PASS" in out, "WAL history: read, PASS")
    check(hashlib.sha256(waldb.read_bytes()).hexdigest() == before, "WAL history: the database's bytes are unchanged")
    left = sorted(p.name for p in waldir.iterdir())
    check(set(left) <= {"syntheon.db", "syntheon.db-shm", "syntheon.db-wal"}, f"WAL history: nothing but -shm/-wal left beside it ({left})")
    wal = waldir / "syntheon.db-wal"
    check(not wal.exists() or wal.stat().st_size == 0, "WAL history: any -wal left behind is empty (nothing was written)")
    check("syntheon.db" in left, "WAL history: the database is still there")

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

    # 6. --test ram.
    ram = tmp / "ram.db"
    runs = [ram_run(k) for k in (0, 2, -1, 3, -2)]
    make_ram_db(ram, runs)
    rc, out, err = run(ram, "--test", "ram")
    print(out, err)
    check(rc == 0 and "RESULT: PASS" in out, "RAM steady set: exit 0, PASS")
    check(out.startswith("RAM repeatability: 5 run(s)"), "RAM: the heading names the test")
    want = statistics.stdev([r["score"] for r in runs]) / statistics.fmean([r["score"] for r in runs]) * 100
    check(f"RAM score            {want:5.2f}%" in out, f"RAM: the score's CV is {want:.2f}%")
    check(sum(l.rstrip().endswith("  ok") and "%" in l for l in out.splitlines()) == 15,
          "RAM: fifteen CVs (score, three groups, eleven metrics), all ok")
    for label in ("Latency", "Bandwidth", "Loaded latency", "DRAM latency", "L3 latency", "Read, all threads",
                  "Copy, one thread", "Loaded latency (ns)"):
        check(any(l.strip().startswith(label + " ") for l in out.splitlines()), f"RAM: a line for {label}")
    rc, out, _ = run(ram)
    check(rc == 2, "RAM-only history without --test ram: no CPU results, exit 2")
    rc, out, _ = run(good, "--test", "ram")
    check(rc == 2, "--test ram on a CPU-only history: exit 2")
    check(run(ram, "--test", "npu")[0] == 2, "--test npu: exit 2")
    check(run(ram, "--test", "gpu")[0] == 2, "--test gpu on a RAM-only history: no GPU results, exit 2")
    # No L3 on any run: not measured, not a failure; low huge pages: reported.
    nol3 = tmp / "nol3.db"
    make_ram_db(nol3, [ram_run(k, l3=False, huge=(41.0 if k == 3 else 100.0)) for k in (0, 2, -1, 3, -2)])
    rc, out, _ = run(nol3, "--test", "ram")
    print(out)
    check(rc == 0 and "RESULT: PASS" in out, "RAM without an L3: PASS")
    check(any(l.strip().startswith("L3 latency") and "not measured" in l and "FAIL" not in l for l in out.splitlines()),
          "RAM without an L3: L3 latency is 'not measured', not a failure")
    check("run 4: huge pages 41% (calibration needs 90%)" in out, "RAM: low huge-page coverage is reported")
    # AP-5 L4: only latency.l3 may be missing everywhere; another scored
    # metric missing from every run fails the set.
    nost = tmp / "nost.db"
    runs = [ram_run(k) for k in (0, 2, -1)]
    for r in runs:
        del r["metrics"]["bandwidth.st.copy"]
    make_ram_db(nost, runs)
    rc, out, _ = run(nost, "--test", "ram", "--runs", "1,2,3")
    print(out)
    check(rc == 1 and "RESULT: FAIL" in out, "RAM without single-thread copy: FAIL")
    check(any(l.strip().startswith("Copy, one thread") and l.rstrip().endswith("FAIL") for l in out.splitlines()),
          "RAM: the missing metric's line is flagged")

    # 7. --anchors (RAM calibration).
    cal = tmp / "cal.db"
    runs = [ram_run(k) for k in (0, 2, -1, 3, -2)]
    for r in runs:  # distinct per metric, so the order of the constants shows
        for j, key in enumerate(RAM_METRICS):
            r["metrics"][key] *= 1 + j / 10
    make_ram_db(cal, runs)
    rc, out, _ = run(cal, "--test", "ram", "--anchors")
    print(out)
    check(rc == 0 and "RESULT: PASS" in out, "--anchors on a steady set: PASS")
    import re
    import math
    src = (SRC / "src" / "tests" / "ram" / "formula_v1.cpp").read_text(encoding="utf-8")
    names = re.findall(r"constexpr double (k\w+) = ", src)
    printed = re.findall(r"constexpr double (k\w+) = ([0-9.]+);", out)
    check([n for n, _ in printed] == [n for n in names if n != "kScale"],
          "--anchors: the constants are formula_v1.cpp's, in its order")
    want = {key: float(f"{statistics.median([r['metrics'][key] for r in runs]):.3g}") for key in RAM_METRICS}
    got = dict(printed)
    check(got.get("kDram") == f"{want['latency.dram']:g}" and got.get("kLoaded") == f"{want['loaded.latency']:g}"
          and got.get("kStCopy") == f"{want['bandwidth.st.copy']:g}",
          "--anchors: medians to 3 significant figures")
    check("run 1: 1,000 (stored 1,000)" in out, "--anchors: the median run re-scores to 1,000")
    check("median 1,000" in out, "--anchors: the re-scored median is 1,000")
    lowhuge = tmp / "lowhuge.db"
    make_ram_db(lowhuge, [ram_run(k, huge=(85.0 if k == 3 else 100.0)) for k in (0, 2, -1, 3, -2)])
    rc, out, _ = run(lowhuge, "--test", "ram", "--anchors")
    check(rc == 1 and "run 4: status ok, huge pages 85%  FAIL" in out, "--anchors: huge pages under 90% fail")
    rc, out, _ = run(lowhuge, "--test", "ram")
    check(rc == 0, "without --anchors low huge pages are only reported")
    check(run(good, "--anchors")[0] == 2, "--anchors with --test cpu: exit 2")
    rc, out, _ = run(ram, "--test", "ram", "-n", "6", "--anchors")
    check(rc == 1 and "only 5 of the 6 runs asked for  FAIL" in out, "--anchors: too few runs fail")

    # 8. --test gpu on the history tst_gpu_test wrote (five runs scored by the C++ formula).
    check(GPU_FIXTURE is not None and GPU_FIXTURE.is_file(), f"the GPU fixture history exists ({GPU_FIXTURE})")
    if GPU_FIXTURE is not None and GPU_FIXTURE.is_file():
        gpu_tests(tmp)

print(f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)

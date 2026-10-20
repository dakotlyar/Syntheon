#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""cpu-repeatability: how repeatable the CPU or RAM test is over saved runs.

Phase 2 design §14.3 / §14.4 (D17) and Phase 3 §14.3 / §14.4 (D19): the
validation gate asks for a CV of the test's score of at most 3% over 5 runs.
This reads a SynBench history (synbench.db, opened read-only) and prints, for
the chosen results of one test:

  * per run: its id, start time, score, the group sub-scores, and the
    conditions it ran in (power source, power profile, background load
    before the run; CPU: the single-thread mark and the throttling flag;
    RAM: DRAM latency, all-thread read, loaded latency and the huge-page
    coverage);
  * the CV (sample standard deviation / mean) of the score, of each group
    sub-score and (CPU) of the single-thread mark or (RAM) of every scored
    metric, with every CV above the limit flagged. A RAM metric that no run
    has (latency.l3 on a core without an L3) is listed as not measured and
    doesn't fail the set.

Usage:
  cpu-repeatability.py <synbench.db> [--test cpu|ram] [-n N] [--runs ID,ID,...] [--limit PCT]

  --test T    which test's results: cpu (the default) or ram
  -n N        the last N results of that test that have a score (default
              5), of the newest result's test and formula version
  --runs ...  these run ids instead (as the first column prints them), in
              the order given; an unscored result is shown, and left out of
              the CVs where its values are missing
  --limit PCT the CV limit, in percent (default 3)

For RAM there is no MLC cross-check (Phase 3 D19): the gate is this CV plus
a sensible machine ranking.

Exit code: 0 if every CV is within the limit, 1 if any is above it (or fewer
than 2 values could be compared), 2 for a usage or database problem.

Standard library only; it never modifies the history and never touches the
network. The database is opened read-only, but SynBench's history is in WAL
mode, so SQLite may leave a -shm file (and an empty -wal file) next to it
when SynBench isn't running; they hold no data and SynBench reuses them.
"""
import argparse
import math
import pathlib
import sqlite3
import statistics
import sys

# Per test: its groups in the order of its Sub-tests table, and the metrics
# whose CVs are printed after the groups'.
TESTS = {
    # Phase 2 §2: the seven groups, then the single-thread mark.
    "cpu": {
        "name": "CPU",
        "groups": [
            ("int", "Integer"),
            ("fp", "FP + primes"),
            ("simd", "SIMD"),
            ("compress", "Compression"),
            ("crypto", "Encryption"),
            ("sort", "Sorting"),
            ("physics", "Physics"),
        ],
        "metrics": [("single.mark", "Single-thread mark")],
    },
    # Phase 3 §2, §10: three groups, then every scored metric.
    "ram": {
        "name": "RAM",
        "groups": [
            ("latency", "Latency"),
            ("bandwidth", "Bandwidth"),
            ("loaded", "Loaded latency"),
        ],
        "metrics": [
            ("latency.dram", "DRAM latency"),
            ("latency.l3", "L3 latency"),
            ("latency.l2", "L2 latency"),
            ("latency.l1", "L1 latency"),
            ("bandwidth.read", "Read, all threads"),
            ("bandwidth.write", "Write, all threads"),
            ("bandwidth.copy", "Copy, all threads"),
            ("bandwidth.st.read", "Read, one thread"),
            ("bandwidth.st.write", "Write, one thread"),
            ("bandwidth.st.copy", "Copy, one thread"),
            ("loaded.latency", "Loaded latency (ns)"),
        ],
    },
}
# §15.1: RAM calibration runs need at least this huge-page coverage.
RAM_HUGE_PAGES_MIN = 90


def open_readonly(path):
    p = pathlib.Path(path)
    if not p.is_file():
        raise SystemExit(f"cpu-repeatability: no such file: {path}")
    # URI mode=ro: SQLite itself refuses every write, and a missing file is an
    # error instead of a new, empty database.
    uri = p.resolve().as_uri() + "?mode=ro"
    try:
        con = sqlite3.connect(uri, uri=True)
        con.execute("SELECT 1 FROM runs LIMIT 1")
        con.execute("SELECT 1 FROM test_results LIMIT 1")
    except sqlite3.Error as e:
        raise SystemExit(f"cpu-repeatability: {path} isn't a readable SynBench history ({e})")
    return con


def test_results(con, test, n, run_ids):
    q = (
        "SELECT r.id, r.started_at, r.status, r.machine_label, r.app_version, "
        "t.id, t.status, t.score, t.test_version, t.formula_version "
        "FROM runs r JOIN test_results t ON t.run_id = r.id AND t.test_id = ? "
    )
    if run_ids:
        rows = []
        for rid in run_ids:
            row = con.execute(q + "WHERE r.id = ?", (test, rid)).fetchone()
            if row is None:
                raise SystemExit(f"cpu-repeatability: run {rid} has no {TESTS[test]['name']} result in this history")
            rows.append(row)
        return rows
    # Only results comparable with the newest one: the same test and formula
    # version (a Phase 1 stub result or an older formula never mixes in).
    newest = con.execute(q + "WHERE t.score IS NOT NULL ORDER BY r.started_at DESC, r.id DESC LIMIT 1", (test,)).fetchone()
    if newest is None:
        return []
    rows = con.execute(q + "WHERE t.score IS NOT NULL AND t.test_version = ? AND t.formula_version = ? "
                           "ORDER BY r.started_at DESC, r.id DESC LIMIT ?", (test, newest[8], newest[9], n)).fetchall()
    return list(reversed(rows))  # oldest first


def metric(con, result_id, key):
    row = con.execute("SELECT value_num, value_text FROM metrics WHERE test_result_id = ? AND key = ?",
                      (result_id, key)).fetchone()
    if row is None:
        return None
    return row[0] if row[0] is not None else row[1]


def sub_scores(con, result_id):
    return {sub: score for sub, score in
            con.execute("SELECT sub_id, score FROM sub_results WHERE test_result_id = ?", (result_id,))}


def cv_percent(values):
    vals = [v for v in values if v is not None]
    if len(vals) < 2:
        return None
    mean = statistics.fmean(vals)
    if mean == 0:
        return None
    return statistics.stdev(vals) / mean * 100.0


def fmt_num(v, digits=0):
    if v is None:
        return "—"
    if isinstance(v, str):
        return v
    return f"{v:,.{digits}f}"


def main(argv=None):
    ap = argparse.ArgumentParser(prog="cpu-repeatability.py",
                                 description="CV of the CPU or RAM score, its group sub-scores and its main metrics over "
                                             "saved runs.",
                                 epilog="The history is opened read-only and never modified. It is in WAL mode, so "
                                        "SQLite may leave a -shm file (and an empty -wal file) next to it when "
                                        "SynBench isn't running; they hold no data.")
    ap.add_argument("db", help="path to synbench.db (opened read-only)")
    ap.add_argument("--test", choices=sorted(TESTS), default="cpu", help="which test's results (default cpu)")
    ap.add_argument("-n", type=int, default=5, help="the last N scored results (default 5)")
    ap.add_argument("--runs", help="comma-separated run ids instead of the last N")
    ap.add_argument("--limit", type=float, default=3.0, help="CV limit in percent (default 3)")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    run_ids = None
    if args.runs:
        try:
            run_ids = [int(x) for x in args.runs.split(",") if x.strip()]
        except ValueError:
            print("cpu-repeatability: --runs takes run ids, such as 3,4,5", file=sys.stderr)
            return 2
    if args.n < 1 or not math.isfinite(args.limit) or args.limit <= 0:
        print("cpu-repeatability: -n must be at least 1 and --limit a positive number", file=sys.stderr)
        return 2
    spec = TESTS[args.test]
    name, groups = spec["name"], spec["groups"]
    try:
        con = open_readonly(args.db)
        rows = test_results(con, args.test, args.n, run_ids)
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2
    if not rows:
        print(f"cpu-repeatability: no scored {name} results in this history", file=sys.stderr)
        return 2

    runs = []
    for (rid, started, rstatus, machine, appver, tid, tstatus, score, tver, fver) in rows:
        subs = sub_scores(con, tid)
        bg = metric(con, tid, "background.load")
        runs.append({
            "id": rid, "started": started, "run_status": rstatus, "machine": machine, "app": appver,
            "status": tstatus, "score": score, "test_version": tver, "formula_version": fver,
            "subs": {g: subs.get(g) for g, _ in groups},
            "metrics": {k: metric(con, tid, k) for k, _ in spec["metrics"]},
            "power": metric(con, tid, "power.source"),
            "profile": metric(con, tid, "power.profile"),
            "background": bg,
            "throttle": metric(con, tid, "throttle"),
            "huge": metric(con, tid, "pages.huge"),
        })
    con.close()

    # --- per run --------------------------------------------------------------
    print(f"{name} repeatability: {len(runs)} run(s) from {args.db}")
    if args.test == "cpu":
        extra_head = ["ST mark"]
        extra = lambda r: [fmt_num(r["metrics"]["single.mark"])]
        tail_head = ["throttling"]
        tail = lambda r: [r["throttle"] or "—"]
    else:
        extra_head = ["DRAM ns", "read GB/s", "loaded ns"]
        extra = lambda r: [fmt_num(r["metrics"]["latency.dram"], 1), fmt_num(r["metrics"]["bandwidth.read"], 1),
                           fmt_num(r["metrics"]["loaded.latency"], 1)]
        tail_head = ["huge pages"]
        tail = lambda r: [f"{r['huge']:.0f}%" if isinstance(r["huge"], (int, float)) else (r["huge"] or "—")]
    head = ["run", "started (UTC)", "status", "score"] + [label for _, label in groups] + \
           extra_head + ["power", "profile", "bg load"] + tail_head
    table = []
    for r in runs:
        bg = r["background"]
        table.append([str(r["id"]), (r["started"] or "")[:19].replace("T", " "), r["status"], fmt_num(r["score"])]
                     + [fmt_num(r["subs"][g]) for g, _ in groups]
                     + extra(r)
                     + [r["power"] or "—", r["profile"] or "—",
                        f"{bg:.1f}%" if isinstance(bg, (int, float)) else (bg or "—")]
                     + tail(r))
    widths = [max(len(h), *(len(row[i]) for row in table)) for i, h in enumerate(head)]
    print("  ".join(h.ljust(w) for h, w in zip(head, widths)))
    for row in table:
        print("  ".join(c.ljust(w) for c, w in zip(row, widths)))

    # --- CVs -----------------------------------------------------------------
    print()
    print(f"CV (sample standard deviation / mean), limit {args.limit:g}%:")
    failed = False
    # (label, values, may be absent from every run)
    series = [(f"{name} score", [r["score"] for r in runs], False)] + \
             [(label, [r["subs"][g] for r in runs], False) for g, label in groups] + \
             [(label, [r["metrics"][k] for r in runs], args.test == "ram") for k, label in spec["metrics"]]
    width = max(20, *(len(label) for label, _, _ in series))
    for label, values, optional in series:
        used = sum(isinstance(v, (int, float)) for v in values)
        if optional and used == 0:
            # A RAM metric no run has: not on this machine (latency.l3
            # without an L3).
            print(f"  {label:<{width}} —      (not measured on these runs)")
            continue
        cv = cv_percent([v if isinstance(v, (int, float)) else None for v in values])
        if cv is None:
            failed = True
            print(f"  {label:<{width}} —      (only {used} value(s): nothing to compare)  FAIL")
            continue
        flag = "FAIL" if cv > args.limit else "ok"
        failed = failed or cv > args.limit
        mean = statistics.fmean([v for v in values if isinstance(v, (int, float))])
        print(f"  {label:<{width}} {cv:5.2f}%  (n={used}, mean {mean:,.{3 if mean < 10 else 1}f})  {flag}")

    # --- conditions that make a set not comparable ---------------------------------
    notes = []
    if not run_ids and len(runs) < args.n:
        notes.append(f"only {len(runs)} comparable scored result(s), of the {args.n} asked for")
    if len({r["machine"] for r in runs}) > 1:
        notes.append("the runs are from different machines")
    if len({(r["test_version"], r["formula_version"]) for r in runs}) > 1:
        notes.append(f"the runs use different {name} test or formula versions")
    for r in runs:
        why = []
        if r["status"] != "ok":
            why.append(f"status {r['status']}")
        if r["power"] not in (None, "AC"):
            why.append(f"power {r['power']}")
        if isinstance(r["background"], (int, float)) and r["background"] >= 10:
            why.append(f"background load {r['background']:.0f}%")
        if r["throttle"] not in (None, "Not detected"):
            why.append(f"throttling {r['throttle'].lower()}")
        if args.test == "ram" and isinstance(r["huge"], (int, float)) and r["huge"] < RAM_HUGE_PAGES_MIN:
            why.append(f"huge pages {r['huge']:.0f}% (calibration needs {RAM_HUGE_PAGES_MIN}%)")
        if why:
            notes.append(f"run {r['id']}: " + ", ".join(why))
    if notes:
        print()
        print("Conditions to check:")
        for n in notes:
            print(f"  - {n}")
    print()
    print("RESULT: " + ("FAIL (a CV is above the limit, or there is too little to compare)" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

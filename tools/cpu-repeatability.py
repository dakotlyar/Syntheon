#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""cpu-repeatability: how repeatable the CPU test is over a set of saved runs.

Phase 2 design §14.3 / §14.4 (D17): the validation gate asks for a CV of the
CPU score of at most 3% over 5 runs. This reads a SynBench history
(synbench.db, opened read-only) and prints, for the chosen CPU results:

  * per run: its id, start time, score, the seven group sub-scores, the
    single-thread mark, and the conditions it ran in (power source, power
    profile, background load before the run, the throttling flag);
  * the CV (sample standard deviation / mean) of the CPU score, of each group
    sub-score and of the single-thread mark, with every CV above the limit
    flagged.

Usage:
  cpu-repeatability.py <synbench.db> [-n N] [--runs ID,ID,...] [--limit PCT]

  -n N        the last N CPU results that have a score (default 5), of the
              newest result's CPU test and formula version
  --runs ...  these run ids instead (as the first column prints them), in
              the order given; an unscored result is shown, and left out of
              the CVs where its values are missing
  --limit PCT the CV limit, in percent (default 3)

Exit code: 0 if every CV is within the limit, 1 if any is above it (or fewer
than 2 values could be compared), 2 for a usage or database problem.

Standard library only; it never writes to the database and never touches the
network.
"""
import argparse
import math
import pathlib
import sqlite3
import statistics
import sys

# The seven groups in the order of the CPU test's Sub-tests table (§2).
GROUPS = [
    ("int", "Integer"),
    ("fp", "FP + primes"),
    ("simd", "SIMD"),
    ("compress", "Compression"),
    ("crypto", "Encryption"),
    ("sort", "Sorting"),
    ("physics", "Physics"),
]


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


def cpu_results(con, n, run_ids):
    q = (
        "SELECT r.id, r.started_at, r.status, r.machine_label, r.app_version, "
        "t.id, t.status, t.score, t.test_version, t.formula_version "
        "FROM runs r JOIN test_results t ON t.run_id = r.id AND t.test_id = 'cpu' "
    )
    if run_ids:
        rows = []
        for rid in run_ids:
            row = con.execute(q + "WHERE r.id = ?", (rid,)).fetchone()
            if row is None:
                raise SystemExit(f"cpu-repeatability: run {rid} has no CPU result in this history")
            rows.append(row)
        return rows
    # Only results comparable with the newest one: the same CPU test and
    # formula version (a Phase 1 stub result or an older formula never mixes in).
    newest = con.execute(q + "WHERE t.score IS NOT NULL ORDER BY r.started_at DESC, r.id DESC LIMIT 1").fetchone()
    if newest is None:
        return []
    rows = con.execute(q + "WHERE t.score IS NOT NULL AND t.test_version = ? AND t.formula_version = ? "
                           "ORDER BY r.started_at DESC, r.id DESC LIMIT ?", (newest[8], newest[9], n)).fetchall()
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
                                 description="CV of the CPU score, group sub-scores and single-thread mark over saved runs.")
    ap.add_argument("db", help="path to synbench.db (opened read-only)")
    ap.add_argument("-n", type=int, default=5, help="the last N scored CPU results (default 5)")
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
    try:
        con = open_readonly(args.db)
        rows = cpu_results(con, args.n, run_ids)
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2
    if not rows:
        print("cpu-repeatability: no scored CPU results in this history", file=sys.stderr)
        return 2

    runs = []
    for (rid, started, rstatus, machine, appver, tid, tstatus, score, tver, fver) in rows:
        subs = sub_scores(con, tid)
        bg = metric(con, tid, "background.load")
        runs.append({
            "id": rid, "started": started, "run_status": rstatus, "machine": machine, "app": appver,
            "status": tstatus, "score": score, "test_version": tver, "formula_version": fver,
            "subs": {g: subs.get(g) for g, _ in GROUPS},
            "mark": metric(con, tid, "single.mark"),
            "power": metric(con, tid, "power.source"),
            "profile": metric(con, tid, "power.profile"),
            "background": bg,
            "throttle": metric(con, tid, "throttle"),
        })
    con.close()

    # --- per run --------------------------------------------------------------
    print(f"CPU repeatability: {len(runs)} run(s) from {args.db}")
    head = ["run", "started (UTC)", "status", "score"] + [label for _, label in GROUPS] + \
           ["ST mark", "power", "profile", "bg load", "throttling"]
    table = []
    for r in runs:
        bg = r["background"]
        table.append([str(r["id"]), (r["started"] or "")[:19].replace("T", " "), r["status"], fmt_num(r["score"])]
                     + [fmt_num(r["subs"][g]) for g, _ in GROUPS]
                     + [fmt_num(r["mark"]), r["power"] or "—", r["profile"] or "—",
                        f"{bg:.1f}%" if isinstance(bg, (int, float)) else (bg or "—"), r["throttle"] or "—"])
    widths = [max(len(h), *(len(row[i]) for row in table)) for i, h in enumerate(head)]
    print("  ".join(h.ljust(w) for h, w in zip(head, widths)))
    for row in table:
        print("  ".join(c.ljust(w) for c, w in zip(row, widths)))

    # --- CVs -----------------------------------------------------------------
    print()
    print(f"CV (sample standard deviation / mean), limit {args.limit:g}%:")
    failed = False
    series = [("CPU score", [r["score"] for r in runs])] + \
             [(label, [r["subs"][g] for r in runs]) for g, label in GROUPS] + \
             [("Single-thread mark", [r["mark"] for r in runs])]
    for name, values in series:
        cv = cv_percent(values)
        used = sum(v is not None for v in values)
        if cv is None:
            failed = True
            print(f"  {name:<20} —      (only {used} value(s): nothing to compare)  FAIL")
            continue
        flag = "FAIL" if cv > args.limit else "ok"
        failed = failed or cv > args.limit
        print(f"  {name:<20} {cv:5.2f}%  (n={used}, mean {statistics.fmean([v for v in values if v is not None]):,.1f})  {flag}")

    # --- conditions that make a set not comparable ---------------------------------
    notes = []
    if not run_ids and len(runs) < args.n:
        notes.append(f"only {len(runs)} comparable scored result(s), of the {args.n} asked for")
    if len({r["machine"] for r in runs}) > 1:
        notes.append("the runs are from different machines")
    if len({(r["test_version"], r["formula_version"]) for r in runs}) > 1:
        notes.append("the runs use different CPU test or formula versions")
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

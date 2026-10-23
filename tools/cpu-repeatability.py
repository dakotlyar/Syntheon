#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""cpu-repeatability: how repeatable the CPU, RAM or GPU test is over saved runs.

Phase 2 design §14.3 / §14.4 (D17), Phase 3 §14.3 / §14.4 (D19) and Phase 4a
§17.4 (task 35): the validation gate asks for a CV of the test's score of at
most 3% over 5 runs.
This reads a SynBench history (synbench.db, opened read-only) and prints, for
the chosen results of one test:

  * per run: its id, start time, score, the group sub-scores, and the
    conditions it ran in (power source, power profile, background load
    before the run; CPU: the single-thread mark and the throttling flag;
    RAM: DRAM latency, all-thread read, loaded latency and the huge-page
    coverage; GPU: Geometry fps, Quads Gpx/s and the GPU's name);
  * the CV (sample standard deviation / mean) of the score, of each group
    sub-score and (CPU) of the single-thread mark or (RAM, GPU) of every
    scored metric, with every CV above the limit flagged. latency.l3 missing from
    every run (a core without an L3) is listed as not measured and doesn't
    fail the set; any other scored metric missing fails it.

Usage:
  cpu-repeatability.py <synbench.db> [--test cpu|ram|gpu] [-n N] [--runs ID,ID,...] [--limit PCT] [--anchors]

  --test T    which test's results: cpu (the default), ram or gpu
  -n N        the last N results of that test that have a score (default
              5), of the newest result's test and formula version; with
              --test gpu the last N results whatever their status (a Partial
              one has no score: it is named, doesn't count, and so fewer than
              N Ok runs fail the gate)
  --runs ...  these run ids instead (as the first column prints them), in
              the order given; an unscored result is shown, and left out of
              the CVs where its values are missing
  --limit PCT the CV limit, in percent (default 3)
  --anchors   RAM calibration (Phase 3 §15.1, §10.3): also gate every run
              on status ok and huge pages >= 90%, print each scored
              metric's median rounded to 3 significant figures (the new
              anchors, as formula_v1.cpp's constants), and each run
              re-scored with them (formula v1, §10.2) with that score's CV;
              the median run then scores about 1,000. A re-scored CV equals
              the stored score's (anchors scale every run alike); it is
              printed as the check. Exit 1 if any gate fails.
              GPU calibration (Phase 4a §17.4) is the same with --test gpu:
              the gate is status ok on every run (a Partial run, such as a
              signature outlier, doesn't count and is reported) on one
              machine, and the GPU score's CV. Every group's and every
              scored metric's CV is reported, never gated: above 2% is
              "above target", above the limit says so too. The ten constants are formula_v1.cpp's
              (kGeometryFps .. kSpritesMops), and the runs are also
              re-scored with the constants that file has NOW: the check that
              this script's formula is the C++ one. Those scores equal the
              stored ones when the runs were made with the same constants;
              a run stored under older anchors differs, which is reported
              and doesn't fail the gate.

For RAM there is no MLC cross-check (Phase 3 D19): the gate is this CV plus
a sensible machine ranking.

Exit code: 0 if every CV is within the limit, 1 if any is above it (or fewer
than 2 values could be compared), 2 for a usage or database problem. With
--test gpu only the score's CV gates (with every run Ok, on one machine).

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
        # Absent from every run on a core without an L3 (Phase 3 D1).
        "optional": ("latency.l3",),
    },
    # Phase 4a §10, §11.1: the three groups, then every scored metric (Qt raster's native numbers, the best API's fill rates).
    "gpu": {
        "name": "GPU",
        "groups": [
            ("scenes3d", "3D scenes"),
            ("native2d", "Native 2D"),
            ("fill2d", "2D fill-rate"),
        ],
        "metrics": [
            ("scenes3d.geometry.fps", "Geometry (fps)"),
            ("scenes3d.drawcalls.fps", "Draw calls (fps)"),
            ("scenes3d.shading.fps", "Shading (fps)"),
            ("scenes3d.particles.fps", "Particles (fps)"),
            ("native2d.shapes", "Shapes (MOps/s)"),
            ("native2d.text", "Text (MOps/s)"),
            ("native2d.blits", "Blits (MOps/s)"),
            ("native2d.gradients", "Gradients (MOps/s)"),
            ("fill2d.quads", "Quads (Gpx/s)"),
            ("fill2d.sprites", "Sprites (MOps/s)"),
        ],
    },
}
# §15.1: RAM calibration runs need at least this huge-page coverage.
RAM_HUGE_PAGES_MIN = 90
# Phase 4a §17.4: each scored GPU metric's CV is reported against this target (the gate is the score's CV).
GPU_METRIC_CV_TARGET = 2.0

# Formula v1 (Phase 3 §10.2, src/tests/ram/formula_v1.cpp), for --anchors:
# group -> (weight, [(metric, weight in group, lower is better)]); the
# latency group without an L3 moves L3's 0.15 to L2 (0.20).
RAM_FORMULA = [
    ("latency", 0.50, [("latency.dram", 0.75, True), ("latency.l3", 0.15, True), ("latency.l2", 0.05, True),
                       ("latency.l1", 0.05, True)]),
    ("bandwidth", 0.35, [("bandwidth.read", 2 / 9, False), ("bandwidth.write", 2 / 9, False),
                         ("bandwidth.copy", 2 / 9, False), ("bandwidth.st.read", 1 / 9, False),
                         ("bandwidth.st.write", 1 / 9, False), ("bandwidth.st.copy", 1 / 9, False)]),
    ("loaded", 0.15, [("loaded.latency", 1.0, True)]),
]
# formula_v1.cpp's anchor constants, in its order.
RAM_ANCHOR_NAMES = [("kL1", "latency.l1", "ns"), ("kL2", "latency.l2", "ns"), ("kL3", "latency.l3", "ns"),
                    ("kDram", "latency.dram", "ns"), ("kRead", "bandwidth.read", "GB/s, all threads"),
                    ("kWrite", "bandwidth.write", "GB/s, all threads, non-temporal stores"),
                    ("kCopy", "bandwidth.copy", "GB/s, all threads, read + written"),
                    ("kStRead", "bandwidth.st.read", "GB/s, single thread"), ("kStWrite", "bandwidth.st.write", ""),
                    ("kStCopy", "bandwidth.st.copy", ""), ("kLoaded", "loaded.latency", "ns, full load")]


# Formula v1 (Phase 4a §11.1, src/tests/gpu/formula_v1.cpp): group -> (weight, [(metric, weight in group)]); every
# scored metric is higher-is-better, so n = metric / anchor.
GPU_FORMULA = [
    ("scenes3d", 0.80, [("scenes3d.geometry.fps", 0.20), ("scenes3d.drawcalls.fps", 0.20),
                        ("scenes3d.shading.fps", 0.30), ("scenes3d.particles.fps", 0.30)]),
    ("native2d", 0.10, [("native2d.shapes", 0.25), ("native2d.text", 0.25), ("native2d.blits", 0.25),
                        ("native2d.gradients", 0.25)]),
    ("fill2d", 0.10, [("fill2d.quads", 0.50), ("fill2d.sprites", 0.50)]),
]
# formula_v1.cpp's anchor constants, in its order (name, metric, unit).
GPU_ANCHOR_NAMES = [("kGeometryFps", "scenes3d.geometry.fps", "fps"), ("kDrawcallsFps", "scenes3d.drawcalls.fps", "fps"),
                    ("kShadingFps", "scenes3d.shading.fps", "fps"), ("kParticlesFps", "scenes3d.particles.fps", "fps"),
                    ("kShapes", "native2d.shapes", "Qt raster, MOps/s"), ("kText", "native2d.text", ""),
                    ("kBlits", "native2d.blits", ""), ("kGradients", "native2d.gradients", ""),
                    ("kQuadsGpx", "fill2d.quads", "Gpx/s, the best API's"), ("kSpritesMops", "fill2d.sprites", "MOps/s, the best API's")]
# The constants formula_v1.cpp has now (PROVISIONAL, the real-display Knowhere run of 2026-10-22; task 36 replaces them).
# tests/tools/test_cpu_repeatability.py compares this table with that file, so an edit of one without the other fails.
GPU_CURRENT_ANCHORS = {"scenes3d.geometry.fps": 673.0, "scenes3d.drawcalls.fps": 265.0, "scenes3d.shading.fps": 119.0,
                       "scenes3d.particles.fps": 96.0, "native2d.shapes": 0.0191, "native2d.text": 1.81,
                       "native2d.blits": 0.0130, "native2d.gradients": 0.0208, "fill2d.quads": 74.3,
                       "fill2d.sprites": 33.8}


def sig3(x):
    """x rounded to 3 significant figures."""
    return float(f"{x:.3g}")


def sig3_text(x):
    t = f"{sig3(x):.6f}".rstrip("0").rstrip(".")
    return t


def ram_rescore(metrics, anchors):
    """The RAM score of one run's metrics with these anchors (formula v1);
    None if a scored metric (or its anchor) is missing."""
    has_l3 = isinstance(metrics.get("latency.l3"), (int, float))
    total = 0.0
    for _, w_group, items in RAM_FORMULA:
        ln_g = 0.0
        for key, v, lower in items:
            if key == "latency.l3" and not has_l3:
                continue
            if key == "latency.l2" and not has_l3:
                v = 0.20
            r, a = metrics.get(key), anchors.get(key)
            if not isinstance(r, (int, float)) or not a or r <= 0:
                return None
            ln_g += v * math.log(a / r if lower else r / a)
        total += w_group * ln_g
    return round(1000.0 * math.exp(total))


def gpu_rescore(metrics, anchors):
    """The GPU score of one run's metrics with these anchors (formula v1, as weightedGeometricScore() builds it):
    (total, {group: sub-score}, 2D score), each None when a metric it needs (or its anchor) is missing or not positive.
    The total is round(1000 x exp(sum W.ln G)), a sub-score round(1000 x G), and the 2D score
    round(1000 x sqrt(G_native x G_fill)) from the unrounded groups (formula_v1::score2d)."""
    # std::round rounds halves away from zero, Python's round() to even: the scores here are positive.
    def half_up(x):
        return float(math.floor(x + 0.5))

    logs, subs, complete = {}, {}, True
    for gid, _, items in GPU_FORMULA:
        ln_g, present = 0.0, True
        for key, w in items:
            r, a = metrics.get(key), anchors.get(key)
            if not isinstance(r, (int, float)) or not r > 0 or not a or not a > 0:
                present = False
                break
            ln_g += w * math.log(r / a)
        if not present:
            complete = False
            continue
        logs[gid] = ln_g
        subs[gid] = half_up(1000.0 * math.exp(ln_g))
    log_total = 0.0
    for g, w, _ in GPU_FORMULA:
        if g in logs:
            log_total += w * logs[g]
    total = half_up(1000.0 * math.exp(log_total)) if complete else None
    # score2d() asks for the groups at a 1e9 scale, so rounding them costs nothing.
    two_d = None
    if "native2d" in logs and "fill2d" in logs:
        native, fill = half_up(1e9 * math.exp(logs["native2d"])), half_up(1e9 * math.exp(logs["fill2d"]))
        two_d = half_up(1000.0 * math.sqrt((native / 1e9) * (fill / 1e9)))
    return total, subs, two_d


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
        "t.id, t.status, t.score, t.test_version, t.formula_version, t.status_reason "
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
    # GPU (§17.4): the newest N results whatever their status, since a Partial one has no score and must be named, not
    # skipped; the CPU and RAM tests take scored results only, as Phase 2 and 3 defined.
    scored = "" if test == "gpu" else "t.score IS NOT NULL AND "
    newest = con.execute(q + f"WHERE {scored}1 ORDER BY r.started_at DESC, r.id DESC LIMIT 1", (test,)).fetchone()
    if newest is None:
        return []
    rows = con.execute(q + f"WHERE {scored}t.test_version = ? AND t.formula_version = ? "
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
                                 description="CV of the CPU, RAM or GPU score, its group sub-scores and its main metrics over "
                                             "saved runs.",
                                 epilog="The history is opened read-only and never modified. It is in WAL mode, so "
                                        "SQLite may leave a -shm file (and an empty -wal file) next to it when "
                                        "SynBench isn't running; they hold no data.")
    ap.add_argument("db", help="path to synbench.db (opened read-only)")
    ap.add_argument("--test", choices=sorted(TESTS), default="cpu", help="which test's results (default cpu)")
    ap.add_argument("-n", type=int, default=5, help="the last N scored results (default 5)")
    ap.add_argument("--runs", help="comma-separated run ids instead of the last N")
    ap.add_argument("--limit", type=float, default=3.0, help="CV limit in percent (default 3)")
    ap.add_argument("--anchors", action="store_true",
                    help="RAM or GPU calibration: gate the runs, print the median anchors (3 significant "
                         "figures) and the runs re-scored with them")
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
    if args.anchors and args.test not in ("ram", "gpu"):
        print("cpu-repeatability: --anchors is for --test ram or --test gpu", file=sys.stderr)
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
    for (rid, started, rstatus, machine, appver, tid, tstatus, score, tver, fver, treason) in rows:
        subs = sub_scores(con, tid)
        bg = metric(con, tid, "background.load")
        runs.append({
            "id": rid, "started": started, "run_status": rstatus, "machine": machine, "app": appver,
            "status": tstatus, "reason": treason, "score": score, "test_version": tver, "formula_version": fver,
            "subs": {g: subs.get(g) for g, _ in groups},
            "metrics": {k: metric(con, tid, k) for k, _ in spec["metrics"]},
            "power": metric(con, tid, "power.source"),
            "profile": metric(con, tid, "power.profile"),
            "background": bg,
            "throttle": metric(con, tid, "throttle"),
            "huge": metric(con, tid, "pages.huge"),
            "gpu": metric(con, tid, "gpu.name"),
            "driver": metric(con, tid, "gpu.driver"),
            "render": metric(con, tid, "render.size"),
            "window": metric(con, tid, "window.size"),
            "stored3d": metric(con, tid, "score.3d"),
            "stored2d": metric(con, tid, "score.2d"),
            "api_status": {k: v for k, v in con.execute(
                "SELECT key, value_text FROM metrics WHERE test_result_id = ? AND key LIKE 'api.%.status'", (tid,))},
        })
    con.close()

    # --- per run --------------------------------------------------------------
    print(f"{name} repeatability: {len(runs)} run(s) from {args.db}")
    # GPU (§17.4): only Ok runs count. A Partial (unscored) or otherwise not-Ok run is shown and named, and fails the gate.
    gpu_mode = args.test == "gpu"
    full = runs
    if gpu_mode:
        runs = [r for r in full if r["status"] == "ok"]
    not_counted = [r for r in full if r not in runs]
    if args.test == "cpu":
        extra_head = ["ST mark"]
        extra = lambda r: [fmt_num(r["metrics"]["single.mark"])]
        tail_head = ["throttling"]
        tail = lambda r: [r["throttle"] or "—"]
    elif args.test == "ram":
        extra_head = ["DRAM ns", "read GB/s", "loaded ns"]
        extra = lambda r: [fmt_num(r["metrics"]["latency.dram"], 1), fmt_num(r["metrics"]["bandwidth.read"], 1),
                           fmt_num(r["metrics"]["loaded.latency"], 1)]
        tail_head = ["huge pages"]
        tail = lambda r: [f"{r['huge']:.0f}%" if isinstance(r["huge"], (int, float)) else (r["huge"] or "—")]
    else:
        extra_head = ["geometry fps", "quads Gpx/s"]
        extra = lambda r: [fmt_num(r["metrics"]["scenes3d.geometry.fps"], 1), fmt_num(r["metrics"]["fill2d.quads"], 1)]
        tail_head = ["GPU"]
        tail = lambda r: [r["gpu"] or "—"]
    head = ["run", "started (UTC)", "status", "score"] + [label for _, label in groups] + \
           extra_head + ["power", "profile", "bg load"] + tail_head
    table = []
    for r in full:
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
             [(label, [r["metrics"][k] for r in runs], k in spec.get("optional", ())) for k, label in spec["metrics"]]
    width = max(20, *(len(label) for label, _, _ in series))
    # §17.4: under --test gpu only the score's CV gates; each group's and metric's CV is reported.
    gpu_reported = {label for _, label in groups} | {label for _, label in spec["metrics"]}
    for label, values, optional in series:
        used = sum(isinstance(v, (int, float)) for v in values)
        if optional and used == 0:
            # latency.l3, which no run has: a core without an L3. Any other
            # metric missing everywhere fails the set (AP-5 L4).
            print(f"  {label:<{width}} —      (not measured on these runs)")
            continue
        cv = cv_percent([v if isinstance(v, (int, float)) else None for v in values])
        reported_only = gpu_mode and label in gpu_reported
        if cv is None:
            failed = failed or not reported_only
            print(f"  {label:<{width}} —      (only {used} value(s): nothing to compare)  " + ("not compared" if reported_only else "FAIL"))
            continue
        if reported_only:
            # Reported, never gated: above 2% is "above target", above the limit says so as well.
            flag = "ok"
            if cv > args.limit:
                flag = f"above target ({GPU_METRIC_CV_TARGET:g}%) and above {args.limit:g}% (reported, not a gate)"
            elif cv > GPU_METRIC_CV_TARGET:
                flag = f"above target ({GPU_METRIC_CV_TARGET:g}%)"
        else:
            flag = "FAIL" if cv > args.limit else "ok"
            failed = failed or cv > args.limit
        mean = statistics.fmean([v for v in values if isinstance(v, (int, float))])
        print(f"  {label:<{width}} {cv:5.2f}%  (n={used}, mean {mean:,.{3 if mean < 10 else 1}f})  {flag}")

    # --- conditions that make a set not comparable ---------------------------------
    notes = []
    if gpu_mode:
        # Every not-Ok run is named, and the gate fails: "every run Ok" (§17.4).
        for r in not_counted:
            reason = r["reason"] or "no reason stored"
            notes.append(f"run {r['id']}: {r['status']}, {reason}, doesn't count")
        if not_counted and not run_ids:
            failed = True
            notes.append(f"not enough Ok runs: {len(runs)} of the {args.n} asked for  FAIL (not counted: "
                         + ", ".join(f"run {r['id']} {r['status']}" for r in not_counted) + ")")
        elif not_counted:
            failed = True
            notes.append("not every run is Ok  FAIL (not counted: " + ", ".join(f"run {r['id']} {r['status']}" for r in not_counted) + ")")
        elif not run_ids and len(runs) < args.n:
            failed = True
            notes.append(f"not enough Ok runs: {len(runs)} of the {args.n} asked for  FAIL")
    elif not run_ids and len(runs) < args.n:
        notes.append(f"only {len(runs)} comparable scored result(s), of the {args.n} asked for")
    if len({r["machine"] for r in runs}) > 1:
        notes.append("the runs are from different machines")
    if len({(r["test_version"], r["formula_version"]) for r in runs}) > 1:
        notes.append(f"the runs use different {name} test or formula versions")
    if args.test == "gpu":
        for what, key in (("GPUs", "gpu"), ("drivers", "driver"), ("render sizes", "render")):
            if len({r[key] for r in runs}) > 1:
                notes.append(f"the runs have different {what}: " + ", ".join(sorted({str(r[key]) for r in runs})))
    for r in runs:
        why = []
        if r["status"] != "ok":  # CPU and RAM only: the GPU's not-Ok runs are named above
            why.append(f"status {r['status']}")
        if r["power"] not in (None, "AC"):
            why.append(f"power {r['power']}")
        if isinstance(r["background"], (int, float)) and r["background"] >= 10:
            why.append(f"background load {r['background']:.0f}%")
        if r["throttle"] not in (None, "Not detected"):
            why.append(f"throttling {r['throttle'].lower()}")
        if args.test == "ram" and isinstance(r["huge"], (int, float)) and r["huge"] < RAM_HUGE_PAGES_MIN:
            why.append(f"huge pages {r['huge']:.0f}% (calibration needs {RAM_HUGE_PAGES_MIN}%)")
        if args.test == "gpu":
            bad = [k.split(".")[1] for k, v in sorted(r["api_status"].items()) if v and v.startswith("Error")]
            if bad:
                why.append("API error: " + ", ".join(bad))
            if isinstance(r["window"], str) and "scaled to fit" in r["window"]:
                why.append("the window was scaled to fit the screen")
        if why:
            notes.append(f"run {r['id']}: " + ", ".join(why))
    if notes:
        print()
        print("Conditions to check:")
        for n in notes:
            print(f"  - {n}")

    # --- RAM and GPU calibration (Phase 3 §15.1, §10.3; Phase 4a §17.4) ---------------------
    if args.anchors:
        gpu = args.test == "gpu"
        print()
        if gpu:
            print(f"Calibration gate (§17.4): {len(full)} run(s), every one status ok (a Partial run doesn't count):")
        else:
            print(f"Calibration gate (§15.1): {len(runs)} run(s), status ok, huge pages >= {RAM_HUGE_PAGES_MIN}%:")
        if not gpu and not run_ids and len(runs) < args.n:  # the GPU's count is the "not enough Ok runs" note above
            failed = True
            print(f"  only {len(runs)} of the {args.n} runs asked for  FAIL")
        if len({r["machine"] for r in runs}) > 1:
            failed = True
            print("  the runs are from different machines  FAIL")
        for r in (full if gpu else runs):
            if gpu:
                ok = r["status"] == "ok"
                failed = failed or not ok
                print(f"  run {r['id']}: status {r['status']}  " + ("ok" if ok else "FAIL (doesn't count)"))
                continue
            huge = r["huge"]
            ok = r["status"] == "ok" and isinstance(huge, (int, float)) and huge >= RAM_HUGE_PAGES_MIN
            failed = failed or not ok
            huge_text = f"{huge:.0f}%" if isinstance(huge, (int, float)) else "unknown"
            print(f"  run {r['id']}: status {r['status']}, huge pages {huge_text}  {'ok' if ok else 'FAIL'}")
        anchors = {}
        print()
        print("Anchors: the median of each scored metric, 3 significant figures (formula_v1.cpp):")
        for aname, key, unit in (GPU_ANCHOR_NAMES if gpu else RAM_ANCHOR_NAMES):
            vals = [r["metrics"][key] for r in runs if isinstance(r["metrics"][key], (int, float))]
            if not vals:
                print(f"  // {aname}: {key} not measured on these runs; keep the current value")
                continue
            anchors[key] = sig3(statistics.median(vals))
            comment = f"  // {unit}" if unit else ""
            print(f"  constexpr double {aname} = {sig3_text(anchors[key])};{comment}")
        print()
        if gpu:
            results = [gpu_rescore(r["metrics"], anchors) for r in runs]
            rescored = [total for total, _, _ in results]
            print("Each run re-scored with these anchors (formula v1):")
            for r, (total, subs, two_d) in zip(runs, results):
                parts = " · ".join(f"{label} {fmt_num(subs.get(g))}" for g, label in groups)
                print(f"  run {r['id']}: {fmt_num(total)} (stored {fmt_num(r['score'])})  {parts} · 2D {fmt_num(two_d)}")
        else:
            rescored = [ram_rescore(r["metrics"], anchors) for r in runs]
            print("Each run re-scored with these anchors (formula v1):")
            for r, sc in zip(runs, rescored):
                print(f"  run {r['id']}: {fmt_num(sc)} (stored {fmt_num(r['score'])})")
        cv = cv_percent(rescored)
        if cv is None:
            failed = True
            print("  re-scored CV: —  (too few scores)  FAIL")
        else:
            flag = "FAIL" if cv > args.limit else "ok"
            failed = failed or cv > args.limit
            med = statistics.median([sc for sc in rescored if sc is not None])
            print(f"  re-scored CV {cv:.2f}%  {flag}; median {med:,.0f} (about 1,000 expected)")
        if gpu:
            # The check that this script's formula is formula_v1.cpp's: the constants that file has now. Equal to the
            # stored scores for runs made with them; a run stored under older anchors differs, and that is not a failure.
            print()
            print("Re-scored with the constants formula_v1.cpp has now (the stored score should equal this one, unless the "
                  "run was stored under other anchors: see git log of formula_v1.cpp):")
            for r in runs:
                total, subs, two_d = gpu_rescore(r["metrics"], GPU_CURRENT_ANCHORS)
                same = (total == r["score"] and all(subs.get(g) == r["subs"][g] for g, _ in groups)
                        and (r["stored3d"] is None or subs.get("scenes3d") == r["stored3d"])
                        and (r["stored2d"] is None or two_d == r["stored2d"]))
                print(f"  run {r['id']}: {fmt_num(total)} (stored {fmt_num(r['score'])})  "
                      + " · ".join(f"{label} {fmt_num(subs.get(g))} ({fmt_num(r['subs'][g])})" for g, label in groups)
                      + f" · 2D {fmt_num(two_d)} ({fmt_num(r['stored2d'])})  " + ("equal" if same else "differs"))
    print()
    print("RESULT: " + ("FAIL (the score's CV is above the limit, a run is not Ok, or there is too little to compare)" if failed and gpu_mode
                        else "FAIL (a CV is above the limit, or there is too little to compare)" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

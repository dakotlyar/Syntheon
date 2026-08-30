#!/usr/bin/env python3
"""
SynBench – Synthetic System Benchmark
======================================
Benchmarks CPU, GPU, NPU, RAM, and Disk and produces a composite score
using methodology similar to Novabench.

Usage:
  python synbench.py                      # Launch GUI
  python synbench.py --cli                # CLI, run all tests
  python synbench.py --cli --tests cpu ram disk
  python synbench.py --cli --json         # Machine-readable JSON output
  python synbench.py --cli --duration 10  # Longer test duration
"""

import argparse
import sys
import os

# Resolve the base directory whether running as a script or PyInstaller bundle.
# sys._MEIPASS is set by PyInstaller to the extraction directory of a frozen app.
if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
    _APP_DIR = sys._MEIPASS  # type: ignore[attr-defined]
else:
    _APP_DIR = os.path.dirname(os.path.abspath(__file__))

if _APP_DIR not in sys.path:
    sys.path.insert(0, _APP_DIR)

# Activate the side-car plugins directory so optional packages installed
# at runtime (cupy, pyopencl, onnxruntime) are importable.
try:
    from runtime import setup_path as _setup_path
    _setup_path()
except Exception:
    pass


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="synbench",
        description="SynBench – Synthetic System Benchmark",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument(
        "--cli", action="store_true",
        help="Run in CLI / headless mode (no GUI)",
    )
    p.add_argument(
        "--tests", nargs="+",
        choices=["cpu", "gpu", "npu", "ram", "disk"],
        metavar="TEST",
        help="Tests to run (default: all). Choices: cpu gpu npu ram disk",
    )
    p.add_argument(
        "--json", action="store_true",
        help="Output results as JSON (CLI mode only)",
    )
    p.add_argument(
        "--output", "-o", metavar="FILE",
        help="Write results to FILE (JSON) instead of stdout",
    )
    p.add_argument(
        "--disk-dir", metavar="DIR",
        help="Directory used for disk I/O test files (default: auto-detect a real disk)",
    )
    p.add_argument(
        "--install-runtimes", action="store_true",
        help="Detect GPU/NPU hardware and install missing optional packages, then exit",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# CLI runner
# ---------------------------------------------------------------------------

def _cli_install_runtimes() -> int:
    """Detect hardware, report recommendations, and install missing packages."""
    from runtime import get_recommendations, install_package, PLUGINS_DIR

    print()
    print("  Detecting hardware …")
    recs = get_recommendations()

    if not recs:
        print("  All recommended runtime packages are already installed.")
        print()
        return 0

    print(f"  Install target: {PLUGINS_DIR}")
    print()
    for rec in recs:
        if rec.is_installed():
            print(f"  [{rec.category.upper()}] {rec.label} — already installed, skipping")
            continue
        print(f"  [{rec.category.upper()}] Installing {rec.pip_name}")
        print(f"          {rec.reason}")
        ok, msg = install_package(rec.pip_name, lambda line: print(f"    {line}"))
        print(f"  {'OK' if ok else 'FAILED'}: {msg}")
        print()

    return 0


def _run_cli(args: argparse.Namespace) -> int:
    import json
    from benchmarks import run_all_benchmarks, compute_overall_score

    # ---- Optional: detect and install runtimes ----
    if getattr(args, 'install_runtimes', False):
        return _cli_install_runtimes()

    tests = args.tests or ["cpu", "ram", "gpu", "disk", "npu"]
    labels = {"cpu": "CPU", "gpu": "GPU", "npu": "NPU", "ram": "RAM", "disk": "Disk"}

    if not args.json:
        print()
        print("  ╔══════════════════════════════════════════╗")
        print("  ║       SynBench – System Benchmark        ║")
        print("  ╚══════════════════════════════════════════╝")
        print()
        print(f"  Running: {', '.join(labels[t] for t in tests)}")
        print()

    current_test = [None]

    def callback(test_name: str, msg: str):
        current_test[0] = test_name
        if not args.json:
            print(f"  [{labels[test_name]}] {msg}")

    bench_kwargs = {}
    if getattr(args, "disk_dir", None):
        bench_kwargs["disk"] = {"test_dir": args.disk_dir}
    results = run_all_benchmarks(tests=tests, callback=callback, bench_kwargs=bench_kwargs or None)
    overall = compute_overall_score(results)

    if args.json:
        data = {
            name: {
                "score":     r.score,
                "available": r.available,
                "summary":   r.summary,
                "metrics":   r.metrics,
                "error":     r.error,
            }
            for name, r in results.items()
        }
        data["overall_score"] = overall
        payload = json.dumps(data, indent=2, default=str)

        if args.output:
            with open(args.output, "w") as f:
                f.write(payload)
            print(f"Results written to {args.output}")
        else:
            print(payload)
        return 0

    # Human-readable summary
    col_w = 10
    print()
    print("  " + "─" * 60)
    print(f"  {'Component':<10} {'Score':>8}   {'Details'}")
    print("  " + "─" * 60)
    for test in tests:
        r = results.get(test)
        if r is None:
            continue
        if not r.available:
            score_str = "N/A"
            detail    = r.error or "Not available"
        else:
            score_str = f"{r.score} pts"
            detail    = r.summary
        print(f"  {labels[test]:<10} {score_str:>8}   {detail}")

    print("  " + "─" * 60)
    print(f"  {'OVERALL':<10} {overall:>6} pts")
    print("  " + "─" * 60)
    print()

    if args.output:
        data = {
            name: {
                "score":     r.score,
                "available": r.available,
                "summary":   r.summary,
                "metrics":   r.metrics,
            }
            for name, r in results.items()
        }
        data["overall_score"] = overall
        with open(args.output, "w") as f:
            import json as _json
            _json.dump(data, f, indent=2, default=str)
        print(f"  Results saved to {args.output}")
        print()

    return 0


# ---------------------------------------------------------------------------
# GUI runner
# ---------------------------------------------------------------------------

def _run_gui() -> None:
    try:
        from PySide6.QtWidgets import QApplication
    except ImportError:
        print("Error: PySide6 is not installed.")
        print("Install it with:  pip install PySide6")
        print("Or use --cli mode.")
        sys.exit(1)

    from gui.app import GLOBAL_STYLE, BenchmarkApp
    app = QApplication(sys.argv)
    app.setStyleSheet(GLOBAL_STYLE)
    window = BenchmarkApp()
    window.show()
    sys.exit(app.exec())


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    args = _parse_args()

    if args.cli:
        sys.exit(_run_cli(args))
    else:
        _run_gui()


if __name__ == "__main__":
    import multiprocessing
    multiprocessing.freeze_support()
    # Force fork on Linux/macOS so child processes don't try to re-execute
    # the frozen PyInstaller binary as a Python interpreter (which hangs).
    if sys.platform != "win32":
        multiprocessing.set_start_method("fork", force=True)
    main()

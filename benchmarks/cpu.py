"""
CPU benchmark: integer throughput, floating-point throughput, multi-core scaling.

Scoring reference: Ryzen 5 5600 (6C/12T, 3.7 GHz base) ≈ 1 000 CPU points.
"""

import math
import multiprocessing
import time
from typing import Callable, Optional

from . import BenchmarkResult

# ---------------------------------------------------------------------------
# Calibration — Ryzen 5 5600 (6C/12T, Zen 3, ~3.7 GHz base) = 1 000 pts.
# No hard cap so high-core-count / fast machines score above 1 000.
# ---------------------------------------------------------------------------
_INT_REFERENCE_OPS   = 10_000_000  # integer ops/sec, single core
_FLOAT_REFERENCE_OPS =  4_000_000  # float ops/sec, single core
_MC_REFERENCE_OPS    = 48_000_000  # float ops/sec, all cores (12-thread ref)

_INT_SCORE_WEIGHT    = 250         # reference machine contribution
_FLOAT_SCORE_WEIGHT  = 250
_MC_SCORE_WEIGHT     = 500

_TEST_DURATION = 5.0               # seconds per sub-test


# ---------------------------------------------------------------------------
# Module-level worker functions (must be picklable for multiprocessing)
# ---------------------------------------------------------------------------

def _int_worker(duration: float, queue: multiprocessing.Queue) -> None:
    """Fibonacci-style integer ops – measures Python integer arithmetic."""
    count = 0
    MOD = 10 ** 9 + 7
    a, b = 0, 1
    start = time.perf_counter()
    while True:
        # Tight inner loop to amortise perf_counter overhead
        for _ in range(10_000):
            a, b = b, (a + b) % MOD
            count += 1
        if time.perf_counter() - start >= duration:
            break
    elapsed = time.perf_counter() - start
    queue.put(count / elapsed)


def _float_worker(duration: float, queue: multiprocessing.Queue) -> None:
    """Trig/exp inner loop – measures Python FP arithmetic."""
    count = 0
    x = 1.0
    start = time.perf_counter()
    while True:
        for _ in range(10_000):
            x = math.sin(x + 0.001) + math.cos(x + 0.002) * math.exp(-0.001 * abs(x))
            count += 1
        if time.perf_counter() - start >= duration:
            break
    elapsed = time.perf_counter() - start
    queue.put(count / elapsed)


def _mc_worker(args):
    """Used by multiprocessing.Pool – returns float ops/sec for one worker."""
    duration = args
    count = 0
    x = 1.0
    start = time.perf_counter()
    while True:
        for _ in range(10_000):
            x = math.sin(x + 0.001) + math.cos(x + 0.002) * math.exp(-0.001 * abs(x))
            count += 1
        if time.perf_counter() - start >= duration:
            break
    elapsed = time.perf_counter() - start
    return count / elapsed


# ---------------------------------------------------------------------------
# Benchmark class
# ---------------------------------------------------------------------------

class CPUBenchmark:
    def __init__(self, callback: Optional[Callable[[str], None]] = None):
        self.cb = callback or (lambda msg: None)

    def _run_in_process(self, target, duration: float) -> float:
        """Spawn a single worker process and return its ops/sec."""
        q: multiprocessing.Queue = multiprocessing.Queue()
        p = multiprocessing.Process(target=target, args=(duration, q))
        p.start()
        p.join(timeout=duration + 10)
        if p.exitcode != 0 or q.empty():
            return 0.0
        return q.get()

    def _run_integer(self) -> float:
        self.cb("Integer throughput test …")
        return self._run_in_process(_int_worker, _TEST_DURATION)

    def _run_float(self) -> float:
        self.cb("Floating-point throughput test …")
        return self._run_in_process(_float_worker, _TEST_DURATION)

    def _run_multicore(self) -> float:
        n_cores = multiprocessing.cpu_count()
        self.cb(f"Multi-core test ({n_cores} cores) …")
        with multiprocessing.Pool(processes=n_cores) as pool:
            rates = pool.map(_mc_worker, [_TEST_DURATION] * n_cores)
        return sum(rates)

    def run(self) -> BenchmarkResult:
        self.cb("Starting CPU benchmark …")

        int_rate   = self._run_integer()
        float_rate = self._run_float()
        mc_rate    = self._run_multicore()

        # No hard cap — high-core-count / faster CPUs legitimately exceed 1 000 pts
        int_score   = int(int_rate   / _INT_REFERENCE_OPS   * _INT_SCORE_WEIGHT)
        float_score = int(float_rate / _FLOAT_REFERENCE_OPS * _FLOAT_SCORE_WEIGHT)
        mc_score    = int(mc_rate    / _MC_REFERENCE_OPS    * _MC_SCORE_WEIGHT)
        total       = int_score + float_score + mc_score

        n_cores = multiprocessing.cpu_count()
        summary = (
            f"Int {int_rate/1e6:.2f} Mops/s  "
            f"FP {float_rate/1e6:.2f} Mops/s  "
            f"MC {mc_rate/1e6:.1f} Mops/s ({n_cores}T)"
        )
        self.cb(f"CPU done — score {total}")

        return BenchmarkResult(
            name="cpu",
            score=total,
            metrics={
                "int_ops_per_sec":   int_rate,
                "float_ops_per_sec": float_rate,
                "mc_ops_per_sec":    mc_rate,
                "cores":             n_cores,
                "int_score":         int_score,
                "float_score":       float_score,
                "mc_score":          mc_score,
            },
            summary=summary,
        )

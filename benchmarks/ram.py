"""
RAM benchmark: sequential read/write bandwidth and random-access latency.

Scoring reference: DDR4-3200 dual-channel ≈ 200 RAM points.
"""

import time
from typing import Callable, Optional

import numpy as np

from . import BenchmarkResult

# ---------------------------------------------------------------------------
# Calibration – tuned to DDR4-3200 DC reference.
# ---------------------------------------------------------------------------
_READ_BW_REF  = 35.0   # GB/s
_WRITE_BW_REF = 30.0   # GB/s
_LATENCY_REF  = 80.0   # nanoseconds (lower = faster)

_READ_SCORE_WEIGHT  = 80
_WRITE_SCORE_WEIGHT = 60
_LATENCY_SCORE_WEIGHT = 60

_ARRAY_BYTES = 512 * 1024 * 1024  # 512 MB working set
_CHUNK       = 64 * 1024 * 1024   # 64 MB chunks for bandwidth passes
_LATENCY_STEPS = 2_000_000        # random-access iterations


class RAMBenchmark:
    def __init__(self, callback: Optional[Callable[[str], None]] = None):
        self.cb = callback or (lambda msg: None)

    # ------------------------------------------------------------------
    def _read_bandwidth(self) -> float:
        """Sequential read: iterate over a large array and sum it."""
        self.cb("Allocating test array …")
        n = _ARRAY_BYTES // 8  # float64
        arr = np.ones(n, dtype=np.float64)

        self.cb("Sequential read bandwidth test …")
        total_bytes = 0
        elapsed = 0.0
        passes = 0

        # Warm up
        _ = arr.sum()

        deadline = time.perf_counter() + 5.0
        while time.perf_counter() < deadline:
            _ = arr.sum()
            total_bytes += arr.nbytes
            passes += 1

        elapsed = 5.0 - max(0.0, deadline - time.perf_counter())
        elapsed = 5.0  # approximate; fine for bandwidth

        # Recalculate with actual timing
        start = time.perf_counter()
        for _ in range(max(1, passes)):
            _ = arr.sum()
        elapsed = time.perf_counter() - start
        actual_bytes = arr.nbytes * passes

        # Redo cleanly
        passes = 3
        start = time.perf_counter()
        for _ in range(passes):
            _ = arr.sum()
        elapsed = time.perf_counter() - start
        gb = (arr.nbytes * passes) / 1e9
        return gb / elapsed  # GB/s

    def _write_bandwidth(self) -> float:
        """Sequential write: fill a large array repeatedly."""
        self.cb("Sequential write bandwidth test …")
        n = _ARRAY_BYTES // 4  # float32 to stress write paths
        arr = np.empty(n, dtype=np.float32)

        # Warm up
        arr[:] = 1.0

        passes = 3
        start = time.perf_counter()
        for _ in range(passes):
            arr[:] = 1.0
        elapsed = time.perf_counter() - start
        gb = (arr.nbytes * passes) / 1e9
        return gb / elapsed  # GB/s

    def _latency(self) -> float:
        """Random-access latency: pointer-chase through a shuffled index array."""
        self.cb("Random-access latency test …")
        n = 16 * 1024 * 1024 // 8  # 16 MB – fits in LLC of most CPUs
        # Larger array forces DRAM access
        n = 256 * 1024 * 1024 // 8

        indices = np.arange(n, dtype=np.int64)
        np.random.shuffle(indices)

        steps = min(_LATENCY_STEPS, n)
        start = time.perf_counter()
        pos = 0
        for _ in range(steps):
            pos = indices[pos % n]
        elapsed = time.perf_counter() - start

        ns_per_access = (elapsed / steps) * 1e9
        return ns_per_access

    def run(self) -> BenchmarkResult:
        self.cb("Starting RAM benchmark …")

        read_bw  = self._read_bandwidth()
        write_bw = self._write_bandwidth()
        latency  = self._latency()

        read_score    = min(_READ_SCORE_WEIGHT,    int(read_bw  / _READ_BW_REF  * _READ_SCORE_WEIGHT))
        write_score   = min(_WRITE_SCORE_WEIGHT,   int(write_bw / _WRITE_BW_REF * _WRITE_SCORE_WEIGHT))
        # Latency: lower is better → invert
        latency_score = min(_LATENCY_SCORE_WEIGHT, int(_LATENCY_REF / max(latency, 1.0) * _LATENCY_SCORE_WEIGHT))
        total = read_score + write_score + latency_score

        summary = (
            f"Read {read_bw:.1f} GB/s  "
            f"Write {write_bw:.1f} GB/s  "
            f"Latency {latency:.0f} ns"
        )
        self.cb(f"RAM done — score {total}")

        return BenchmarkResult(
            name="ram",
            score=total,
            metrics={
                "read_bandwidth_gbps":  read_bw,
                "write_bandwidth_gbps": write_bw,
                "latency_ns":           latency,
                "read_score":           read_score,
                "write_score":          write_score,
                "latency_score":        latency_score,
            },
            summary=summary,
        )

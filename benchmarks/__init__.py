from dataclasses import dataclass, field
from typing import Dict, Any, Optional, Callable, List


@dataclass
class BenchmarkResult:
    name: str
    score: int
    metrics: Dict[str, Any] = field(default_factory=dict)
    summary: str = ""
    available: bool = True
    error: Optional[str] = None


def run_all_benchmarks(
    tests: Optional[List[str]] = None,
    callback: Optional[Callable[[str, str], None]] = None,
    bench_kwargs: Optional[Dict[str, Any]] = None,
) -> Dict[str, BenchmarkResult]:
    """Run selected benchmarks and return results keyed by test name.

    callback(test_name, message) is called with progress updates.
    """
    from .cpu import CPUBenchmark
    from .gpu import GPUBenchmark
    from .npu import NPUBenchmark
    from .ram import RAMBenchmark
    from .disk import DiskBenchmark

    registry = {
        "cpu": CPUBenchmark,
        "ram": RAMBenchmark,
        "gpu": GPUBenchmark,
        "disk": DiskBenchmark,
        "npu": NPUBenchmark,
    }

    if tests is None:
        tests = list(registry.keys())

    cb = callback or (lambda name, msg: None)
    extra = bench_kwargs or {}
    results: Dict[str, BenchmarkResult] = {}

    for test in tests:
        if test not in registry:
            continue
        try:
            kw = extra.get(test, {})
            bench = registry[test](callback=lambda msg, t=test: cb(t, msg), **kw)
            results[test] = bench.run()
        except Exception as exc:  # noqa: BLE001
            results[test] = BenchmarkResult(
                name=test,
                score=0,
                available=False,
                error=str(exc),
                summary="Error",
            )

    return results


def compute_overall_score(results: Dict[str, BenchmarkResult]) -> int:
    return sum(r.score for r in results.values() if r and r.available)

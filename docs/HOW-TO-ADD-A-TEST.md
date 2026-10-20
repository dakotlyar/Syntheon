# How to add a test (the plugin contract)

A SynBench "test" is a class implementing `sb::ITest` (`src/core/itest.h`).
Tests are linked in statically and listed in `src/tests/CMakeLists.txt`
(`SYNBENCH_TESTS`); the core, the UI and the database know nothing about any
particular test. The real ones so far are the CPU test (Phase 2,
`src/tests/cpu/`) and the RAM test (Phase 3, `src/tests/ram/`); the other five
are still Phase 1 stubs built from `StubTest` (`src/tests/common/`). A real
test replaces its folder's contents.

Rules that never change: **IDs are forever** (test id, sub-test id, option key,
metric key). They become JSON keys and database values. Never rename; deprecate.

## 1. Identity: `TestInfo`

```cpp
TestInfo info;
info.id = "ram";            // stable, lower-case
info.name = "RAM";
info.summary = "Bandwidth, latency, channels";   // the picker row
info.colorToken = "t-ram";  // a theme token, not a colour
info.testVersion = 1;       // bump when the MEASUREMENT changes
info.formulaVersion = 1;    // bump when the SCORE FORMULA changes
info.exclusivity = Exclusivity::Shared;          // RunsAlone for Battery
info.recovery = RecoveryMode::None;              // FromCheckpoints for Battery
info.subTests = {{"bandwidth", "Bandwidth", true /*selectable*/, true /*on*/}};
```

Compare marks a row "scoring changed" when the versions of two runs differ, so
bumping them is how the UI learns that old scores aren't comparable.

## 2. Discovery: `probe()` and `options()`

`probe()` runs on a pool thread at startup (finish in about 2 s) and returns an
`Availability`: available, partial (some sub-tests unavailable, with reasons) or
unavailable (with a reason; the picker shows N/A). It may add `capabilities`
("Vulkan 1.4") that the first-run dialog and System info display, and a `hint`
(a pointer to something the user can install).

`options()` returns an `OptionSchema` (`src/core/options.h`): Info, Bool, Number,
SingleSelect, MultiSelect or DynamicSelect entries. The option tab is generated
from it; you write no QML. User-selectable sub-tests get an automatic "Sub-tests"
checklist. `preconditions()` (optional) are live checks shown while the tab is
visible (Battery: on AC? charged?); `validate()` is a pure function.

## 3. Running: `run()`

```cpp
TestResult run(RunContext &ctx, const OptionValues &options) override
{
    ctx.log(LogLevel::Info, "Measuring bandwidth");
    ctx.progress(0.25, "Bandwidth");
    if (ctx.cancelled())                     // poll at least every 250 ms
        return TestResult::withStatus(Status::Cancelled);
    TestResult r;
    r.status = Status::Ok;
    r.metrics = {{"read_bw", {}, "Read bandwidth", 74.8, "GB/s", Direction::HigherIsBetter, true}};
    return r;
}
```

- It runs on a worker thread, blocking. A test that ignores a Stop for the grace
  period is abandoned and the run is saved as Interrupted (the app must be restarted).
- `ctx.progress()`, `log()` and `cancelled()` are safe from your helper threads;
  `checkpoint()` and `scratchDir()` write to the database and must be called on
  the thread that called `run()`.
- Metrics carry a stable `key`, a label, a value (number or text), a unit and a
  `Direction` (the Compare screen colours changes by it). Mark headline metrics.
- `estimate()` feeds "about 14 min"; `timeLimit()` is the hard cap (0 = none).
- Root-only work goes through `ctx.privileged()` (`PrivilegedOps`), which fails with
  a `PrivError` instead of hanging. A new privileged op is a deliberate change in
  `privileged_ops.h`, the protocol and the Linux helper.

## 4. Scoring and the Detail page

`score(const TestResult &)` is a **pure function over stored data**: it returns a
`ScoreBreakdown`, which the orchestrator computes once when the run ends and
stores frozen with the result; it is never recomputed for a stored run (a
formula change bumps `formulaVersion` instead, and Compare marks the runs
"scoring changed"). `weightedScore()` and `weightedGeometricScore()`
(`core/scoring.h`) are the two formula shapes so far. In the geometric one a
metric where smaller is better (a latency in ns) sets
`GeometricMetric::lowerIsBetter`, so it is normalised as anchor ÷ result
instead of result ÷ anchor (the RAM test's latencies); it is off by default.
Anything `score()` needs to choose between formulas must be in the stored
result too (the RAM test reads `cache.levels` to know whether the core has an
L3 to weight).

`detail()` returns a `DetailLayout` of declarative blocks (KPIs, table,
CrystalDiskMark-style grid, line chart, note, N/A state, or `Custom` for a view
only the app can draw). The default is `genericDetailLayout()`. The renderer
lays a line chart and the tables right after it out side by side (3:2), pairs
two consecutive tables unless one is `fullWidth`, and gives everything else a
row of its own, so a note between blocks ends a row. A `LineChartBlock` draws
one series:

- `xAxis`: linear or log2, with labelled ticks;
- `yAxis` (Phase 3): `Linear` by default (five grid lines from 0, every chart
  before Phase 3), or `Log2` / `Log10` with a grid line per tick (given, or
  chosen over the data); on a log axis a point at or below 0 isn't drawn;
- `annotations`: labels placed above the curve at an x;
- `markers` (Phase 3): open dots on the curve's own points at these x values
  (the RAM test's measured cache levels).

## 5. The measurement engine (only for a benchmark that times kernels)

The CPU and RAM tests share one engine, the `synbench_test_engine` CMake target
(its files stay in `src/tests/cpu/engine/`, `cpu_context.*` and `telemetry/`,
namespace `sb::cpu`; Phase 3 D17). It runs a `Kernel` in time-boxed windows on a
`WorkerPool` (a warm-up and five repetitions, the median and the spread), polls
Stop, and checks each unit's result:

- `WorkerPool::Options::threads`, `pinTo`, `pickFrom`: the CPU test's pools (all
  threads unpinned; one thread on the top-ranked core, re-picked per
  repetition);
- `WorkerPool::Options::pinEach` (Phase 3 P11): worker *i* pinned to entry *i*
  for the pool's life (the RAM test's all-thread pools, one worker per logical
  CPU from `isa::allowedLogicalCpus()`); empty by default, so other pools are
  unchanged;
- `cpu::scaled()`: the windows at the developer time scale
  (`SYNBENCH_STUB_SCALE`, smoke runs), so a test's whole run fits in a CI job;
- `opaque()`: hides a value from the optimiser (seeds and buffer pointers on
  the way into a unit).

A test links `synbench_test_engine` and keeps its kernels in a target of their
own with fixed optimisation flags (`synbench_cpu_kernels`,
`synbench_ram_kernels`), so a Debug build measures the same code.

`tools/cpu-repeatability.py <synbench.db> --test cpu|ram` prints the CV of a
test's score, its groups and its main metrics over the last runs in a history
(the validation gate's ≤ 3% over 5 runs); a new real test adds its groups and
metrics to the script's `TESTS` table.

## 6. Work in a separate process (only if needed)

A test whose work shouldn't run inside SynBench (a library with global state
you can't reset, code that could crash, or a configuration that must not leak
in) can run it in a small worker program, as the CPU test does for encryption
(`src/tests/cpu/worker/` and `crypto_client.h`):

- The worker is its own executable, installed next to the app (Windows) or in
  `libexec/` (Linux), listed in the packaging checks, and found by absolute
  path from the app's directory; never through `PATH`.
- Its environment is built from scratch (`QProcessEnvironment`), not
  inherited, and it runs in its own folder; on Windows it loads its DLLs from
  System32 and its own folder only (`/DEPENDENTLOADFLAG:0xA00`).
- It takes only fixed options and answers with **JSON lines** on stdout (one
  object per line, a known shape, a length cap per line); the client checks
  the shape and the values (checksums, plausibility) itself, never trusting
  the worker's verdict alone.
- The client polls `cancelled` every 50 ms and **kills** the process on Stop
  or at a time limit (killing a process is safe, unlike a thread), and holds
  its stdin open so the worker exits by itself if SynBench goes away.
- A `--self-test` mode lets `probe()` mark the part unavailable when the
  worker is missing or broken.

## 7. Crash recovery (only if needed)

A test that runs for hours (Battery) sets `recovery = FromCheckpoints`, calls
`ctx.checkpoint(QCborMap{...})` as it goes, and implements `recover()`, which is
given the checkpoints after a crash or power loss and returns the result it can
rebuild. Other tests need nothing: a cut-short run is marked Interrupted.

## 8. Registering it

1. Create `src/tests/<id>/` with a `CMakeLists.txt` building `synbench_test_<id>`
   and `<id>_test.h` declaring `sb::tests::<Id>Test`.
2. Add `<id>` to `SYNBENCH_TESTS` in `src/tests/CMakeLists.txt`. The registration
   code is generated from that list (no self-registering static initialisers).
3. Add unit tests (see `tests/unit/tst_stubs.cpp`, `tst_registry.cpp` and, for a
   real test, `tst_cpu_test.cpp` and `tst_ram_test.cpp`) and run `ctest`. GUI
   tests that run or configure a test go through the stubs, not a real test
   (a real run takes minutes and, for RAM, gigabytes). `SYNBENCH_SMOKE=1` runs every
   available test end to end, real ones at the developer time scale.
4. A third-party library: see "the third-party pattern" in BUILDING.md (pinned,
   static, and a notice in `THIRD-PARTY-NOTICES.txt`).

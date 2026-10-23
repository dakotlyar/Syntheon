# How to add a test (the plugin contract)

A SynBench "test" is a class implementing `sb::ITest` (`src/core/itest.h`).
Tests are linked in statically and listed in `src/tests/CMakeLists.txt`
(`SYNBENCH_TESTS`); the core, the UI and the database know nothing about any
particular test. The real ones so far are the CPU test (Phase 2,
`src/tests/cpu/`), the RAM test (Phase 3, `src/tests/ram/`) and the GPU test
(Phase 4a, `src/tests/gpu/`); the other four are still Phase 1 stubs built from
`StubTest` (`src/tests/common/`). A real test replaces its folder's contents.

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
- `ctx.progress(fraction, stage)` drives the test's row on the Progress screen
  (the stage line, e.g. "Write, plain stores · rep 2 of 5"). `ctx.setPart(part)`
  (optional) names the part of the test now running ("All-thread bandwidth",
  the CPU test's group names): the live log's pill shows "RAM · All-thread
  bandwidth" from the next `progress()` on. A test that never sets a part gets
  its stage in the pill.
- `ctx.holdInterface(true)` (Phase 4a, optional) is for a test that runs
  something the main window's own drawing would disturb: the GPU test renders
  in a window of its own, and the main window's animations and ticker would
  steal frames from it. While held, the interface does not animate, tick or
  repaint the live progress and log; the queue row reads "Running in its own
  window" and a card shows the test's name and the current part from
  `setPart()` (updated once per part, never ticking). Nothing is lost:
  progress, log lines and sub-test changes still arrive and are applied, in
  order, when the hold ends. Call `holdInterface(false)` yourself when
  the disturbing part is over; the orchestrator also releases it when `run()`
  returns, whatever the status, so a test that returns early or throws needs
  no cleanup. Call it from `run()` or its helper threads; a test that never
  calls it is unchanged. Keep the held span short and name the step in `setPart()`, because
  that is the only live feedback while it lasts (the stage strings are buffered).
- `ctx.progress()`, `setPart()`, `holdInterface()`, `log()` and `cancelled()` are safe from your helper threads;
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

`tools/cpu-repeatability.py <synbench.db> --test cpu|ram|gpu` prints the CV of a
test's score, its groups and its main metrics over the last runs in a history
(the validation gate's ≤ 3% over 5 runs); a new real test adds its groups and
metrics to the script's `TESTS` table.

`--anchors` (RAM and GPU) is the calibration step. It gates the runs (status
ok, one machine, and for RAM the huge pages), prints the median of every
scored metric rounded to 3 significant figures as `formula_v1.cpp`'s
constants (in that file's order, so they paste straight in), and re-scores each
run with them: the median run scores about 1,000, and the re-scored CV equals
the stored score's. The script holds its own copy of the formula (the groups,
their weights, and for the GPU the table of the file's current constants), so
it **must match the C++ formula exactly** (the RAM rule, Phase 3 8c95648): the
GPU run of `--anchors` also re-scores the runs with the constants `formula_v1.cpp` has now
and prints "equal" or "differs" against the stored scores, and
`tests/tools/test_cpu_repeatability.py` compares the script's weights, constants
and re-scores with the C++ file and with a history of five GPU runs that
`tst_gpu_test` writes through the app's own storage (`repeatabilityFixture`,
run by the `gpu_repeatability_fixture` CTest test, which `cpu_repeatability_tool`
depends on). Change `formula_v1.cpp`'s weights or constants and the script's
tables in the same commit, or that test fails. A run stored under older anchors
differs from its re-score by design: that is the anchor history, not a bug.

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

### The GPU worker (Phase 4a): a second example

`synbench-gpu` (`src/tests/gpu/worker/`, client in `src/tests/gpu/client/`)
follows the same rules and adds what a graphics test needs. The reason for the
process is that a driver crash, a GPU hang or a stuck present kills only the
worker; the GPU test reports an Error for that API and goes on.

- **The environment is built from scratch** (`GpuClient::baseEnvironment()`):
  Linux gets the display variables (`DISPLAY`, `WAYLAND_DISPLAY`,
  `XDG_RUNTIME_DIR`, `XAUTHORITY`, the session and desktop, the D-Bus address),
  `HOME`, `LANG` and `VK_LOADER_LAYERS_DISABLE=~implicit~` (so overlays and
  recorders stay out); Windows gets `SystemRoot`, the profile folders, and a
  `PATH` of `System32` and the worker's folder. `QT_PLUGIN_PATH`,
  `LD_LIBRARY_PATH` and driver overrides never reach it. A per-API addition
  goes in `extraEnv`: OpenGL gets the PRIME offload variables that put it on
  the chosen GPU, and nothing else gets any.
- **One process per API, not per test.** The GPU test runs scene by scene
  (every API on Geometry, then every API on Draw calls, and so on) so that each
  API sees the same thermal conditions. It therefore starts one long-lived
  `--serve` worker per API (`GpuSession`), waits for every `ready` (each
  worker creates its device and pipelines before it says so, with no window),
  and then sends one `run <workload>:<warmupMs>:<measureMs>` command at a time,
  each answered by a `result` (or an `error`) and then `idle`. `quit` ends a
  worker politely. Workloads that need no scene-by-scene order (the native 2D
  paths, tests) use `GpuClient::run()`: one worker, one plan.
- **Stop and kill.** The client reads with a poll of 50 ms. On Stop it writes
  `stop` (the worker finishes its frame and exits 4) and, for several workers
  at once, simply kills them, so Stop stays within 250 ms. A worker that goes
  quiet is killed when a workload's timeout (2 × warm-up + measure, plus 30 s)
  runs out. The worker holds stdin as a lifeline: EOF means SynBench is gone
  and it exits 3 at once.
- **Exit codes mean something:** 0 done, 1 error, 2 bad arguments or a
  protocol error, 3 stdin closed, 4 stopped, 5 the API is not available. Exit 5
  is **N/A, not an Error**: the reason comes from the worker's `error` event
  and the API is left out of the score's comparison; anything else, a crash, a
  protocol violation or a missing result is Failed. A worker that delivered
  every result and then exited badly keeps them, with a note.
- **The client verifies, the worker doesn't judge.** Frame counts, plausible
  rates and the image signature of every workload are checked by the GPU test,
  which compares the APIs' pictures with each other (a mismatch is an Error for
  the odd one out), because a wrong result is the worst kind of bug here.
- **No dialogs, ever.** The worker is started without a console from a GUI app,
  so on Windows it sets `SetErrorMode` and a stderr message handler first
  thing, and a fatal Qt message exits instead of showing a box. Its start-up
  `--probe` runs while SynBench starts, so a hang there would block the
  window; the probe has its own short time limit.
- **Packaging.** The worker is listed in the packaging checks like the
  encryption one, and because it runs headless for its probe it needs Qt's
  `offscreen` platform plugin next to the window one: the Windows install test
  runs the installed `--probe` without a console to catch exactly that.

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

# SynBench

A synthetic system benchmark for CPU, GPU, NPU, RAM, and Disk.  Produces a composite score calibrated against a Ryzen 5 5600 reference system, similar in methodology to Novabench.

Available as a self-contained GUI application (AppImage on Linux, an installer with an uninstaller on Windows) and as a headless CLI tool for scripting and automation.

---

## Benchmarks

| Test     | What it measures                                                               | Reference score          |
|----------|--------------------------------------------------------------------------------|--------------------------|
| **CPU**  | Integer throughput, FP throughput, multi-core scaling (Python multiprocessing) | Ryzen 5 5600 ≈ 1 000 pts |
| **GPU**  | FP32 GEMM compute + 2D textured fill-rate + 3D shaded/lit triangle throughput   | RTX 3060 ≈ 300 pts       |
| **NPU**  | AI inference throughput (ONNX Runtime, best available execution provider)      | Dedicated NPU ≈ 200 pts  |
| **RAM**  | Sequential and random memory bandwidth                                         | High-end DDR5 ≈ 200 pts  |
| **Disk** | Sequential + random 4 K read/write (256 MB test file, fsync'd)                 | PCIe 4 NVMe ≈ 200 pts    |

### GPU: compute + rendering

The GPU score blends three independent measurements, weighted 60/20/20 of the 300-point reference:

1. **Compute (60%)** — FP32 matrix multiply (GEMM), GFLOPS
2. **2D rendering (20%)** — textured full-viewport quad fill-rate, Mpixels/sec
3. **3D rendering (20%)** — a shaded, textured, lit wavy-grid mesh, triangles/sec

Compute and rendering use entirely separate subsystems and fail independently — a machine can score on one even if the other is unavailable (e.g. no CUDA/OpenCL runtime, or no display for OpenGL), and only shows "No GPU or OpenCL/OpenGL drivers found" if *both* fail.

**Compute backend priority** — SynBench tries these in order, using the first one that succeeds:

1. **CuPy** (CUDA or ROCm) — best scores, requires optional package
2. **CUDA via cuBLAS ctypes** — near-CuPy scores, requires CUDA toolkit (`libcublas`), no Python package needed
3. **PyOpenCL** — requires optional package
4. **OpenCL via ctypes** — works on any GPU with OpenCL drivers, no packages needed
5. **N/A** — reported when no GPU or drivers are detected

**2D/3D rendering** uses an offscreen Qt/OpenGL context (`QOffscreenSurface` + `QOpenGLContext`). PySide6 is already a hard dependency for the GUI, so this needs no extra packages, and it runs identically from `--cli` or the GUI, on a real GPU or a software rasterizer.

The log panel shows exactly which backend was tried and why each fallback occurred.

### NPU backend

Uses ONNX Runtime and automatically selects the best available execution provider in priority order: QNN (Qualcomm) → OpenVINO (Intel) → TensorRT → CUDA → ROCm → DirectML → CoreML → CPU.  Reports N/A when ONNX Runtime is not installed.

---

## Requirements

### Runtime (pre-built binary)

The AppImage and Windows `.exe` are fully self-contained — **no Python or other software required** on the target machine.

Optional packages improve GPU and NPU benchmark scores.  Use **Tools → Install Optional Runtimes** in the GUI (or accept the setup dialog on first launch) to install them with one click — the release package bundles its own sidecar Python (`python-embed/` next to the executable) purely so this installer works even on a freshly-imaged machine with no Python of its own. You can also install them manually with an existing `pip`; either way they're picked up from the `synbench_plugins/` folder next to the executable (or `~/.synbench/plugins/` if that's not writable) on the next run:

| Hardware               | Package       | Command                     |
|------------------------|---------------|-----------------------------|
| NVIDIA GPU (CUDA 12.x) | cupy-cuda12x  | `pip install cupy-cuda12x`  |
| AMD GPU (ROCm)         | cupy-rocm-5-0 | `pip install cupy-rocm-5-0` |
| Any GPU (OpenCL)       | pyopencl      | `pip install pyopencl`      |
| NPU / AI accelerator   | onnxruntime   | `pip install onnxruntime`   |

> **Note:** The built-in OpenCL ctypes backend requires only GPU drivers and works without any pip install.  The cuBLAS backend requires the CUDA toolkit (`libcublas.so.12` on Linux, `cublas64_12.dll` on Windows) but no Python packages.

> **AMD GPU with Mesa (Linux):** The OpenCL ctypes backend requires the ICD loader and Mesa's OpenCL implementation.  Install them first:
>
> ```bash
> # Arch / CachyOS
> sudo pacman -S ocl-icd opencl-rusticl-mesa
> # Ubuntu / Debian
> sudo apt install ocl-icd-libopencl1 mesa-opencl-icd
> # Fedora Atomic / Bazzite / SteamOS (reboot after)
> rpm-ostree install mesa-libOpenCL
> ```
>
> `opencl-rusticl-mesa` (Rusticl) is preferred over the older `opencl-mesa` (Clover) — it has better OpenCL 3.0 compliance and fewer driver bugs. Rusticl also registers *zero* OpenCL platforms unless the `RUSTICL_ENABLE` environment variable names your GPU driver — SynBench sets this itself (`radeonsi:iris`) before probing, so no manual `export` is needed once the package above is installed.

### Build requirements

| Requirement         | Linux                                        | Windows                                      |
|---------------------|----------------------------------------------|----------------------------------------------|
| Internet connection | For downloading Python + appimagetool        | For downloading Python installer             |
| curl                | Yes                                          | No (PowerShell used)                         |
| [Inno Setup](https://jrsoftware.org/isdl.php) | N/A                        | Optional — only needed to build `SynBench-Setup.exe`; the portable `dist\SynBench\` folder still builds without it |
| Everything else     | Downloaded automatically by the build script | Downloaded automatically by the build script |

---

## Building

### Linux — AppImage

```bash
chmod +x build_linux.sh
./build_linux.sh
```

The script:

1. Downloads a portable **Python 3.10 x86_64_v2** (SSE4.2 baseline, ~2011+) from python-build-standalone — avoids SIGILL on CPUs without AVX-512 such as Raptor Lake
2. Creates an isolated venv and installs PyInstaller, numpy, psutil, PySide6
3. Runs PyInstaller to produce `dist/SynBench/`
4. Downloads `appimagetool` if not already present
5. Packages everything into `SynBench-x86_64.AppImage`

Subsequent builds reuse the cached Python and appimagetool downloads.

**Output:** `SynBench-x86_64.AppImage`

> If you see a FUSE error when running the AppImage:
>
> ```bash
> # Arch / CachyOS
> sudo pacman -S fuse2
> # Ubuntu / Debian
> sudo apt install libfuse2
> # Or bypass FUSE entirely
> APPIMAGE_EXTRACT_AND_RUN=1 ./SynBench-x86_64.AppImage
> ```

### Windows — Installer

```
build_windows.bat
```

The script:

1. Looks for Python in `PATH` or a known install location; downloads and silently installs it if missing (no admin required)
2. Installs PyInstaller, numpy, psutil, PySide6 via pip
3. Runs PyInstaller to produce `dist\SynBench\`
4. Bundles a sidecar Python (`dist\SynBench\python-embed\`) for the in-app GPU/NPU plugin installer
5. If [Inno Setup](https://jrsoftware.org/isdl.php) is installed, compiles `installer_output\SynBench-Setup.exe` from `installer.iss` — a single self-extracting installer that registers an uninstaller in "Apps & Features", with optional Start Menu / Desktop shortcuts

Set `SYNBENCH_VERSION` before running to stamp a specific version (e.g. `set SYNBENCH_VERSION=1.2.3`); defaults to `0.0.0-local`. CI passes the real version from the git tag.

**Output:** `installer_output\SynBench-Setup.exe` (requires admin to run — installs per-machine to Program Files). If Inno Setup isn't installed, this step is skipped and `dist\SynBench\` is still usable as a portable folder — zip it and distribute directly, no installation required on the end-user machine.

### Running from source

Requires Python 3.10+ with PySide6, numpy, and psutil.  On Linux, [uv](https://docs.astral.sh/uv/) is the easiest way to get a compatible environment:

```bash
# GUI
./run.sh

# CLI
./run.sh --cli --tests cpu ram disk
```

Without uv, use any Python that has the dependencies installed:

```bash
pip install PySide6 numpy psutil
python synbench.py          # GUI
python synbench.py --cli    # CLI
```

---

## Running

### GUI

```bash
# AppImage
chmod +x SynBench-x86_64.AppImage
./SynBench-x86_64.AppImage

# Windows
SynBench.exe
```

On first launch, if optional runtime packages are recommended for the detected hardware, a setup dialog appears.  This can be suppressed with "Don't show this again" or re-opened later via **Tools → Install Optional Runtimes**.

### CLI

```bash
# All tests
./SynBench-x86_64.AppImage --cli

# Select specific tests
./SynBench-x86_64.AppImage --cli --tests cpu ram disk

# Machine-readable JSON output
./SynBench-x86_64.AppImage --cli --json

# Write JSON results to a file
./SynBench-x86_64.AppImage --cli --json --output results.json

# Custom disk test directory
./SynBench-x86_64.AppImage --cli --disk-dir /mnt/data

# Detect hardware and install recommended GPU/NPU packages
./SynBench-x86_64.AppImage --cli --install-runtimes
```

#### All CLI flags

| Flag                 | Description                                                           |
|----------------------|-----------------------------------------------------------------------|
| `--cli`              | Headless mode (required for all flags below)                          |
| `--tests TEST…`      | Space-separated subset of `cpu gpu npu ram disk`                      |
| `--json`             | Print results as JSON instead of the formatted table                  |
| `--output FILE`      | Write JSON results to FILE                                            |
| `--disk-dir DIR`     | Directory to use for disk I/O test files (default: auto-detect)       |
| `--install-runtimes` | Detect GPU/NPU hardware, install missing optional packages, then exit |

#### Example JSON output

```json
{
  "cpu":  { "score": 987,  "available": true,  "summary": "Int 8.21 Mops/s  FP 3.94 Mops/s  MC 38.1 Mops/s (12T)" },
  "gpu":  { "score": 289,  "available": true,  "summary": "11340 GFLOPS, 29800 Mpx/s, 640,000,000 tris/s  [CUDA (cuBLAS) – NVIDIA GeForce RTX 3060]" },
  "npu":  { "score": 0,    "available": false, "summary": "onnxruntime not installed" },
  "ram":  { "score": 142,  "available": true,  "summary": "…" },
  "disk": { "score": 178,  "available": true,  "summary": "…" },
  "overall_score": 1591
}
```

---

## Scoring reference

| Score  | CPU                        | GPU                       |
|--------|----------------------------|---------------------------|
| ~50    | —                          | Typical laptop iGPU       |
| ~250   | Intel Core i5-8400         | —                         |
| ~190   | —                          | GTX 1060                  |
| ~215   | —                          | RTX 2060                  |
| ~500   | Ryzen 5 3600               | —                         |
| ~1000  | Ryzen 5 5600 *(reference)* | —                         |
| ~300   | —                          | RTX 3060 *(reference)*    |
| ~460   | —                          | RTX 3080                  |
| ~600   | Ryzen 9 5900X              | —                         |
| ~1500+ | Threadripper / Xeon        | —                         |
| ~765   | —                          | RTX 4090                  |

CPU scores are uncapped and linear — high-core-count CPUs will keep exceeding the reference values proportionally. GPU scores are uncapped but follow a square-root curve against GFLOPS, so a weak iGPU still registers a visible score instead of rounding to near-zero, while flagship discrete GPUs no longer dominate the composite score as heavily. The overall score is the sum of all available sub-scores.

---

## Project structure

```
synbench.py          Entry point (GUI + CLI)
benchmarks/
  cpu.py             CPU benchmark (multiprocessing)
  gpu.py             GPU benchmark (cupy / cuBLAS / OpenCL compute + Qt/OpenGL 2D/3D rendering)
  npu.py             NPU benchmark (ONNX Runtime)
  ram.py             RAM bandwidth benchmark
  disk.py            Disk I/O benchmark
gui/
  app.py             PySide6 GUI application
runtime.py           Optional runtime detection and installer
synbench.spec        PyInstaller build spec
installer.iss        Inno Setup script (Windows installer)
build_linux.sh       Linux AppImage build script
build_windows.bat    Windows build script (portable folder + installer)
run.sh               Development launcher (uses uv)
```

---

## Compatibility

| Platform       | Minimum                                           | Notes                                         |
|----------------|---------------------------------------------------|-----------------------------------------------|
| Linux x86_64   | SSE4.2 (Intel Nehalem ~2008, AMD Bulldozer ~2011) | AppImage; FUSE2 required at runtime           |
| Linux aarch64  | ARMv8-A                                           | AppImage                                      |
| Windows x86_64 | Windows 10 64-bit                                 | Installer requires admin (installs to Program Files) |
| macOS          | macOS 12 Monterey                                 | Build from source; not officially distributed |

The GPU benchmark works on any GPU with OpenCL drivers (NVIDIA, AMD, Intel) without additional packages.  CUDA-accelerated benchmarking requires the CUDA toolkit (`libcublas`).
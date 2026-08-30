"""
GPU compute benchmark.

Backend priority: CuPy (CUDA/ROCm) → PyOpenCL → OpenCL via ctypes.

The ctypes backend loads OpenCL.dll / libOpenCL directly and requires no
extra Python packages — only GPU drivers.  It runs automatically on any
machine with a GPU, making GPU benchmarking work out-of-the-box from the
compiled executable.

Scoring reference: RTX 3060 (12.7 TFLOPS FP32) ≈ 300 GPU points.
Higher-bandwidth backends (cupy) achieve scores closer to theoretical peak;
the ctypes tiled-GEMM backend is somewhat lower for the same GPU but
measures real GPU compute throughput.

Optional packages (install for higher scores):
  NVIDIA GPU (CUDA 12.x): pip install cupy-cuda12x
  AMD GPU (ROCm):         pip install cupy-rocm-5-0
  Any GPU (OpenCL):       pip install pyopencl
"""

import sys
import time
from typing import Callable, Optional, Tuple

import numpy as np

from . import BenchmarkResult

# ---------------------------------------------------------------------------
# Calibration
# ---------------------------------------------------------------------------
# score = _GPU_BASE * (gflops / _GPU_GFLOPS_REF) ** _GPU_CURVE   (no hard cap)
#
# A square-root curve (_GPU_CURVE = 0.5) keeps the score uncapped for
# enthusiast hardware while compressing the huge GFLOPS spread between a
# weak laptop iGPU and a flagship discrete GPU into a much narrower score
# spread — a linear mapping made low-end iGPUs (a few hundred GFLOPS on the
# unoptimized tiled OpenCL kernel) round down to single-digit or zero
# scores, while a 4090 ran off to ~1 950 points and dominated the composite.
#   Laptop iGPU ~300 GFLOPS  → ~46 pts   (was ~7 pts, linear)
#   GTX 1060    ~5 000 GFLOPS → ~188 pts  (was ~118 pts, linear)
#   RTX 3060    ~12 700 GFLOPS → 300 pts  (reference, unchanged)
#   RTX 4090    ~82 600 GFLOPS → ~765 pts (was ~1 953 pts, linear)
_GPU_GFLOPS_REF = 12_700.0
_GPU_BASE       = 300
_GPU_CURVE      = 0.5
_TEST_DURATION  = 5.0


# ---------------------------------------------------------------------------
# Backends
# ---------------------------------------------------------------------------

def _bench_cupy(duration: float) -> Tuple[float, str]:
    import cupy as cp  # type: ignore

    N = 4096
    A = cp.random.rand(N, N, dtype=cp.float32)
    B = cp.random.rand(N, N, dtype=cp.float32)
    cp.cuda.Stream.null.synchronize()

    ops = 0
    start = time.perf_counter()
    while time.perf_counter() - start < duration:
        cp.dot(A, B)
        cp.cuda.Stream.null.synchronize()
        ops += 2 * N ** 3

    elapsed = time.perf_counter() - start
    gflops = ops / elapsed / 1e9

    try:
        dev_name = cp.cuda.runtime.getDeviceProperties(0)["name"]
        if isinstance(dev_name, bytes):
            dev_name = dev_name.decode()
        backend = f"CUDA – {dev_name}"
    except Exception:
        backend = "CUDA/ROCm"

    return gflops, backend


def _bench_opencl(duration: float) -> Tuple[float, str]:
    import pyopencl as cl          # type: ignore
    import pyopencl.array as cla   # type: ignore

    ctx  = cl.create_some_context(interactive=False)
    queue = cl.CommandQueue(ctx)

    N = 2048
    A = cla.to_device(queue, np.random.rand(N, N).astype(np.float32))
    B = cla.to_device(queue, np.random.rand(N, N).astype(np.float32))

    # Use PyOpenCL's built-in matmul (relies on clBLAS / clblast if available,
    # else pure OpenCL kernel)
    ops = 0
    start = time.perf_counter()
    while time.perf_counter() - start < duration:
        C = cla.dot(A, B)         # type: ignore[attr-defined]
        queue.finish()
        ops += 2 * N ** 3

    elapsed = time.perf_counter() - start
    gflops = ops / elapsed / 1e9

    dev_name = ctx.devices[0].name
    return gflops, f"OpenCL – {dev_name}"


def _bench_cublas_ctypes(duration: float) -> Tuple[float, str]:
    """
    FP32 GEMM via CUDA Driver API + cuBLAS using ctypes only.

    Requires NVIDIA display drivers (libcuda / nvcuda.dll) and the CUDA
    toolkit runtime libraries (libcublas).  No Python packages needed —
    both ship with a standard NVIDIA CUDA installation.

    Uses N=4096 SGEMM via cuBLAS, which achieves near-peak TFLOPS and
    produces scores comparable to the cupy backend.
    """
    import ctypes
    import ctypes.util

    import os

    # ---- Load libraries -----------------------------------------------------
    # Extra search paths for CUDA (common on Arch/CachyOS where cuda installs
    # to /opt/cuda which isn't always in ldconfig's search path).
    _cuda_lib_dirs = []
    for _var in ('CUDA_HOME', 'CUDA_PATH', 'CUDA_ROOT'):
        _p = os.environ.get(_var)
        if _p:
            _cuda_lib_dirs.append(os.path.join(_p, 'lib64'))
    _cuda_lib_dirs += [
        '/usr/local/cuda/lib64',
        '/opt/cuda/lib64',
        '/usr/lib/x86_64-linux-gnu',
        '/usr/lib64',
        '/usr/lib',
    ]

    def _load(names, dirs=()):
        """Try loading a shared library by name, then by full path in dirs."""
        for _n in names:
            try:
                return ctypes.CDLL(_n)
            except OSError:
                pass
        for _d in dirs:
            for _n in names:
                try:
                    return ctypes.CDLL(os.path.join(_d, _n))
                except OSError:
                    pass
        return None

    if sys.platform == 'win32':
        _cuda = _load(['nvcuda'])
        if _cuda is None:
            raise RuntimeError('nvcuda.dll not found — install NVIDIA drivers')
        _cublas = _load(['cublas64_12', 'cublas64_11', 'cublas64_10'])
    else:
        _cuda = _load(
            ['libcuda.so.1', 'libcuda.so'],
            _cuda_lib_dirs,
        )
        if _cuda is None:
            raise RuntimeError('libcuda.so not found — install NVIDIA drivers')
        _cublas = _load(
            ['libcublas.so.12', 'libcublas.so.11', 'libcublas.so'],
            _cuda_lib_dirs,
        )

    if _cublas is None:
        raise RuntimeError(
            'libcublas not found — install CUDA toolkit '
            f'(searched: {", ".join(_cuda_lib_dirs)})'
        )

    # ---- Type aliases -------------------------------------------------------
    i32 = ctypes.c_int32
    f32 = ctypes.c_float
    vp  = ctypes.c_void_p
    dp  = ctypes.c_uint64   # CUdeviceptr (64-bit device address)

    # ---- Return types -------------------------------------------------------
    for _fn in ('cuInit', 'cuDeviceGet', 'cuDeviceGetName',
                'cuCtxCreate_v2', 'cuMemAlloc_v2', 'cuMemcpyHtoD_v2',
                'cuMemFree_v2', 'cuCtxSynchronize'):
        getattr(_cuda, _fn).restype = i32
    for _fn in ('cublasCreate_v2', 'cublasDestroy_v2', 'cublasSgemm_v2'):
        getattr(_cublas, _fn).restype = i32

    CUDA_SUCCESS   = 0
    CUBLAS_SUCCESS = 0
    CUBLAS_OP_N    = 0   # no transpose

    # ---- Initialise CUDA ----------------------------------------------------
    if _cuda.cuInit(0) != CUDA_SUCCESS:
        raise RuntimeError('cuInit failed — no NVIDIA GPU present')

    dev = i32(0)
    _cuda.cuDeviceGet(ctypes.byref(dev), 0)

    name_buf = ctypes.create_string_buffer(256)
    _cuda.cuDeviceGetName(name_buf, 256, dev)
    dev_name = name_buf.value.decode('utf-8', errors='replace').strip()

    ctx = vp(0)
    if _cuda.cuCtxCreate_v2(ctypes.byref(ctx), 0, dev) != CUDA_SUCCESS:
        raise RuntimeError('cuCtxCreate_v2 failed')

    # ---- cuBLAS handle ------------------------------------------------------
    handle = vp(0)
    if _cublas.cublasCreate_v2(ctypes.byref(handle)) != CUBLAS_SUCCESS:
        raise RuntimeError('cublasCreate_v2 failed')

    # ---- Device memory ------------------------------------------------------
    N      = 4096
    nbytes = N * N * ctypes.sizeof(f32)
    A_h    = np.random.rand(N, N).astype(np.float32)
    B_h    = np.random.rand(N, N).astype(np.float32)

    d_A = dp(0); d_B = dp(0); d_C = dp(0)
    _cuda.cuMemAlloc_v2(ctypes.byref(d_A), nbytes)
    _cuda.cuMemAlloc_v2(ctypes.byref(d_B), nbytes)
    _cuda.cuMemAlloc_v2(ctypes.byref(d_C), nbytes)
    _cuda.cuMemcpyHtoD_v2(d_A, A_h.ctypes.data_as(vp), nbytes)
    _cuda.cuMemcpyHtoD_v2(d_B, B_h.ctypes.data_as(vp), nbytes)

    alpha = f32(1.0)
    beta  = f32(0.0)

    # cuBLAS is column-major; swapping A↔B in the call produces A×B
    # in the row-major layout that numpy uses.
    def _gemm():
        return _cublas.cublasSgemm_v2(
            handle,
            i32(CUBLAS_OP_N), i32(CUBLAS_OP_N),
            i32(N), i32(N), i32(N),
            ctypes.byref(alpha),
            d_B, i32(N),
            d_A, i32(N),
            ctypes.byref(beta),
            d_C, i32(N),
        )

    # Warm-up
    _gemm()
    _cuda.cuCtxSynchronize()

    # ---- Benchmark ----------------------------------------------------------
    ops, start = 0, time.perf_counter()
    while time.perf_counter() - start < duration:
        _gemm()
        _cuda.cuCtxSynchronize()
        ops += 2 * N ** 3
    elapsed = time.perf_counter() - start
    gflops  = ops / elapsed / 1e9

    # ---- Cleanup ------------------------------------------------------------
    _cuda.cuMemFree_v2(d_A)
    _cuda.cuMemFree_v2(d_B)
    _cuda.cuMemFree_v2(d_C)
    _cublas.cublasDestroy_v2(handle)

    return gflops, f'CUDA (cuBLAS) – {dev_name}'


def _bench_opencl_ctypes(duration: float) -> Tuple[float, str]:
    """
    Tiled 16×16 GEMM via raw OpenCL calls through ctypes.

    Requires only GPU drivers (OpenCL.dll on Windows, libOpenCL on Linux/macOS).
    No extra Python packages needed — works from a PyInstaller bundle directly.
    """
    import ctypes
    import ctypes.util

    import os

    # ---- Load OpenCL shared library ----------------------------------
    if sys.platform == 'win32':
        try:
            _cl = ctypes.WinDLL('OpenCL')
        except OSError:
            raise RuntimeError('OpenCL.dll not found – install GPU drivers')
    elif sys.platform == 'darwin':
        _cl = ctypes.CDLL('/System/Library/Frameworks/OpenCL.framework/OpenCL')
    else:
        # Mesa's RustiCL OpenCL implementation registers zero platforms
        # (not one platform with zero devices) unless RUSTICL_ENABLE names
        # the Gallium driver — this is why plenty of Mesa systems with a
        # perfectly good iGPU/APU report "No OpenCL platforms found" out of
        # the box (e.g. handheld gaming images like Bazzite/SteamOS).
        # Harmless everywhere else: proprietary AMD/NVIDIA/Intel OpenCL
        # runtimes ignore this variable entirely.
        os.environ.setdefault('RUSTICL_ENABLE', 'radeonsi:iris')

        # ctypes.util.find_library('OpenCL') relies on ldconfig.
        # On systems where the ICD loader (ocl-icd) is installed but not yet
        # cached by ldconfig, or where the .so lives outside the default paths
        # (common with Mesa on Arch/CachyOS), find_library returns None even
        # though the library is present.  Try explicit paths as fallback.
        _lib = ctypes.util.find_library('OpenCL')
        if not _lib:
            for _candidate in (
                'libOpenCL.so.1',
                '/usr/lib/libOpenCL.so.1',
                '/usr/lib64/libOpenCL.so.1',
                '/usr/lib/x86_64-linux-gnu/libOpenCL.so.1',
                '/usr/local/lib/libOpenCL.so.1',
            ):
                try:
                    ctypes.CDLL(_candidate)
                    _lib = _candidate
                    break
                except OSError:
                    pass
        if not _lib:
            raise RuntimeError(
                'libOpenCL not found.\n'
                'For AMD GPU with Mesa, install: ocl-icd opencl-rusticl-mesa\n'
                '  Arch/CachyOS: sudo pacman -S ocl-icd opencl-rusticl-mesa\n'
                '  Ubuntu/Debian: sudo apt install ocl-icd-libopencl1 mesa-opencl-icd\n'
                '  Fedora Atomic/Bazzite/SteamOS: rpm-ostree install mesa-libOpenCL (reboot after)'
            )
        _cl = ctypes.CDLL(_lib)

    # ---- Return-type annotations -------------------------------------
    vp = ctypes.c_void_p
    i32, u32, u64, sz = ctypes.c_int32, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_size_t
    for name, rt in [
        ('clGetPlatformIDs',          i32),
        ('clGetDeviceIDs',            i32),
        ('clGetDeviceInfo',           i32),
        ('clCreateContext',           vp),
        ('clCreateCommandQueue',      vp),
        ('clCreateProgramWithSource', vp),
        ('clBuildProgram',            i32),
        ('clGetProgramBuildInfo',     i32),
        ('clCreateKernel',            vp),
        ('clCreateBuffer',            vp),
        ('clSetKernelArg',            i32),
        ('clEnqueueWriteBuffer',      i32),
        ('clEnqueueNDRangeKernel',    i32),
        ('clFinish',                  i32),
        ('clReleaseMemObject',        i32),
        ('clReleaseKernel',           i32),
        ('clReleaseProgram',          i32),
        ('clReleaseCommandQueue',     i32),
        ('clReleaseContext',          i32),
    ]:
        getattr(_cl, name).restype = rt

    CL_SUCCESS         = 0
    CL_DEVICE_TYPE_GPU = u64(4)
    CL_DEVICE_NAME     = u32(0x102B)
    CL_MEM_READ_ONLY   = 4    # 1 << 2
    CL_MEM_WRITE_ONLY  = 2    # 1 << 1

    # ---- Find first OpenCL GPU device --------------------------------
    n_plat = u32(0)
    if _cl.clGetPlatformIDs(0, None, ctypes.byref(n_plat)) != CL_SUCCESS or not n_plat.value:
        raise RuntimeError(
            'No OpenCL platforms found.\n'
            "Mesa's RustiCL registers zero platforms unless RUSTICL_ENABLE names your "
            'GPU driver (already set to "radeonsi:iris" by SynBench) — if this still '
            'fails, the RustiCL ICD itself is likely missing from this OS image.\n'
            '  Fedora Atomic/Bazzite/SteamOS: rpm-ostree install mesa-libOpenCL (reboot after)\n'
            '  Arch/CachyOS: sudo pacman -S ocl-icd opencl-rusticl-mesa\n'
            '  Ubuntu/Debian: sudo apt install ocl-icd-libopencl1 mesa-opencl-icd'
        )
    plats = (vp * n_plat.value)()
    _cl.clGetPlatformIDs(n_plat.value, plats, None)

    gpu = None
    for p in plats:
        n_dev = u32(0)
        if (_cl.clGetDeviceIDs(vp(p), CL_DEVICE_TYPE_GPU, 0, None, ctypes.byref(n_dev))
                == CL_SUCCESS and n_dev.value):
            devs = (vp * n_dev.value)()
            _cl.clGetDeviceIDs(vp(p), CL_DEVICE_TYPE_GPU, n_dev.value, devs, None)
            gpu = vp(devs[0])
            break
    if gpu is None:
        raise RuntimeError(
            'No OpenCL GPU device found.\n'
            'Mesa AMD: install ocl-icd and opencl-rusticl-mesa (or opencl-mesa),\n'
            'then ensure /etc/OpenCL/vendors/rusticl.icd (or radeon.icd) exists.'
        )

    name_buf = ctypes.create_string_buffer(256)
    _cl.clGetDeviceInfo(gpu, CL_DEVICE_NAME, 256, name_buf, None)
    dev_name = name_buf.value.decode('utf-8', errors='replace').strip()

    # Query max work-group size — Mesa / Clover can be as low as 256 (16×16).
    # Fall back to an 8×8 tile if the device can't support 16×16.
    CL_DEVICE_MAX_WORK_GROUP_SIZE = u32(0x1004)
    _wg = sz(0)
    _cl.clGetDeviceInfo(gpu, CL_DEVICE_MAX_WORK_GROUP_SIZE,
                        ctypes.sizeof(sz), ctypes.byref(_wg), None)
    TILE = 16 if _wg.value >= 256 else 8

    # ---- Context and queue -------------------------------------------
    err = i32(0)
    ctx = vp(_cl.clCreateContext(None, 1, ctypes.byref(gpu), None, None, ctypes.byref(err)))
    if err.value != CL_SUCCESS:
        raise RuntimeError(f'clCreateContext failed ({err.value})')
    queue = vp(_cl.clCreateCommandQueue(ctx, gpu, 0, ctypes.byref(err)))
    if err.value != CL_SUCCESS:
        raise RuntimeError(f'clCreateCommandQueue failed ({err.value})')

    # ---- Tiled GEMM kernel -------------------------------------------
    # N must be a multiple of TILE.
    N = 2048 if TILE == 16 else 1024
    src = (f"""
#define T {TILE}
__kernel void gemm(__global const float* A, __global const float* B,
                   __global float* C, int N) {{
    __local float tA[T][T], tB[T][T];
    int r=get_global_id(0), c=get_global_id(1);
    int lr=get_local_id(0), lc=get_local_id(1);
    float s=0.f;
    for(int t=0;t<N/T;t++){{
        tA[lr][lc]=A[r*N+t*T+lc]; tB[lr][lc]=B[(t*T+lr)*N+c];
        barrier(CLK_LOCAL_MEM_FENCE);
        for(int k=0;k<T;k++) s+=tA[lr][k]*tB[k][lc];
        barrier(CLK_LOCAL_MEM_FENCE);
    }}
    C[r*N+c]=s;
}}""").encode()

    src_ptr, src_len = ctypes.c_char_p(src), sz(len(src))
    prog = vp(_cl.clCreateProgramWithSource(
        ctx, 1, ctypes.byref(src_ptr), ctypes.byref(src_len), ctypes.byref(err)))
    if _cl.clBuildProgram(prog, 1, ctypes.byref(gpu), None, None, None) != CL_SUCCESS:
        # Retrieve the build log so the error message is actually useful.
        CL_PROGRAM_BUILD_LOG = u32(0x1183)
        _log_sz = sz(0)
        _cl.clGetProgramBuildInfo(prog, gpu, CL_PROGRAM_BUILD_LOG,
                                  0, None, ctypes.byref(_log_sz))
        _log_buf = ctypes.create_string_buffer(_log_sz.value or 1)
        _cl.clGetProgramBuildInfo(prog, gpu, CL_PROGRAM_BUILD_LOG,
                                  _log_sz.value, _log_buf, None)
        build_log = _log_buf.value.decode('utf-8', errors='replace').strip()
        raise RuntimeError(f'clBuildProgram failed:\n{build_log}')
    kern = vp(_cl.clCreateKernel(prog, b'gemm', ctypes.byref(err)))
    if err.value != CL_SUCCESS:
        raise RuntimeError(f'clCreateKernel failed ({err.value})')

    # ---- Buffers -------------------------------------------------------
    # Use clEnqueueWriteBuffer rather than CL_MEM_COPY_HOST_PTR: Mesa's
    # Rusticl has had bugs with the latter when the host pointer is not
    # page-aligned (numpy doesn't guarantee page alignment).
    nbytes = N * N * 4
    A_arr  = np.random.rand(N, N).astype(np.float32)
    B_arr  = np.random.rand(N, N).astype(np.float32)
    buf_A  = vp(_cl.clCreateBuffer(ctx, CL_MEM_READ_ONLY,  nbytes, None, ctypes.byref(err)))
    buf_B  = vp(_cl.clCreateBuffer(ctx, CL_MEM_READ_ONLY,  nbytes, None, ctypes.byref(err)))
    buf_C  = vp(_cl.clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, nbytes, None, ctypes.byref(err)))
    CL_TRUE = i32(1)
    _cl.clEnqueueWriteBuffer(queue, buf_A, CL_TRUE, 0, nbytes,
                             A_arr.ctypes.data_as(vp), 0, None, None)
    _cl.clEnqueueWriteBuffer(queue, buf_B, CL_TRUE, 0, nbytes,
                             B_arr.ctypes.data_as(vp), 0, None, None)
    n_arg  = i32(N)
    for i, (val, sz_) in enumerate([
        (ctypes.byref(buf_A), ctypes.sizeof(vp)),
        (ctypes.byref(buf_B), ctypes.sizeof(vp)),
        (ctypes.byref(buf_C), ctypes.sizeof(vp)),
        (ctypes.byref(n_arg), ctypes.sizeof(i32)),
    ]):
        _cl.clSetKernelArg(kern, i, sz_, val)

    gws = (sz * 2)(N, N)
    lws = (sz * 2)(TILE, TILE)

    # Warm-up
    _cl.clEnqueueNDRangeKernel(queue, kern, 2, None, gws, lws, 0, None, None)
    _cl.clFinish(queue)

    # ---- Benchmark loop ----------------------------------------------
    ops, start = 0, time.perf_counter()
    while time.perf_counter() - start < duration:
        _cl.clEnqueueNDRangeKernel(queue, kern, 2, None, gws, lws, 0, None, None)
        _cl.clFinish(queue)
        ops += 2 * N ** 3
    elapsed = time.perf_counter() - start
    gflops  = ops / elapsed / 1e9

    # Cleanup
    for obj in (buf_A, buf_B, buf_C):
        _cl.clReleaseMemObject(obj)
    _cl.clReleaseKernel(kern)
    _cl.clReleaseProgram(prog)
    _cl.clReleaseCommandQueue(queue)
    _cl.clReleaseContext(ctx)

    return gflops, f'OpenCL – {dev_name}'


# ---------------------------------------------------------------------------
# Benchmark class
# ---------------------------------------------------------------------------

class GPUBenchmark:
    def __init__(self, callback: Optional[Callable[[str], None]] = None):
        self.cb = callback or (lambda msg: None)

    def _detect_and_run(self) -> Tuple[float, str]:
        """Try GPU backends in priority order. Raises RuntimeError if none available."""
        self.cb("Detecting GPU …")

        _backends = [
            ("CuPy",           _bench_cupy),
            ("CUDA (cuBLAS)",  _bench_cublas_ctypes),
            ("PyOpenCL",       _bench_opencl),
            ("OpenCL",         _bench_opencl_ctypes),
        ]

        for label, fn in _backends:
            self.cb(f"Trying {label} …")
            try:
                gflops, backend = fn(_TEST_DURATION)
                self.cb(f"GPU detected: {backend}")
                return gflops, backend
            except Exception as exc:
                self.cb(f"  {label} unavailable: {exc}")

        raise RuntimeError("No GPU runtime found")

    def run(self) -> BenchmarkResult:
        self.cb("Starting GPU compute benchmark …")

        try:
            gflops, backend = self._detect_and_run()
        except RuntimeError:
            self.cb("No GPU detected – skipped.")
            return BenchmarkResult(
                name="gpu",
                score=0,
                available=False,
                summary="No GPU or OpenCL drivers found",
                error="Install GPU drivers or: pip install cupy-cuda12x / pyopencl",
            )

        score = int(_GPU_BASE * (gflops / _GPU_GFLOPS_REF) ** _GPU_CURVE)
        summary = f"{gflops:.1f} GFLOPS  [{backend}]"
        self.cb(f"GPU done — score {score}")

        return BenchmarkResult(
            name="gpu",
            score=score,
            metrics={
                "gflops":   gflops,
                "backend":  backend,
            },
            summary=summary,
        )

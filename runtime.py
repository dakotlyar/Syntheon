"""
Runtime dependency installer for SynBench.

Detects GPU and NPU hardware present on the system and, when the
corresponding Python packages are not yet importable, offers to install
them into a side-car 'synbench_plugins/' directory placed next to the
executable.  The plugins directory is prepended to sys.path so that
installed packages are found by subsequent imports.

Public API
----------
setup_path()              – call once at process start to activate plugins dir
get_recommendations()     – returns List[RuntimeRecommendation] for detected hw
install_package()         – pip-install one package into the plugins dir
runtime_setup_skipped()   – True if the user checked "don't ask again"
set_runtime_setup_skipped()
"""

import json
import os
import subprocess
import sys
from typing import Callable, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Plugins directory
# ---------------------------------------------------------------------------

def _resolve_plugins_dir() -> str:
    """Return a writable directory for side-car package installs."""
    if getattr(sys, 'frozen', False):
        base = os.path.dirname(sys.executable)
    else:
        base = os.path.dirname(os.path.abspath(__file__))
    candidate = os.path.join(base, 'synbench_plugins')
    try:
        os.makedirs(candidate, exist_ok=True)
        probe = os.path.join(candidate, '.write_probe')
        open(probe, 'w').close()
        os.unlink(probe)
        return candidate
    except OSError:
        pass
    fallback = os.path.join(os.path.expanduser('~'), '.synbench', 'plugins')
    os.makedirs(fallback, exist_ok=True)
    return fallback


PLUGINS_DIR: str = _resolve_plugins_dir()


def setup_path() -> None:
    """Append the plugins directory to sys.path (idempotent).

    Appending (not prepending) ensures that packages already bundled inside
    the PyInstaller AppImage are always found first.  The plugins dir only
    supplies packages that are absent from the bundle (cupy, pyopencl,
    onnxruntime) so it never needs priority over bundled packages.
    """
    if PLUGINS_DIR not in sys.path:
        sys.path.append(PLUGINS_DIR)


# ---------------------------------------------------------------------------
# Persistent user preference
# ---------------------------------------------------------------------------

def _config_path() -> str:
    home_cfg = os.path.join(os.path.expanduser('~'), '.synbench', 'config.json')
    os.makedirs(os.path.dirname(home_cfg), exist_ok=True)
    return home_cfg


def _load_config() -> dict:
    try:
        with open(_config_path()) as f:
            return json.load(f)
    except Exception:
        return {}


def _save_config(cfg: dict) -> None:
    try:
        with open(_config_path(), 'w') as f:
            json.dump(cfg, f, indent=2)
    except Exception:
        pass


def runtime_setup_skipped() -> bool:
    return bool(_load_config().get('skip_runtime_setup', False))


def set_runtime_setup_skipped(value: bool) -> None:
    cfg = _load_config()
    cfg['skip_runtime_setup'] = value
    _save_config(cfg)


# ---------------------------------------------------------------------------
# Hardware detection helpers
# ---------------------------------------------------------------------------

def _run(*args: str, timeout: int = 4) -> Optional[str]:
    try:
        r = subprocess.run(list(args), capture_output=True, text=True, timeout=timeout)
        return r.stdout if r.returncode == 0 else None
    except Exception:
        return None


def has_nvidia_gpu() -> bool:
    return _run('nvidia-smi', '--query-gpu=name', '--format=csv,noheader') is not None


def has_amd_rocm() -> bool:
    out = _run('rocm-smi', '--showproductname')
    return bool(out and out.strip())


def has_opencl() -> bool:
    """Return True when an OpenCL ICD or library is present."""
    if sys.platform.startswith('linux'):
        import glob
        icd_dir = '/etc/OpenCL/vendors'
        if os.path.isdir(icd_dir) and os.listdir(icd_dir):
            return True
        return bool(
            glob.glob('/usr/lib*/libOpenCL*') or
            glob.glob('/usr/local/lib*/libOpenCL*')
        )
    if sys.platform == 'win32':
        try:
            import ctypes
            ctypes.WinDLL('OpenCL')
            return True
        except OSError:
            return False
    # macOS: OpenCL always available
    return sys.platform == 'darwin'


def has_npu() -> bool:
    """Return True when an NPU / AI accelerator appears to be present."""
    if sys.platform.startswith('linux'):
        # Kernel accel device nodes (Intel NPU, AMD XDNA, …)
        accel_dir = '/dev/accel'
        if os.path.isdir(accel_dir) and os.listdir(accel_dir):
            return True
        # Loaded driver modules
        try:
            modules = open('/proc/modules').read()
            npu_drivers = ('intel_vpu', 'amdxdna', 'qcom_q6v5')
            return any(d in modules for d in npu_drivers)
        except OSError:
            return False

    if sys.platform == 'win32':
        out = _run(
            'powershell', '-NoProfile', '-Command',
            'Get-PnpDevice -Status OK | '
            'Where-Object { $_.FriendlyName -match "NPU|Neural|VPU" } | '
            'Select-Object -ExpandProperty FriendlyName',
            timeout=8,
        )
        return bool(out and out.strip())

    if sys.platform == 'darwin':
        # Apple Neural Engine is present on all Apple Silicon Macs
        r = _run('sysctl', '-n', 'hw.optional.arm64')
        return r is not None and r.strip() == '1'

    return False


# ---------------------------------------------------------------------------
# Package availability
# ---------------------------------------------------------------------------

def _importable(module_name: str) -> bool:
    """Return True if *module_name* can be imported (checks plugins dir too)."""
    setup_path()
    import importlib.util
    return importlib.util.find_spec(module_name) is not None


# ---------------------------------------------------------------------------
# Recommendations
# ---------------------------------------------------------------------------

class RuntimeRecommendation:
    """One optional package that could improve a benchmark on this machine."""

    __slots__ = ('label', 'pip_name', 'import_name', 'reason', 'category')

    def __init__(
        self,
        label: str,
        pip_name: str,
        import_name: str,
        reason: str,
        category: str,  # 'gpu' or 'npu'
    ):
        self.label       = label
        self.pip_name    = pip_name
        self.import_name = import_name
        self.reason      = reason
        self.category    = category

    def is_installed(self) -> bool:
        return _importable(self.import_name)


def get_recommendations() -> List[RuntimeRecommendation]:
    """Return packages recommended for the hardware found on this machine."""
    recs: List[RuntimeRecommendation] = []

    cupy_ok   = _importable('cupy')
    opencl_ok = _importable('pyopencl')
    ort_ok    = _importable('onnxruntime')

    nvidia = has_nvidia_gpu()
    amd    = has_amd_rocm()
    opencl = has_opencl()
    npu    = has_npu()

    # --- GPU ---
    if nvidia and not cupy_ok:
        recs.append(RuntimeRecommendation(
            label='cupy-cuda12x',
            pip_name='cupy-cuda12x',
            import_name='cupy',
            reason='NVIDIA GPU detected — cuBLAS backend scores higher than the built-in OpenCL benchmark',
            category='gpu',
        ))
    elif amd and not cupy_ok:
        recs.append(RuntimeRecommendation(
            label='cupy-rocm-5-0',
            pip_name='cupy-rocm-5-0',
            import_name='cupy',
            reason='AMD GPU detected — ROCm backend scores higher than the built-in OpenCL benchmark',
            category='gpu',
        ))

    # --- NPU ---
    if npu and not ort_ok:
        recs.append(RuntimeRecommendation(
            label='onnxruntime',
            pip_name='onnxruntime',
            import_name='onnxruntime',
            reason='NPU / AI accelerator detected — enables AI inference benchmark',
            category='npu',
        ))

    return recs


# ---------------------------------------------------------------------------
# Installer
# ---------------------------------------------------------------------------

def _bundled_pip_cmd() -> Optional[List[str]]:
    """Return a command for the sidecar Python shipped in 'python-embed/'
    next to the executable, if the build included one.

    Frozen builds don't have a real interpreter behind sys.executable, so
    this sidecar is what lets plugin installs work on a freshly-imaged
    machine with no system Python at all.
    """
    base = os.path.dirname(sys.executable)
    if sys.platform == 'win32':
        candidate = os.path.join(base, 'python-embed', 'python.exe')
        return [candidate, '-m', 'pip'] if os.path.isfile(candidate) else None

    import glob
    matches = sorted(glob.glob(os.path.join(base, 'python-embed', 'bin', 'python3*')))
    return [matches[0], '-m', 'pip'] if matches else None


def _find_pip_cmd() -> Optional[List[str]]:
    """Return a command list that invokes pip, or None if none found."""
    import shutil

    # When running as a plain Python script, sys.executable IS Python.
    if not getattr(sys, 'frozen', False):
        return [sys.executable, '-m', 'pip']

    bundled = _bundled_pip_cmd()
    if bundled:
        return bundled

    # No sidecar shipped (or it's missing) — fall back to a system Python.
    for name in ('python3', 'python'):
        py = shutil.which(name)
        if not py:
            continue
        try:
            r = subprocess.run([py, '-m', 'pip', '--version'],
                               capture_output=True, timeout=8)
            if r.returncode == 0:
                return [py, '-m', 'pip']
        except Exception:
            pass

    # Try standalone pip / pip3 on PATH.
    for name in ('pip3', 'pip'):
        pip = shutil.which(name)
        if pip:
            return [pip]

    return None


def install_package(
    pip_name: str,
    progress_cb: Optional[Callable[[str], None]] = None,
) -> Tuple[bool, str]:
    """
    Install *pip_name* into PLUGINS_DIR using pip.

    progress_cb(line) is called for each line of pip output.
    Returns (success, human-readable message).
    """
    cmd = _find_pip_cmd()
    if cmd is None:
        msg = (
            f'pip not found on PATH.\n'
            f'Install manually:  pip install --target "{PLUGINS_DIR}" {pip_name}'
        )
        if progress_cb:
            progress_cb(msg)
        return False, msg

    full_cmd = cmd + ['install', '--target', PLUGINS_DIR, pip_name]
    if progress_cb:
        progress_cb(f'$ {" ".join(full_cmd)}')

    try:
        proc = subprocess.Popen(
            full_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        for line in proc.stdout:
            if progress_cb:
                progress_cb(line.rstrip())
        proc.wait()
    except Exception as exc:
        return False, str(exc)

    if proc.returncode == 0:
        setup_path()
        return True, f'Installed {pip_name} into {PLUGINS_DIR}'
    return False, f'pip exited with code {proc.returncode}'

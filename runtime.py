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


def detect_npu_vendor() -> Optional[str]:
    """Return 'qualcomm' | 'intel' | 'amd' | 'apple' when an NPU / AI
    accelerator appears to be present, else None.

    The vendor matters, not just presence: the NPU benchmark only ever
    targets a genuine NPU-specific ONNX Runtime execution provider for
    that vendor — it deliberately does not fall back to CPU or GPU
    inference, so knowing *which* NPU is here decides which provider
    (and which onnxruntime package) applies. See NPU_PROVIDER_INFO.
    """
    if sys.platform.startswith('linux'):
        # Kernel accel device nodes (Intel NPU, AMD XDNA, …) — inspect the
        # driver serving each node before falling back to module names.
        accel_dir = '/dev/accel'
        if os.path.isdir(accel_dir) and os.listdir(accel_dir):
            for name in os.listdir(accel_dir):
                driver = os.path.realpath(f'/sys/class/accel/{name}/device/driver') or ''
                if 'intel_vpu' in driver:
                    return 'intel'
                if 'amdxdna' in driver:
                    return 'amd'

        try:
            modules = open('/proc/modules').read()
        except OSError:
            modules = ''
        if 'intel_vpu' in modules:
            return 'intel'
        if 'amdxdna' in modules:
            return 'amd'
        if 'qcom_q6v5' in modules:
            return 'qualcomm'
        return None

    if sys.platform == 'win32':
        out = _run(
            'powershell', '-NoProfile', '-Command',
            'Get-PnpDevice -Status OK | '
            'Where-Object { $_.FriendlyName -match "NPU|Neural|VPU" } | '
            'Select-Object -ExpandProperty FriendlyName',
            timeout=8,
        )
        if not out or not out.strip():
            return None
        text = out.lower()
        if 'qualcomm' in text or 'hexagon' in text or 'snapdragon' in text:
            return 'qualcomm'
        if 'intel' in text:
            return 'intel'
        if 'amd' in text or 'ryzen' in text or 'xdna' in text:
            return 'amd'
        return None

    if sys.platform == 'darwin':
        # Apple Neural Engine is present on all Apple Silicon Macs
        r = _run('sysctl', '-n', 'hw.optional.arm64')
        return 'apple' if (r is not None and r.strip() == '1') else None

    return None


def has_npu() -> bool:
    """Return True when an NPU / AI accelerator appears to be present."""
    return detect_npu_vendor() is not None


# ---------------------------------------------------------------------------
# NPU vendor -> ONNX Runtime provider mapping
# ---------------------------------------------------------------------------
# Only vendors with a genuine, pip-installable, NPU-exclusive execution
# provider are listed here. A vendor detected but absent from this dict
# (e.g. 'amd' — Ryzen AI's Vitis-AI EP needs a separate manual SDK install,
# not a plain pip package) is reported as "NPU detected, unsupported" by
# the benchmark rather than silently substituting CPU/GPU inference.
NPU_PROVIDER_INFO = {
    'qualcomm': {
        'pip_name': 'onnxruntime-qnn',
        'provider': 'QNNExecutionProvider',
        'provider_options': {
            'backend_path': 'QnnHtp.dll' if sys.platform == 'win32' else 'libQnnHtp.so',
        },
        'label': 'Qualcomm Hexagon NPU (QNN)',
    },
    'intel': {
        'pip_name': 'onnxruntime-openvino',
        'provider': 'OpenVINOExecutionProvider',
        'provider_options': {'device_type': 'NPU'},
        'label': 'Intel AI Boost NPU (OpenVINO)',
    },
    'apple': {
        # The standard macOS onnxruntime wheel already bundles CoreML —
        # no separate package needed.
        'pip_name': 'onnxruntime',
        'provider': 'CoreMLExecutionProvider',
        'provider_options': {'MLComputeUnits': 'CPUAndNeuralEngine'},
        'label': 'Apple Neural Engine (CoreML)',
    },
}


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

    __slots__ = ('label', 'pip_name', 'import_name', 'reason', 'category', 'required_provider')

    def __init__(
        self,
        label: str,
        pip_name: str,
        import_name: str,
        reason: str,
        category: str,  # 'gpu' or 'npu'
        required_provider: Optional[str] = None,
    ):
        self.label       = label
        self.pip_name    = pip_name
        self.import_name = import_name
        self.reason      = reason
        self.category    = category
        # For NPU recommendations: the module being importable isn't enough
        # to call it "installed" — a CPU-only onnxruntime import succeeds
        # too. Also require this specific execution provider to be present.
        self.required_provider = required_provider

    def is_installed(self) -> bool:
        if not _importable(self.import_name):
            return False
        if self.required_provider:
            try:
                import onnxruntime as ort
                return self.required_provider in ort.get_available_providers()
            except Exception:
                return False
        return True


def get_recommendations() -> List[RuntimeRecommendation]:
    """Return packages recommended for the hardware found on this machine."""
    recs: List[RuntimeRecommendation] = []

    cupy_ok   = _importable('cupy')
    opencl_ok = _importable('pyopencl')

    nvidia = has_nvidia_gpu()
    amd    = has_amd_rocm()
    opencl = has_opencl()
    npu_vendor = detect_npu_vendor()

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
    # The NPU benchmark only ever targets a genuine NPU-specific execution
    # provider for the detected vendor — never CPU or GPU inference — so a
    # recommendation only makes sense when NPU_PROVIDER_INFO actually has
    # an entry for this vendor (e.g. 'amd' currently doesn't: Ryzen AI's
    # Vitis-AI EP needs a separate manual SDK, not a plain pip package).
    info = NPU_PROVIDER_INFO.get(npu_vendor) if npu_vendor else None
    if info:
        rec = RuntimeRecommendation(
            label=info['pip_name'],
            pip_name=info['pip_name'],
            import_name='onnxruntime',
            reason=f'{info["label"]} detected — enables the NPU-targeted AI inference benchmark',
            category='npu',
            required_provider=info['provider'],
        )
        if not rec.is_installed():
            recs.append(rec)

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

    # onnxruntime's variants (onnxruntime / onnxruntime-gpu / -openvino /
    # -qnn / -directml, …) are different pip distributions that all install
    # the same top-level 'onnxruntime' package — pip's --target mode won't
    # remove a previous variant's files when swapping to a different one,
    # which can leave a broken mix of two builds' files. Clear any existing
    # onnxruntime install first so a variant switch is always a clean swap.
    if pip_name.startswith('onnxruntime'):
        import glob
        import shutil as _shutil
        for path in glob.glob(os.path.join(PLUGINS_DIR, 'onnxruntime*')):
            if progress_cb:
                progress_cb(f'Removing previous install: {path}')
            if os.path.isdir(path):
                _shutil.rmtree(path, ignore_errors=True)
            else:
                try:
                    os.remove(path)
                except OSError:
                    pass

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

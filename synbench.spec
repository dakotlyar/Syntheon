# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec for SynBench.

Produces a dist/SynBench/ folder containing:
  SynBench[.exe]      – GUI entry point (no console window on Windows)
  SynBench-CLI[.exe]  – CLI entry point (console window on Windows)

Both share the same DLLs / Python stdlib (PySide6, numpy, psutil).

Build commands:
  Windows:  python -m PyInstaller synbench.spec --clean
  Linux:    python3 -m PyInstaller synbench.spec --clean
  macOS:    python3 -m PyInstaller synbench.spec --clean
"""

import sys

block_cipher = None

a = Analysis(
    ['synbench.py'],
    pathex=['.'],
    binaries=[],
    datas=[],
    hiddenimports=[
        # Local packages (deferred imports inside functions not caught by static analysis)
        'runtime',
        'benchmarks',
        'benchmarks.cpu',
        'benchmarks.gpu',
        'benchmarks.ram',
        'benchmarks.disk',
        'benchmarks.npu',
        'gui',
        'gui.app',
        # PySide6 – PyInstaller's built-in hooks collect the Qt plugins and
        # shared libraries automatically; we just need to name the modules used.
        'PySide6.QtWidgets',
        'PySide6.QtCore',
        'PySide6.QtGui',
        'PySide6.QtOpenGL',   # offscreen 2D/3D rendering benchmark (benchmarks/gpu.py)
        # multiprocessing internals (needed for CPU benchmark worker pool)
        'multiprocessing.pool',
        'multiprocessing.queues',
        'multiprocessing.synchronize',
        'multiprocessing.managers',
        # standard library
        'ctypes',
        'ctypes.util',
        'hashlib',
        'json',
        'tempfile',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    # Exclude heavy packages that are never imported by SynBench
    excludes=[
        'matplotlib', 'scipy', 'PIL', 'Pillow',
        'PyQt5', 'PyQt6', 'PySide2',
        'wx', 'gi', 'IPython', 'jupyter',
        'pandas', 'sklearn', 'tensorflow', 'torch',
        'tkinter',
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

# ---- GUI executable (no console window on Windows) ----
exe_gui = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='SynBench',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    console=False,          # <-- No black console window when double-clicked
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    # icon='assets/synbench.ico',  # Uncomment and provide .ico to set app icon
)

# ---- CLI executable (console window for --cli output) ----
exe_cli = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='SynBench-CLI',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    console=True,           # <-- Console window shows --cli output on Windows
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

# ---- Collect into one output folder ----
coll = COLLECT(
    exe_gui,
    exe_cli,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='SynBench',
)

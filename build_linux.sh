#!/usr/bin/env bash
# Build SynBench as a self-contained AppImage for Linux.
#
# Output: SynBench-x86_64.AppImage  (or aarch64)
#
# Key design decision — portable Python:
#   uv auto-selects the x86_64_v3 Python (AVX2) on modern desktops.
#   That binary causes SIGILL on laptops/older CPUs without AVX2.
#   We download the x86_64_v2 build (SSE4.2, ~2011+) directly from
#   python-build-standalone so the AppImage runs everywhere.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

ARCH="$(uname -m)"
OUT="SynBench-${ARCH}.AppImage"

echo
echo " ============================================"
echo "  SynBench | Linux AppImage Build  ($ARCH)"
echo " ============================================"
echo

# ============================================================
# 1.  Download a portable Python from python-build-standalone
#
#     x86_64_v2 = SSE4.2 baseline (Intel Nehalem 2008+,
#     AMD Bulldozer 2011+).  Runs on virtually every modern
#     x86_64 laptop, unlike x86_64_v3 (AVX2) which uv would
#     install on a capable build machine.
#
#     aarch64 has no v2/v3 split — use the standard build.
# ============================================================
PYTHON_VERSION="3.10.15"
PBS_TAG="20241002"
PYTHON_DIR=".build_python"

if [ "$ARCH" = "x86_64" ]; then
    PBS_TARGET="x86_64_v2-unknown-linux-gnu"
elif [ "$ARCH" = "aarch64" ]; then
    PBS_TARGET="aarch64-unknown-linux-gnu"
else
    echo " ERROR: Unsupported architecture: $ARCH"
    exit 1
fi

PBS_FILE="cpython-${PYTHON_VERSION}+${PBS_TAG}-${PBS_TARGET}-install_only.tar.gz"
PBS_URL="https://github.com/indygreg/python-build-standalone/releases/download/${PBS_TAG}/${PBS_FILE}"

if [ ! -f "${PYTHON_DIR}/bin/python3.10" ]; then
    echo " Downloading portable Python ${PYTHON_VERSION} (${PBS_TARGET})..."
    TMP=$(mktemp -d)
    curl -# -L "$PBS_URL" | tar xz -C "$TMP"
    # Tarball extracts to a 'python' subdirectory
    mv "$TMP/python" "$PYTHON_DIR"
    rm -rf "$TMP"
    echo " Python downloaded."
else
    echo " Using cached Python at ${PYTHON_DIR}/"
fi

PYTHON="${PYTHON_DIR}/bin/python3.10"
echo " Python: $("$PYTHON" --version)"
echo

# Bootstrap pip into the portable interpreter itself (not just the build
# venv below) so a trimmed copy of it can be shipped as a runtime sidecar —
# see step 4. python-build-standalone's install_only builds carry the
# stdlib's vendored ensurepip wheels, so this works fully offline.
"$PYTHON" -m ensurepip --upgrade --quiet 2>/dev/null || true

# ============================================================
# 2.  Isolated build venv — no system Python, no PEP 668
# ============================================================
VENV=".build_venv"
if [ ! -d "$VENV" ]; then
    echo " Creating build virtual environment..."
    "$PYTHON" -m venv "$VENV"
fi
PYTHON="$VENV/bin/python"
echo " Venv Python: $("$PYTHON" --version)"
echo

echo " Installing build dependencies..."
"$PYTHON" -m pip install --upgrade pip --quiet

# Install numpy, psutil, PySide6 normally.
"$PYTHON" -m pip install numpy psutil PySide6 --quiet

# PyInstaller's bootloader is a small C binary that gets compiled by the
# system compiler at install time on some platforms.  On CachyOS the
# system compiler targets x86-64-v3 or v4; the resulting bootloader then
# SIGILLs on any machine that lacks those instructions (e.g. Raptor Lake
# which has AVX2 but not AVX-512).
# Force a portable baseline so the bootloader runs on any x86-64 CPU.
CFLAGS="-march=x86-64-v2" CXXFLAGS="-march=x86-64-v2" \
    "$PYTHON" -m pip install pyinstaller --quiet

echo " Dependencies installed."
echo

# ============================================================
# 3.  PyInstaller bundle
# ============================================================
echo " Running PyInstaller..."
[ -d dist/SynBench ] && rm -rf dist/SynBench
[ -d build ]         && rm -rf build
"$PYTHON" -m PyInstaller synbench.spec --clean
echo

# ============================================================
# 4.  Assemble AppDir
# ============================================================
APPDIR="SynBench.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR"
echo " Assembling AppDir..."

# Copy PyInstaller output flat into AppDir
cp -r dist/SynBench/. "$APPDIR/"

# Entry point — sets Qt plugin paths so PySide6 can find its platform plugins
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "${0}")")"

# PyInstaller 6.x puts support files in _internal/; older versions use flat layout.
INTERNAL="$HERE/_internal"
[ -d "$INTERNAL" ] || INTERNAL="$HERE"

# Tell Qt where its platform plugins (libqxcb.so, libqwayland-*.so) live.
# Without this the window never appears when running from an AppImage.
for _pdir in \
    "$INTERNAL/PySide6/Qt/plugins" \
    "$INTERNAL/PySide6/qt/plugins" \
    "$INTERNAL/PySide6/plugins"; do
    if [ -d "$_pdir" ]; then
        export QT_PLUGIN_PATH="$_pdir"
        export QT_QPA_PLATFORM_PLUGIN_PATH="$_pdir/platforms"
        break
    fi
done

# Bundled shared libraries
export LD_LIBRARY_PATH="$INTERNAL${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

exec "$HERE/SynBench" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Desktop entry
cat > "$APPDIR/SynBench.desktop" << 'EOF'
[Desktop Entry]
Name=SynBench
Comment=Synthetic System Benchmark
Exec=SynBench
Icon=synbench
Type=Application
Categories=Utility;System;
EOF

# Icon
if [ -f "assets/synbench.png" ]; then
    cp "assets/synbench.png" "$APPDIR/synbench.png"
else
    "$PYTHON" - << 'PYEOF'
import struct, zlib, pathlib

def chunk(tag, data):
    crc = zlib.crc32(tag + data) & 0xffffffff
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', crc)

def solid_png(size, r, g, b):
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0))
    row  = b'\x00' + bytes([r, g, b]) * size
    idat = chunk(b'IDAT', zlib.compress(row * size))
    return b'\x89PNG\r\n\x1a\n' + ihdr + idat + chunk(b'IEND', b'')

pathlib.Path('SynBench.AppDir/synbench.png').write_bytes(
    solid_png(256, 35, 134, 54))
PYEOF
fi

# Runtime sidecar Python — lets the in-app GPU/NPU plugin installer run
# 'pip install' on a freshly-imaged machine that has no system Python at
# all. runtime.py looks for it at <exe_dir>/python-embed/bin/python3*.
echo " Bundling sidecar Python for runtime plugin installs..."
cp -r "$PYTHON_DIR" "$APPDIR/python-embed"
rm -rf \
    "$APPDIR/python-embed/lib/python3.10/test" \
    "$APPDIR/python-embed/lib/python3.10/idlelib" \
    "$APPDIR/python-embed/lib/python3.10/tkinter" \
    "$APPDIR/python-embed/lib/python3.10/turtledemo" \
    "$APPDIR/python-embed/share" \
    "$APPDIR/python-embed/include"
find "$APPDIR/python-embed" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
find "$APPDIR/python-embed" -name '*.a' -delete

echo " AppDir ready."
echo

# ============================================================
# 5.  Get appimagetool
# ============================================================
TOOL_BIN="appimagetool-${ARCH}.AppImage"
if command -v appimagetool >/dev/null 2>&1; then
    APPTOOL="appimagetool"
elif [ -x "./$TOOL_BIN" ]; then
    APPTOOL="./$TOOL_BIN"
else
    echo " Downloading appimagetool ($ARCH)..."
    TOOL_URL="https://github.com/AppImage/AppImageKit/releases/download/continuous/${TOOL_BIN}"
    curl -# -L "$TOOL_URL" -o "./$TOOL_BIN"
    chmod +x "./$TOOL_BIN"
    APPTOOL="./$TOOL_BIN"
fi

# ============================================================
# 6.  Build the AppImage
# ============================================================
echo " Building $OUT ..."
if ! ARCH="$ARCH" "$APPTOOL" "$APPDIR" "$OUT" 2>/dev/null; then
    echo " Retrying with APPIMAGE_EXTRACT_AND_RUN=1 (FUSE2 not available)..."
    APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$ARCH" "$APPTOOL" "$APPDIR" "$OUT"
fi

echo
echo " ============================================"
echo "  Build complete!"
echo
echo "  Output: $OUT"
echo "  Built with Python ${PYTHON_VERSION} (${PBS_TARGET})"
echo "  Compatible with x86_64 CPUs from ~2011 onwards."
echo
echo "  Run:  chmod +x $OUT && ./$OUT"
echo "  CLI:  ./$OUT --cli"
echo
echo "  If you see a FUSE error at runtime:"
echo "    Arch/CachyOS : sudo pacman -S fuse2"
echo "    Ubuntu/Debian: sudo apt install libfuse2"
echo "  Or: APPIMAGE_EXTRACT_AND_RUN=1 ./$OUT"
echo " ============================================"
echo

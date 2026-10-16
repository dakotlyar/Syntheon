#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# AppDir -> AppImage with the STATIC type-2 runtime (no libfuse2 needed), for
# either architecture: squashfs packing is architecture-neutral, so the x86_64
# appimagetool builds the aarch64 image without emulation.
#
#   make-appimage.sh <AppDir> <output.AppImage> <x86_64|aarch64> [<tools-dir>]
#
# <tools-dir> (default ./appimage-tools) caches the pinned downloads; every one
# is checked against the SHA-256 below before it is used.
set -euo pipefail
appdir=${1:?usage: make-appimage.sh <AppDir> <out.AppImage> <x86_64|aarch64> [tools-dir]}
out=${2:?output file}
arch=${3:?x86_64 or aarch64}
tools=${4:-appimage-tools}

tool_version=1.9.1
tool_sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
runtime_tag=20251108
case $arch in
    x86_64)  runtime_sha=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d ;;
    aarch64) runtime_sha=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 ;;
    *) echo "unknown architecture $arch" >&2; exit 2 ;;
esac

mkdir -p "$tools"
fetch() {  # <url> <file> <sha256>
    if [[ ! -f $2 ]]; then curl -fsSL -o "$2" "$1"; fi
    echo "$3  $2" | sha256sum -c --quiet - || { echo "checksum mismatch for $2" >&2; rm -f "$2"; exit 1; }
}
tool="$tools/appimagetool-$tool_version"
runtime="$tools/runtime-$arch-$runtime_tag"
fetch "https://github.com/AppImage/appimagetool/releases/download/$tool_version/appimagetool-x86_64.AppImage" "$tool" "$tool_sha"
fetch "https://github.com/AppImage/type2-runtime/releases/download/$runtime_tag/runtime-$arch" "$runtime" "$runtime_sha"
chmod +x "$tool"

# appimagetool is itself an AppImage: extract-and-run so CI needs no FUSE.
ARCH=$arch "$tool" --appimage-extract-and-run --runtime-file "$runtime" --no-appstream "$appdir" "$out"
chmod +x "$out"
echo "built $out"

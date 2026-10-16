#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Linux ARM64 sysroot (design 8, P26) without qemu or a chroot:
# `mmdebstrap --variant=extract` only unpacks the packages. Absolute symlinks
# are then made relative (fix-sysroot-symlinks.py), because a cross compiler
# resolves them against the host, not against the sysroot.
#
#   packaging/linux/make-sysroot.sh <output-dir>
# Needs: mmdebstrap (apt), python3. CI caches the result keyed on a hash of
# linux-arm64-sysroot.txt (see .github/workflows/sysroot-arm64.yml).
set -euo pipefail
out=${1:?usage: make-sysroot.sh <output-dir>}
here=$(cd "$(dirname "$0")" && pwd)
packages=$(grep -v '^[[:space:]]*#' "$here/linux-arm64-sysroot.txt" | grep -v '^[[:space:]]*$' | paste -sd, -)

rm -rf "$out"
mmdebstrap --variant=extract --architectures=arm64 --include="$packages" \
    jammy "$out" "http://ports.ubuntu.com/ubuntu-ports"
python3 "$here/fix-sysroot-symlinks.py" "$out"
echo "sysroot ready: $out"

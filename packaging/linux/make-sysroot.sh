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
#
# mmdebstrap runs as root (through sudo, when we are not root). Its rootless
# "unshare" mode cannot create a target folder inside a CI workspace
# ("E: cannot create ...: Permission denied"); as root it can, and the result
# is then handed back to the invoking user. SYNTHEON_SUDO overrides the
# privilege command (empty = run mmdebstrap directly, e.g. when already root
# or in the test).
set -euo pipefail
out=${1:?usage: make-sysroot.sh <output-dir>}
here=$(cd "$(dirname "$0")" && pwd)
packages=$(grep -v '^[[:space:]]*#' "$here/linux-arm64-sysroot.txt" | grep -v '^[[:space:]]*$' | paste -sd, -)

if [[ -n ${SYNTHEON_SUDO+x} ]]; then
    priv=$SYNTHEON_SUDO
elif [[ $EUID -eq 0 ]]; then
    priv=
else
    priv=sudo
fi

out=$(mkdir -p "$(dirname "$out")" && cd "$(dirname "$out")" && pwd)/$(basename "$out")
# shellcheck disable=SC2086  # $priv is "sudo" or empty on purpose
$priv rm -rf "$out"
# shellcheck disable=SC2086
# Ubuntu 24.04 (noble): main AND universe (libxcb-cursor-dev and libxcb-cursor0 are
# universe-only there), from the release
# pocket plus updates and security, so the sysroot matches a current 24.04. ports.ubuntu.com is the arm64 archive.
mirror=http://ports.ubuntu.com/ubuntu-ports
$priv mmdebstrap --variant=extract --architectures=arm64 --components=main,universe --include="$packages" \
    noble "$out" \
    "deb $mirror noble main universe" \
    "deb $mirror noble-updates main universe" \
    "deb $mirror noble-security main universe"
if [[ -n $priv ]]; then
    $priv chown -R "$(id -u):$(id -g)" "$out"
fi
python3 "$here/fix-sysroot-symlinks.py" "$out"
echo "sysroot ready: $out"

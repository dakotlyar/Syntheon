#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""npu-pack verify: the host-side "is the served pack correct" check.

Re-reads npu-models/manifest.json, validates it the same way the app's parser
does (manifest.cpp), re-hashes every served file against the manifest's
SHA-256, and checks the manifest.sha256 sidecar (the calibration pin, §20 N5).
Run it on Mjolnir — or wherever the tree is staged — before and after
publishing. Exit 0 means the tree is exactly what the manifest claims; any
mismatch, missing file or bad digest is a non-zero exit with the path named.

Usage:
  verify.py [--output-root DIR]
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

from manifest_common import CACHE_ROOT, is_hex64, sha256_of, validate_manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output-root", type=Path, default=CACHE_ROOT / "served")
    args = ap.parse_args()

    root = args.output_root
    manifest_path = root / "npu-models" / "manifest.json"
    if not manifest_path.is_file():
        print(f"verify: {manifest_path} not found", file=sys.stderr)
        return 1

    raw = manifest_path.read_bytes()
    try:
        manifest = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"verify: {manifest_path} isn't valid JSON: {e}", file=sys.stderr)
        return 1

    errors = validate_manifest(manifest)
    if errors:
        print("verify: invalid manifest — " + "; ".join(errors), file=sys.stderr)
        return 1

    version = manifest["pack_version"]
    base = root / "npu-models" / version
    if not base.is_dir():
        print(f"verify: served tree {base} is missing (pack_version {version})", file=sys.stderr)
        return 1

    bad, checked, total = 0, 0, 0
    for mo in manifest["models"]:
        for fo in mo["files"]:
            checked += 1
            p = base / fo["path"]
            if not p.is_file():
                print(f"  MISSING  {fo['path']}", file=sys.stderr)
                bad += 1
                continue
            if p.stat().st_size != fo["size"]:
                print(f"  SIZEDIFF {fo['path']} (served {p.stat().st_size}, manifest {fo['size']})",
                      file=sys.stderr)
                bad += 1
            digest = sha256_of(p)
            if digest != fo["sha256"].lower():
                print(f"  BADHASH  {fo['path']} (served {digest})", file=sys.stderr)
                bad += 1
            else:
                print(f"  ok       {fo['path']}  ({fo['size']} bytes)")
            total += fo["size"]

    sidecar = root / "npu-models" / "manifest.sha256"
    if sidecar.is_file():
        want = sidecar.read_text(encoding="utf-8").strip().lower()
        if not is_hex64(want):
            print(f"  SIDECAR  manifest.sha256 isn't a 64-char hex digest", file=sys.stderr)
            bad += 1
        else:
            got = hashlib.sha256(raw).hexdigest()
            if want != got:
                print(f"  SIDECAR  manifest.sha256 is {want}, manifest.json hashes to {got}",
                      file=sys.stderr)
                bad += 1
            else:
                print(f"  ok       manifest.sha256 matches ({got[:16]}…)")
    else:
        print("  note     no manifest.sha256 sidecar (the calibration pin isn't generated)")

    if bad:
        print(f"verify: {bad} problem(s) across {checked} file(s)", file=sys.stderr)
        return 1
    print(f"verify: ok — {checked} file(s), {total} bytes total ({total / 1e9:.3f} GB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

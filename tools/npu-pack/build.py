#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""npu-pack: build the NPU test's model-pack manifest + served tree (Phase 4c design §6.1, D6).

Takes a per-vendor tree of model files (the OpenVINO IR / ONNX / QNN files we
compiled or pulled) and a config describing each model's id/vendor/workload/
precision, then:

  1. computes each file's size + SHA-256,
  2. copies the files into the served tree so the paths are exactly
       npu-models/<pack_version>/<vendor>/<workload>/<id>/<version>/<file>
     (the app fetches <base_url>/npu-models/manifest.json and
      <base_url>/npu-models/<pack_version>/<path> — downloader.cpp),
  3. emits npu-models/manifest.json (schema 1, pack_version from --pack-version)
     and npu-models/manifest.sha256 (the SHA-256 of manifest.json — the pin
     compiled into source at calibration, design §20 N5).

Idempotent: an unchanged source file is not re-hashed (a state file remembers
its size+mtime+sha256), and a served file whose hash already matches is not
re-copied. Re-running over a ~1.5 GB LLM costs about one read, not two.

The model files themselves live OUTSIDE the repo (huge; P-PATH). Source goes
under ~/.cache/syntheon/npu-pack/src/, the served tree under
~/.cache/syntheon/npu-pack/served/; only this script, manifest_common.py,
config.json and README.md are in-tree.

Usage:
  build.py [--config CONFIG] [--source-root DIR] [--output-root DIR] [--pack-version V] [--partial]

By default the build fails fast if any declared model's files are missing (so a
half-sourced tree never publishes). With `--partial`, a model whose files are
absent is skipped with a warning and the manifest lists only the models that are
fully present — for serving an intermediate pack (e.g. the vision pair while the
LLM is still being produced). A model is only ever published whole: if even one
of its files is missing it is skipped, never half-listed.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

from manifest_common import (SCHEMA, ALLOWED_VENDORS, ALLOWED_WORKLOADS,
                             CACHE_ROOT, is_safe_version, is_safe_relative_path,
                             sha256_of, serialise_manifest, validate_manifest)

DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.json"


def fail(msg, code=2):
    print(f"npu-pack: {msg}", file=sys.stderr)
    sys.exit(code)


def load_config(path):
    try:
        cfg = json.loads(path.read_text())
    except FileNotFoundError:
        fail(f"config {path} not found")
    except json.JSONDecodeError as e:
        fail(f"config {path} isn't valid JSON: {e}")
    models = cfg.get("models")
    if not isinstance(models, list) or not models:
        fail(f"config {path} has no 'models' list")
    out, seen = [], set()
    for i, mo in enumerate(models):
        where = f"config models[{i}]"
        if not isinstance(mo, dict):
            fail(f"{where}: not an object")
        mid = mo.get("id")
        if not isinstance(mid, str) or not mid:
            fail(f"{where}: missing/blank id")
        if mid in seen:
            fail(f"{where}: duplicate id {mid!r}")
        seen.add(mid)
        vendor, workload = mo.get("vendor"), mo.get("workload")
        if vendor not in ALLOWED_VENDORS:
            fail(f"{where} ({mid}): vendor must be one of {ALLOWED_VENDORS}, got {vendor!r}")
        if workload not in ALLOWED_WORKLOADS:
            fail(f"{where} ({mid}): workload must be one of {ALLOWED_WORKLOADS}, got {workload!r}")
        precision = mo.get("precision")
        if not isinstance(precision, str) or not precision:
            fail(f"{where} ({mid}): missing precision")
        version = mo.get("version", "1")
        if not is_safe_version(version):
            fail(f"{where} ({mid}): bad version {version!r}")
        files = mo.get("files")
        if not isinstance(files, list) or not files:
            fail(f"{where} ({mid}): no 'files'")
        for fn in files:
            if not isinstance(fn, str) or not fn or fn in (".", "..") or "/" in fn or "\\" in fn:
                fail(f"{where} ({mid}): bad file name {fn!r} (must be a bare name)")
        out.append({"id": mid, "vendor": vendor, "workload": workload,
                    "precision": precision, "version": version, "files": files})
    return out


def load_state(path):
    if path.exists():
        try:
            return json.loads(path.read_text())
        except (json.JSONDecodeError, ValueError):
            return {}
    return {}


def save_state(path, state):
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def source_digest(src, state):
    """The source's SHA-256, cached by size+mtime so an unchanged big file isn't
    re-hashed. A file that changed keeps a fresh digest in `state` (keyed by the
    absolute source path)."""
    key = str(src)
    st = src.stat()
    entry = state.get(key)
    if entry and entry.get("size") == st.st_size and entry.get("mtime_ns") == st.st_mtime_ns:
        return entry["sha256"]
    digest = sha256_of(src)
    state[key] = {"size": st.st_size, "mtime_ns": st.st_mtime_ns, "sha256": digest}
    return digest


def atomic_copy(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".tmp")
    shutil.copyfile(src, tmp)
    os.replace(tmp, dest)


def build(config_path, source_root, output_root, pack_version, partial=False):
    if not is_safe_version(pack_version):
        fail(f"pack_version {pack_version!r} must be one safe path component (no '/', '\\', '.', '..')")
    models = load_config(config_path)

    def src_path(m, fn):
        return source_root / m["vendor"] / m["workload"] / m["id"] / fn

    # A model is only ever published whole: complete = every file present.
    complete, incomplete = [], []
    for m in models:
        (complete if all(src_path(m, f).is_file() for f in m["files"]) else incomplete).append(m)

    if incomplete:
        missing = [str(src_path(m, f)) for m in incomplete for f in m["files"]
                   if not src_path(m, f).is_file()]
        if not partial:
            # Fail fast before any copy: a half-sourced tree must not leave a
            # partially-laid-out served tree behind.
            fail("missing source file(s):\n  " + "\n  ".join(missing))
        for m in incomplete:
            n = sum(1 for f in m["files"] if not src_path(m, f).is_file())
            print(f"npu-pack: skipping {m['id']} ({m['vendor']}/{m['workload']}): {n} missing file(s)")
    if not complete:
        fail("no model has all its files present — nothing to publish")

    npu_models = output_root / "npu-models"
    # The state file caches source SHA-256s by size+mtime, so it belongs beside
    # the sources — never in the served tree Hera publishes.
    state = load_state(source_root / ".npu-pack-state.json")

    manifest_models, total = [], 0
    for mo in complete:
        files_out = []
        for fn in mo["files"]:
            src = source_root / mo["vendor"] / mo["workload"] / mo["id"] / fn
            if not src.is_file():
                fail(f"{src} is missing (model {mo['id']}) — drop the file there and re-run")
            digest = source_digest(src, state)
            rel = f"{mo['vendor']}/{mo['workload']}/{mo['id']}/{mo['version']}/{fn}"
            if not is_safe_relative_path(rel):
                fail(f"internal error: derived an unsafe path {rel!r}")
            size = src.stat().st_size
            dest = npu_models / pack_version / rel
            # Idempotent: re-hash an existing served file before deciding to
            # re-emit, so a re-run skips an already-correct (possibly huge) file.
            if dest.is_file() and dest.stat().st_size == size and sha256_of(dest) == digest:
                pass
            else:
                atomic_copy(src, dest)
                print(f"  wrote {rel}")
            files_out.append({"path": rel, "size": size, "sha256": digest})
            total += size
        manifest_models.append({"id": mo["id"], "vendor": mo["vendor"],
                                "workload": mo["workload"], "precision": mo["precision"],
                                "files": files_out})

    manifest = {"schema": SCHEMA, "pack_version": pack_version, "models": manifest_models}
    errors = validate_manifest(manifest)
    if errors:
        fail("internal error: built an invalid manifest — " + "; ".join(errors))

    npu_models.mkdir(parents=True, exist_ok=True)
    text = serialise_manifest(manifest)
    manifest_path = npu_models / "manifest.json"
    if not manifest_path.is_file() or manifest_path.read_text() != text:
        manifest_path.write_text(text)

    pin = hashlib.sha256(text.encode()).hexdigest()
    sidecar = npu_models / "manifest.sha256"
    sidecar_text = pin + "\n"
    if not sidecar.is_file() or sidecar.read_text() != sidecar_text:
        sidecar.write_text(sidecar_text)

    save_state(source_root / ".npu-pack-state.json", state)

    print(f"npu-pack: pack_version={pack_version}, {len(manifest_models)} model(s), "
          f"{total} bytes total ({total / 1e9:.3f} GB)")
    if incomplete:
        skipped = ", ".join(m["id"] for m in incomplete)
        print(f"npu-pack: NOTE partial pack — skipped {skipped} (files not present)")
    print(f"npu-pack: manifest  {manifest_path}")
    print(f"npu-pack: sidecar   {sidecar}")
    print(f"npu-pack: manifest.sha256 = {pin}")
    print("served paths (base-URL-relative):")
    for mo in manifest_models:
        for f in mo["files"]:
            print(f"  npu-models/{pack_version}/{f['path']}  ({f['size']} bytes)")
    print("hint: run verify.py --output-root <same dir> to certify the tree")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    ap.add_argument("--source-root", type=Path, default=CACHE_ROOT / "src")
    ap.add_argument("--output-root", type=Path, default=CACHE_ROOT / "served")
    ap.add_argument("--pack-version", default="1")
    ap.add_argument("--partial", action="store_true",
                    help="skip models whose files are missing instead of failing (vision-only pack, etc.)")
    args = ap.parse_args()
    return build(args.config, args.source_root, args.output_root, args.pack_version, args.partial)


if __name__ == "__main__":
    sys.exit(main())

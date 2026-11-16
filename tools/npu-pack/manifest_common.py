# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared helpers for the NPU model-pack tools (Phase 4c design §6.1, D6).

Kept in one module so `build.py` and `verify.py` agree exactly on the schema,
the path/version safety rules and the deterministic serialisation. The rules
here mirror `src/tests/npu/models/manifest.cpp` (`parse()`, plus the
downloader's base-URL layout), so a manifest this module accepts is one the
app's parser accepts, and vice versa.
"""

import hashlib
import json
import os
from pathlib import Path

SCHEMA = 1
SHA256_LEN = 64
MAX_FILE_SIZE = 10**15  # 1 PB: manifest.cpp's kMaxFileSize (stays a whole number in JSON)

ALLOWED_VENDORS = ("intel", "amd", "qualcomm")
ALLOWED_WORKLOADS = ("classify", "detect", "llm")
# precision is free text (the parser only requires non-empty); the design uses these two.
KNOWN_PRECISIONS = ("INT8", "INT4")

# The default working root, like the build trees (P-PATH: never in the repo).
CACHE_ROOT = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "syntheon" / "npu-pack"


def is_safe_version(v):
    """pack_version / model version: a single safe path component (manifest.cpp isSafeVersion)."""
    return isinstance(v, str) and bool(v) and "/" not in v and "\\" not in v and v not in (".", "..")


def is_safe_relative_path(p):
    """A manifest file path must stay inside the pack root (manifest.cpp isSafeRelativePath)."""
    if not isinstance(p, str) or not p:
        return False
    if p.startswith("/") or p.startswith("\\"):
        return False
    if len(p) >= 2 and p[1] == ":":  # "C:…"
        return False
    segs = [s for s in p.replace("\\", "/").split("/") if s]
    return all(s != ".." for s in segs)


def is_hex64(s):
    return isinstance(s, str) and len(s) == SHA256_LEN and all(c in "0123456789abcdefABCDEF" for c in s)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def serialise_manifest(manifest):
    """Deterministic bytes: keys are already in schema order, 2-space indent, one
    trailing newline. The exact bytes are what the manifest.sha256 pin hashes."""
    return json.dumps(manifest, indent=2) + "\n"


def _valid_size(size):
    # manifest.cpp accepts any JSON number that is a positive whole number and
    # ≤ kMaxFileSize (it reads it as a double, then checks floor(bytes)==bytes).
    if isinstance(size, bool):
        return False
    if not isinstance(size, (int, float)):
        return False
    if size <= 0 or size > MAX_FILE_SIZE:
        return False
    return float(size) == float(int(size))


def validate_manifest(manifest):
    """Mirror of manifest.cpp parse(): return a list of error strings (empty = valid)."""
    errors = []
    if not isinstance(manifest, dict):
        return ["the manifest isn't a JSON object"]
    if manifest.get("schema") != SCHEMA:
        errors.append(f"unsupported schema {manifest.get('schema')!r} (expected {SCHEMA})")
    if not is_safe_version(manifest.get("pack_version")):
        errors.append("the manifest has a bad pack_version")
    models = manifest.get("models")
    if not isinstance(models, list) or not models:
        errors.append("the manifest lists no models")
        return errors
    seen_ids = set()
    for i, mo in enumerate(models):
        where = f"model #{i}"
        if not isinstance(mo, dict):
            errors.append(f"{where}: not an object")
            continue
        mid = mo.get("id")
        if not isinstance(mid, str) or not mid:
            errors.append(f"{where}: missing its id")
        elif mid in seen_ids:
            errors.append(f"{where}: duplicate id {mid!r}")
        if isinstance(mid, str):
            seen_ids.add(mid)
        if mo.get("vendor") not in ALLOWED_VENDORS:
            errors.append(f"{where} ({mid}): unknown vendor {mo.get('vendor')!r}")
        if mo.get("workload") not in ALLOWED_WORKLOADS:
            errors.append(f"{where} ({mid}): unknown workload {mo.get('workload')!r}")
        if not isinstance(mo.get("precision"), str) or not mo["precision"]:
            errors.append(f"{where} ({mid}): missing a precision")
        files = mo.get("files")
        if not isinstance(files, list) or not files:
            errors.append(f"{where} ({mid}): lists no files")
            continue
        for fo in files:
            fwhere = f"{where} ({mid})"
            if not isinstance(fo, dict):
                errors.append(f"{fwhere}: a file entry isn't an object")
                continue
            path = fo.get("path")
            if not is_safe_relative_path(path):
                errors.append(f"{fwhere}: unsafe path {path!r}")
            if not _valid_size(fo.get("size")):
                errors.append(f"{fwhere}: bad size for {path!r}")
            if not is_hex64(fo.get("sha256")):
                errors.append(f"{fwhere}: {path!r} has a sha256 that isn't 64 hex chars")
    return errors

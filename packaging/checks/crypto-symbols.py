#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The hardware-crypto check (Phase 2 design §11.2): the built libcrypto must
contain OpenSSL's hardware AES and SHA-256 code. ARM never runs in CI (Phase 1
D13), so this is the only proof that the ARM64 builds have the ARMv8 Crypto
path (the stock Windows ARM64 target would have none, N4). It reads symbols,
never runs the file.

  crypto-symbols.py <libcrypto.a | libcrypto.lib> --arch x86_64|aarch64 [--nm <tool>]
                    [--expect-missing AES|SHA-256|all]

Required, by architecture (any one piece of evidence per line), each defined
and at least the given size:
  x86_64   AES:     aesni_gcm_encrypt or ossl_aes_gcm_encrypt_avx512, >= 256 bytes
           SHA-256: sha256_block_data_order_shaext (>= 256 bytes), or the SHA-NI
                    module itself (sha256_block_data_order >= 256 bytes, defined
                    in the sha256-x86_64 object, which always contains SHA-NI)
  aarch64  AES:     aes_v8_encrypt, >= 32 bytes
           SHA-256: sha256_block_armv8 (>= 64 bytes), or sha256_block_data_order
                    (>= 256 bytes) in the sha256-armv8 object
The size rule rejects the stubs perlasm emits when it judges the assembler too
old for the real code (aesni_gcm_encrypt as "xor eax,eax; ret"). ELF symbol
tables carry sizes; COFF ones (Windows) don't, so there a symbol's size is the
distance to the next global symbol of the same object. In perlasm's stubs every
GCM entry point is followed by the next one, so a stub measures a few bytes; a
symbol with no global after it has an unknown size, which is accepted (and
printed).

--expect-missing: a negative control on a planted library. Exit 0 only if the
check fails with exactly that requirement missing (and the other met), or,
with "all", with both missing (a library that isn't libcrypto).

Exit status: 0 the hardware path is there; 1 it isn't; 2 the file couldn't be
read (nm failed, or listed no defined symbols). A negative control on some
other library must see 1, not 2: 2 means nothing was checked.
"""
import argparse
import re
import subprocess
import sys

DEFINED = set("TtDdRrBbSsVvWw")  # nm types that define a symbol (code or data)
GLOBAL = set("TDRBSVW")

# requirement -> list of (symbol, member regex or None, minimum size in bytes)
REQUIREMENTS = {
    "x86_64": {
        "AES": [("aesni_gcm_encrypt", None, 256), ("ossl_aes_gcm_encrypt_avx512", None, 256)],
        "SHA-256": [("sha256_block_data_order_shaext", None, 256),
                    ("sha256_block_data_order", r"sha256-x86_64\.", 256)],
    },
    "aarch64": {
        "AES": [("aes_v8_encrypt", None, 32)],
        "SHA-256": [("sha256_block_armv8", None, 64),
                    ("sha256_block_data_order", r"sha256-armv8\.", 256)],
    },
}
NAMES = {"x86_64": {"AES": "AES-NI / VAES", "SHA-256": "SHA-NI"},
         "aarch64": {"AES": "ARMv8 Crypto", "SHA-256": "ARMv8 Crypto"}}


class Unreadable(Exception):
    pass


def read_symbols(nm, path):
    """[{member, name, kind, addr, size}] for every defined symbol; size None if unknown."""
    try:
        out = subprocess.run([nm, "-A", "-S", path], capture_output=True, text=True, errors="replace")
    except OSError as e:
        raise Unreadable(f"cannot run {nm}: {e}")
    if out.returncode != 0:
        raise Unreadable(f"{nm} failed on {path}: {out.stderr.strip()}")
    symbols = []
    for line in out.stdout.splitlines():
        # "<archive>:<member>: <addr> [<size>] <type> <name>" (or "<file>: ...").
        m = re.match(r"^(.*?):\s*([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?([A-Za-z?-])\s+(\S+)$", line)
        if not m:
            continue
        where, addr, size, kind, name = m.groups()
        if kind not in DEFINED:
            continue
        member = where.split(":")[-1] if ":" in where else where
        size = int(size, 16) if size else 0
        symbols.append({"member": member, "name": name, "kind": kind, "addr": int(addr, 16), "size": size or None})
    if not symbols:
        raise Unreadable(f"{nm} listed no defined symbols in {path}")
    # COFF: no sizes; measure to the next global symbol in the same object.
    by_member = {}
    for s in symbols:
        by_member.setdefault(s["member"], []).append(s)
    for syms in by_member.values():
        starts = sorted({s["addr"] for s in syms if s["kind"] in GLOBAL})
        for s in syms:
            if s["size"] is None:
                after = [a for a in starts if a > s["addr"]]
                s["size"] = (after[0] - s["addr"]) if after else None
    return symbols


def evaluate(symbols, arch):
    """{requirement: (evidence or None, why candidates were rejected)}."""
    result = {}
    for req, options in REQUIREMENTS[arch].items():
        evidence, rejected = None, []
        for symbol, member_re, minimum in options:
            for s in symbols:
                if s["name"] != symbol or (member_re and not re.search(member_re, s["member"])):
                    continue
                size = s["size"]
                if size is not None and size < minimum:
                    rejected.append(f"{symbol} in {s['member']} is only {size} bytes (a stub?)")
                    continue
                shown = f"{size} bytes" if size is not None else "size unknown (COFF, no later global)"
                evidence = f"{symbol} in {s['member']}, {shown}"
                break
            if evidence:
                break
        result[req] = (evidence, rejected)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("library")
    ap.add_argument("--arch", required=True, choices=sorted(REQUIREMENTS))
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--expect-missing", choices=["AES", "SHA-256", "all"])
    a = ap.parse_args()

    try:
        symbols = read_symbols(a.nm, a.library)
    except Unreadable as e:
        print(f"::error::{e}")
        return 2
    print(f"{a.library}: {len(symbols)} defined symbols ({a.arch})")
    missing = set()
    for req, (evidence, rejected) in evaluate(symbols, a.arch).items():
        label = f"{req} ({NAMES[a.arch][req]})"
        if evidence:
            print(f"  ok      {label}: {evidence}")
        else:
            wanted = ", ".join(s for s, _, _ in REQUIREMENTS[a.arch][req])
            print(f"  MISSING {label}: none of {wanted}" + (f" ({'; '.join(rejected)})" if rejected else ""))
            missing.add(req)
    if a.expect_missing:
        expected = set(REQUIREMENTS[a.arch]) if a.expect_missing == "all" else {a.expect_missing}
        if missing == expected:
            print(f"negative control: the check fails on {a.expect_missing} only, as it must")
            return 0
        print(f"::error::negative control: expected only {a.expect_missing} missing, got {sorted(missing) or 'nothing'}")
        return 1
    if missing:
        print("::error::the hardware crypto path is not in this libcrypto (a no-asm build, the wrong target, or stubs)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The hardware-crypto check (Phase 2 design §11.2): the built libcrypto must
contain OpenSSL's hardware AES and SHA-256 code. ARM never runs in CI (Phase 1
D13), so this is the only proof that the ARM64 builds have the ARMv8 Crypto
path (the stock Windows ARM64 target would have none, N4). It reads symbols,
never runs the file.

  crypto-symbols.py <libcrypto.a | libcrypto.lib> --arch x86_64|aarch64 [--nm <tool>]
                    [--objdump <llvm-objdump>] [--expect-missing AES|SHA-256|all]

Required, by architecture (any one piece of evidence per line), each defined
and of at least the given, measured size:
  x86_64   AES:     aesni_gcm_encrypt >= 256 bytes (the AES-NI GCM module; an
                    assembler that can do the AVX-512 code can do this one, so
                    ossl_aes_gcm_encrypt_avx512 is not accepted instead: its
                    old-assembler fallback is one unsized ud2 label)
           SHA-256: sha256_block_data_order_shaext (>= 256 bytes), or the SHA-NI
                    module itself (sha256_block_data_order >= 256 bytes, defined
                    in the sha256-x86_64 object, which always contains SHA-NI)
  aarch64  AES:     aes_v8_encrypt, >= 32 bytes
           SHA-256: sha256_block_armv8 (>= 64 bytes), or sha256_block_data_order
                    (>= 256 bytes) in the sha256-armv8 object
The size rule rejects the stubs perlasm emits when it judges the assembler too
old ("xor eax,eax; ret", "ud2; ret"). A symbol's size is never assumed:
  - ELF: the symbol table's size; a symbol without one takes the size of
    another name at the same address (perlasm puts several names on one stub);
  - otherwise (COFF has no sizes): the distance to the next function or
    global symbol in the same section, or to the section's end, from
    `llvm-objdump -t` / `-h` (--objdump);
  - a size that can't be measured fails the check.

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
        "AES": [("aesni_gcm_encrypt", None, 256)],
        "SHA-256": [("sha256_block_data_order_shaext", None, 256),
                    ("sha256_block_data_order", r"sha256-x86_64\.", 256)],
    },
    "aarch64": {
        "AES": [("aes_v8_encrypt", None, 32)],
        "SHA-256": [("sha256_block_armv8", None, 64),
                    ("sha256_block_data_order", r"sha256-armv8\.", 256)],
    },
}
NAMES = {"x86_64": {"AES": "AES-NI", "SHA-256": "SHA-NI"},
         "aarch64": {"AES": "ARMv8 Crypto", "SHA-256": "ARMv8 Crypto"}}


class Unreadable(Exception):
    pass


def run(tool, *args):
    try:
        out = subprocess.run([tool, *args], capture_output=True, text=True, errors="replace")
    except OSError as e:
        raise Unreadable(f"cannot run {tool}: {e}")
    if out.returncode != 0:
        raise Unreadable(f"{tool} {' '.join(args[:-1])} failed on {args[-1]}: {out.stderr.strip()}")
    return out.stdout


def member_of(where):
    return where.split(":")[-1] if ":" in where else where


def read_nm(nm, path):
    """Defined symbols from `nm --format=sysv -A`: member, name, kind, addr,
    size (None if the table has none), section (None if not given: COFF)."""
    symbols = []
    for line in run(nm, "--format=sysv", "-A", path).splitlines():
        parts = line.split("|")
        if len(parts) < 7:
            continue
        head = parts[0]
        # "<archive>:<member>:<name>   " (GNU) or "<archive>:<member>: <name>  " (LLVM)
        m = re.match(r"^(.*):\s*(\S+)\s*$", head)
        if not m:
            continue
        where, name = m.groups()
        addr, kind, typ, size, section = (parts[1].strip(), parts[2].strip(), parts[3].strip(),
                                          parts[4].strip(), parts[6].strip())
        if not addr or kind not in DEFINED:
            continue
        symbols.append({"member": member_of(where), "name": name, "kind": kind, "addr": int(addr, 16),
                        "func": typ == "FUNC", "size": int(size, 16) if size and int(size, 16) else None,
                        "section": section or None})
    if not symbols:
        raise Unreadable(f"{nm} listed no defined symbols in {path}")
    return symbols


def read_objdump(objdump, path):
    """COFF sections from `llvm-objdump -t` and `-h`: {(member, name, addr): (section, func, global)}
    and {(member, section): size}."""
    where = None
    syms = {}
    for line in run(objdump, "-t", path).splitlines():
        m = re.match(r"^.*\((.+)\):\s+file format", line) or re.match(r"^(.+):\s+file format", line)
        if m:
            where = member_of(m.group(1))
            continue
        m = re.match(r"^\[\s*\d+\]\(sec\s+(-?\d+)\)\(fl [^)]*\)\(ty\s+([0-9a-fA-F]+)\)\(scl\s+(\d+)\)\s*\(nx \d+\)\s+0x([0-9a-fA-F]+)\s+(\S+)$", line)
        if m and where:
            sec, ty, scl, addr, name = m.groups()
            syms[(where, name, int(addr, 16))] = (int(sec), int(ty, 16) == 0x20, int(scl) == 2)
    sizes = {}
    where = None
    for line in run(objdump, "-h", path).splitlines():
        m = re.match(r"^.*\((.+)\):\s+file format", line) or re.match(r"^(.+):\s+file format", line)
        if m:
            where = member_of(m.group(1))
            continue
        m = re.match(r"^\s*(\d+)\s+\S+\s+([0-9a-fA-F]+)\s", line)
        if m and where:
            sizes[(where, int(m.group(1)) + 1)] = int(m.group(2), 16)  # -t numbers sections from 1
    return syms, sizes


def read_symbols(nm, path, objdump=None):
    symbols = read_nm(nm, path)
    sec_sizes = {}
    if objdump and any(s["section"] is None for s in symbols):
        coff, sec_sizes = read_objdump(objdump, path)
        for s in symbols:
            info = coff.get((s["member"], s["name"], s["addr"]))
            if info:
                s["section"], s["func"], is_global = info
    # Names at one address in one section are one thing: they share a size.
    groups = {}
    for s in symbols:
        groups.setdefault((s["member"], s["section"], s["addr"]), []).append(s)
    for alias in groups.values():
        known = [a["size"] for a in alias if a["size"]]
        for a in alias:
            a["size"] = max(known) if known else None
    # No size in the table: measure to the next function or global symbol in
    # the same section, else to the section's end. No section known: no size.
    for s in symbols:
        if s["size"] is not None or s["section"] is None:
            continue
        later = [t["addr"] for t in symbols
                 if t["member"] == s["member"] and t["section"] == s["section"] and t["addr"] > s["addr"]
                 and (t["func"] or t["kind"] in GLOBAL)]
        if later:
            s["size"] = min(later) - s["addr"]
        elif (s["member"], s["section"]) in sec_sizes:
            s["size"] = sec_sizes[(s["member"], s["section"])] - s["addr"]
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
                if size is None:
                    rejected.append(f"{symbol} in {s['member']}: its size can't be measured")
                    continue
                if size < minimum:
                    rejected.append(f"{symbol} in {s['member']} is only {size} bytes (a stub?)")
                    continue
                evidence = f"{symbol} in {s['member']}, {size} bytes"
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
    ap.add_argument("--objdump", help="llvm-objdump, for COFF (Windows) archives: sections and their sizes")
    ap.add_argument("--expect-missing", choices=["AES", "SHA-256", "all"])
    a = ap.parse_args()

    try:
        symbols = read_symbols(a.nm, a.library, a.objdump)
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

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The hardware-crypto check (Phase 2 design §11.2): the built libcrypto must
contain OpenSSL's hardware AES and SHA-256 code. ARM never runs in CI (Phase 1
D13), so this is the only proof that the ARM64 builds have the ARMv8 Crypto
path (the stock Windows ARM64 target would have none, N4). It reads symbols,
never runs the file.

  crypto-symbols.py <libcrypto.a | libcrypto.lib> --arch x86_64|aarch64 [--nm <tool>] [--self-test]

Required, by architecture (any one piece of evidence per line):
  x86_64   AES:     aesni_gcm_encrypt or ossl_aes_gcm_encrypt_avx512 defined
           SHA-256: sha256_block_data_order_shaext defined, or the SHA-NI
                    module itself (sha256_block_data_order defined in the
                    sha256-x86_64 object, which always contains the SHA-NI code)
  aarch64  AES:     aes_v8_encrypt defined
           SHA-256: sha256_block_armv8 defined, or sha256_block_data_order
                    defined in the sha256-armv8 object (which contains it)
The second SHA-256 form covers assemblers that don't keep local labels in the
symbol table (the SHA extension entry points are local in OpenSSL).

--nm: an nm that reads the file (default: nm; aarch64-linux-gnu-nm for a cross
build; llvm-nm for Windows .lib archives). --self-test: after the check
passes, remove the objects that provide each requirement in turn and require
the check to fail every time (a check that can't fail proves nothing).
Exit status 1 with the findings if anything fails.
"""
import argparse
import re
import subprocess
import sys

DEFINED = set("TtDdRrBbSsVvWw")  # nm types that define a symbol (code or data)

# requirement -> list of (symbol, member regex or None)
REQUIREMENTS = {
    "x86_64": {
        "AES (AES-NI / VAES)": [("aesni_gcm_encrypt", None), ("ossl_aes_gcm_encrypt_avx512", None)],
        "SHA-256 (SHA-NI)": [("sha256_block_data_order_shaext", None),
                             ("sha256_block_data_order", r"sha256-x86_64\.")],
    },
    "aarch64": {
        "AES (ARMv8 Crypto)": [("aes_v8_encrypt", None)],
        "SHA-256 (ARMv8 Crypto)": [("sha256_block_armv8", None),
                                   ("sha256_block_data_order", r"sha256-armv8\.")],
    },
}


def read_symbols(nm, path):
    """[(member, symbol, type)] for every defined symbol."""
    try:
        out = subprocess.run([nm, "-A", path], capture_output=True, text=True, errors="replace")
    except OSError as e:
        sys.exit(f"cannot run {nm}: {e}")
    if out.returncode != 0 and not out.stdout:
        sys.exit(f"{nm} failed on {path}: {out.stderr.strip()}")
    symbols = []
    for line in out.stdout.splitlines():
        # "<archive>:<member>: <addr> <type> <name>" (archive) or "<file>: <addr> <type> <name>".
        m = re.match(r"^(.*?):\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z?-])\s+(\S+)$", line)
        if not m:
            continue
        where, kind, name = m.groups()
        if kind not in DEFINED:
            continue
        member = where.split(":")[-1] if ":" in where else where
        symbols.append((member, name, kind))
    return symbols


def evaluate(symbols, arch, excluded_members=()):
    """{requirement: evidence string or None}."""
    have = [(mem, name) for mem, name, _ in symbols if mem not in excluded_members]
    result = {}
    for req, options in REQUIREMENTS[arch].items():
        evidence = None
        for symbol, member_re in options:
            for mem, name in have:
                if name == symbol and (member_re is None or re.search(member_re, mem)):
                    evidence = f"{symbol} in {mem}"
                    break
            if evidence:
                break
        result[req] = evidence
    return result


def providers(symbols, arch, req):
    """The members whose symbols can satisfy one requirement."""
    found = set()
    for symbol, member_re in REQUIREMENTS[arch][req]:
        for mem, name, _ in symbols:
            if name == symbol and (member_re is None or re.search(member_re, mem)):
                found.add(mem)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("library")
    ap.add_argument("--arch", required=True, choices=sorted(REQUIREMENTS))
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()

    symbols = read_symbols(a.nm, a.library)
    print(f"{a.library}: {len(symbols)} defined symbols ({a.arch})")
    result = evaluate(symbols, a.arch)
    failed = False
    for req, evidence in result.items():
        if evidence:
            print(f"  ok      {req}: {evidence}")
        else:
            print(f"  MISSING {req}: none of {', '.join(s for s, _ in REQUIREMENTS[a.arch][req])}")
            failed = True
    if failed:
        print("::error::the hardware crypto path is not in this libcrypto (a no-asm build, or the wrong target?)")
        return 1
    if a.self_test:
        for req in REQUIREMENTS[a.arch]:
            gone = providers(symbols, a.arch, req)
            if evaluate(symbols, a.arch, gone)[req]:
                print(f"::error::self-test: the check still passes for '{req}' without {sorted(gone)}")
                return 1
            print(f"  self-test: without {sorted(gone)} the check fails for '{req}', as it must")
    return 0


if __name__ == "__main__":
    sys.exit(main())

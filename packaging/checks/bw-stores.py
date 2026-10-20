#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The RAM test's store variants (Phase 3 design §5.3, D4, §14.1
"Bandwidth", §14.2 item 5), read from the x64 bandwidth objects' code:

  sb_bw_<isa>_write_nt, sb_bw_<isa>_copy_nt   use non-temporal stores
                                              (MOVNT*) and an SFENCE
  sb_bw_<isa>_write, sb_bw_<isa>_copy         use no non-temporal store
  every sb_bw_<isa>_* entry point             calls nothing and branches
                                              to nothing outside itself (a
                                              copy loop a compiler turned
                                              into memcpy could get the C
                                              library's NT stores)

so the scored write/copy really bypass the caches and the plain ones really
don't, whatever the compiler did with the intrinsics.

    bw-stores.py --objdump <objdump> <isa>=<object> [...] [--expect-violation]

The disassembler is GNU objdump (ELF) or llvm-objdump (COFF too); it is run
with -dr, so a branch to another symbol shows as a relocation on it. It
reads the objects, never runs them.

Exit 0: every object follows the rules (with --expect-violation: at least one
breaks one, for the planted negative). 1: the other way round. 2: an object
couldn't be read, or lacks one of the entry points.
"""
import argparse
import re
import subprocess
import sys

FUNC = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:\s*$")
INSN = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:(?:[0-9a-fA-F]{2} )+\s*)?([a-z][a-z0-9.]*)")
RELOC = re.compile(r"^\s*[0-9a-fA-F]+:\s+(R_X86_64_\S+|IMAGE_REL_AMD64_\S+)\s+(\S+)")
NT = re.compile(r"^v?movnt")
ENTRIES = ("write", "write_nt", "copy", "copy_nt", "read")


def disassemble(objdump, path):
    out = subprocess.run([objdump, "-dr", "--no-show-raw-insn", path], capture_output=True, text=True,
                         errors="replace")
    if out.returncode != 0:
        raise OSError(f"{objdump} -dr {path}: {out.stderr.strip()}")
    funcs = {}
    current = None
    last = None
    for line in out.stdout.splitlines():
        m = FUNC.match(line)
        if m:
            # COFF names may carry a leading underscore on some tools.
            current = m.group(1).lstrip("_") if m.group(1).startswith("_sb_bw_") else m.group(1)
            funcs.setdefault(current, {"insns": [], "branch_relocs": []})
            last = None
            continue
        if current is None:
            continue
        r = RELOC.match(line)
        if r:
            if last and (last.startswith("call") or last.startswith("j")):
                funcs[current]["branch_relocs"].append(f"{last} {r.group(2)}")
            continue
        i = INSN.match(line)
        if i:
            last = i.group(1)
            funcs[current]["insns"].append(last)
    return funcs


def problems(isa, funcs):
    bad = []
    for e in ENTRIES:
        name = f"sb_bw_{isa}_{e}"
        f = funcs.get(name)
        if f is None:
            raise LookupError(f"{name} is not in the object")
        insns = f["insns"]
        nt = [x for x in insns if NT.match(x)]
        calls = [x for x in insns if x.startswith("call")]
        if e.endswith("_nt"):
            if not nt:
                bad.append(f"{name}: no non-temporal store")
            if "sfence" not in insns:
                bad.append(f"{name}: no sfence")
        elif nt:
            bad.append(f"{name}: non-temporal store {nt[0]} in a plain-store path")
        if calls:
            bad.append(f"{name}: calls out ({calls[0]})")
        for b in f["branch_relocs"]:
            bad.append(f"{name}: branches to another symbol ({b})")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--expect-violation", action="store_true")
    ap.add_argument("objects", nargs="+", metavar="ISA=OBJECT")
    args = ap.parse_args()
    found = False
    for item in args.objects:
        isa, _, path = item.partition("=")
        if not path:
            print(f"bw-stores: '{item}' is not <isa>=<object>", file=sys.stderr)
            return 2
        try:
            bad = problems(isa, disassemble(args.objdump, path))
        except (OSError, LookupError) as e:
            print(f"bw-stores: {path}: {e}", file=sys.stderr)
            return 2
        if bad:
            found = True
            print(f"{path} ({isa}): {len(bad)} problem(s):")
            for b in bad:
                print(f"    {b}")
        else:
            print(f"ok  {path} ({isa})")
    if args.expect_violation:
        if not found:
            print("bw-stores: expected a violation (planted object), found none", file=sys.stderr)
            return 1
        return 0
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The ODR rule for the SIMD kernel's wide-arch objects (Phase 2 design §5.3,
P8, §14.2 item 1).

A TU compiled with wider arch flags than the build's baseline (AVX2, AVX-512,
SVE) must not define any global or weak symbol except its one entry point,
sb_simd_<isa>_unit, and names inside its own namespace sb::cpu::simd_<isa>.
Otherwise an inline or template function (std::min<float>, say) could be
emitted there with wide instructions, and the linker could pick that copy
for a baseline caller: SIGILL on older CPUs.

    simd-odr.py --nm <nm> <isa>=<object> [<isa>=<object> ...] [--expect-violation]

ELF objects (GCC, Clang): `nm -C --defined-only`; a global or weak symbol
(type T D B R W V u i...) passes if it is the entry point or its demangled
name starts with sb::cpu::simd_<isa>:: (also "vtable for ..." and the like).
COFF objects (MSVC; llvm-nm): the same, on the raw MSVC names (an MSVC
COMDAT function shows as T); MSVC's own constant pools (__real@..., __xmm@,
__ymm@, __zmm@, ??_C@ string literals) are named after their content, so
copies are identical and pass.

Exit 0: every object follows the rule (with --expect-violation: at least one
object breaks it, for the planted negative). 1: the other way round. 2: an
object couldn't be read.
"""
import argparse
import re
import subprocess
import sys

ELF_PREFIXES = ("vtable for ", "typeinfo for ", "typeinfo name for ", "VTT for ", "construction vtable for ",
                "guard variable for ", "reference temporary for ")
MSVC_CONSTANTS = re.compile(r"^(__real@|__xmm@|__ymm@|__zmm@|\?\?_C@)")


def is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def symbols(nm, path, demangle):
    cmd = [nm, "--defined-only"] + (["-C"] if demangle else []) + [path]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        raise OSError(f"{' '.join(cmd)}: {out.stderr.strip()}")
    for line in out.stdout.splitlines():
        # "<address> <type> <name>"; the name may contain spaces (demangled).
        m = re.match(r"^\s*([0-9a-fA-F]*)\s+(\S)\s+(.*)$", line)
        if m:
            yield m.group(2), m.group(3)


def exported(kind):
    # Lower-case types are local (t, d, b, r, s, n...), except the GNU unique
    # global "u" and the weak "v"/"w" forms; upper case is global.
    return kind.isupper() or kind in "uvwi"


def violations(nm, isa, path):
    entry = f"sb_simd_{isa}_unit"
    elf = is_elf(path)
    bad = []
    for kind, name in symbols(nm, path, demangle=elf):
        if not exported(kind) or name == entry:
            continue
        if elf:
            n = name
            for p in ELF_PREFIXES:
                if n.startswith(p):
                    n = n[len(p):]
            if n.startswith(f"sb::cpu::simd_{isa}::"):
                continue
        else:
            if MSVC_CONSTANTS.match(name):
                continue
            # ?name@<scopes...>@simd_<isa>@cpu@sb@@...: in our namespace.
            if re.match(rf"^\?+\$?[^@]*@(?:[^@]+@)*?simd_{re.escape(isa)}@cpu@sb@@", name):
                continue
        bad.append(f"{kind} {name}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--expect-violation", action="store_true")
    ap.add_argument("objects", nargs="+", metavar="ISA=OBJECT")
    args = ap.parse_args()
    found = False
    for item in args.objects:
        isa, _, path = item.partition("=")
        if not path:
            print(f"simd-odr: '{item}' is not <isa>=<object>", file=sys.stderr)
            return 2
        try:
            bad = violations(args.nm, isa, path)
        except OSError as e:
            print(f"simd-odr: {e}", file=sys.stderr)
            return 2
        if bad:
            found = True
            print(f"{path} ({isa}): {len(bad)} symbol(s) outside sb::cpu::simd_{isa} other than sb_simd_{isa}_unit:")
            for b in bad:
                print(f"    {b}")
        else:
            print(f"ok  {path} ({isa})")
    if args.expect_violation:
        if not found:
            print("simd-odr: expected a violation (planted object), found none", file=sys.stderr)
            return 1
        return 0
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())

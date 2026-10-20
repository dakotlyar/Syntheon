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

    simd-odr.py --nm <nm> [bw:]<isa>=<object> [...] [--expect-violation]

The RAM test's bandwidth paths (Phase 3 design §5.3, P8) follow the same
rule with "bw:" in front: their namespace is sb::ram::bw_<isa> and their
entry points are sb_bw_<isa>_<op> (read, write, write_nt, copy, copy_nt).

ELF objects (GCC, Clang): `nm -C --defined-only`; a global or weak symbol
(type T D B R W V u i...) passes if it is the entry point or its demangled
name starts with sb::cpu::simd_<isa>:: (also "vtable for ..." and the like).
COFF objects (MSVC; llvm-nm): the same, on the raw MSVC names (an MSVC
COMDAT function shows as T); MSVC's own constant pools (__real@..., __xmm@,
__ymm@, __zmm@, ??_C@ string literals) are named after their content, so
copies are identical and pass.

Dynamic initialisers are refused too (AP-5 L1): a wide TU's static
constructor runs before main(), before any dispatch, on every CPU. ELF:
.init_array / .ctors / .preinit_array sections or _GLOBAL__sub_I_ symbols;
COFF: .CRT$XC* sections or ??__E (MSVC dynamic initialiser) symbols. The
section names are read from the object's own headers.

Exit 0: every object follows the rule (with --expect-violation: at least one
object breaks it, for the planted negative). 1: the other way round. 2: an
object couldn't be read.
"""
import argparse
import re
import struct
import subprocess
import sys

ELF_PREFIXES = ("vtable for ", "typeinfo for ", "typeinfo name for ", "VTT for ", "construction vtable for ",
                "guard variable for ", "reference temporary for ")
MSVC_CONSTANTS = re.compile(r"^(__real@|__xmm@|__ymm@|__zmm@|\?\?_C@)")


def is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def elf_sections(data):
    """Section names of an ELF object (32/64-bit, either byte order)."""
    is64 = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(end + "Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x3A)
    else:
        shoff, = struct.unpack_from(end + "I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x2E)
    def header(i):
        base = shoff + i * shentsize
        name, = struct.unpack_from(end + "I", data, base)
        off, size = (struct.unpack_from(end + "QQ", data, base + 0x18) if is64
                     else struct.unpack_from(end + "II", data, base + 0x10))
        return name, off, size
    _, stroff, strsize = header(shstrndx)
    strtab = data[stroff:stroff + strsize]
    return [strtab[n:strtab.index(b"\0", n)].decode("latin-1") for n, _, _ in (header(i) for i in range(shnum))]


def coff_sections(data):
    """Section names of a COFF object (regular or /bigobj)."""
    sig1, sig2 = struct.unpack_from("<HH", data, 0)
    if sig1 == 0 and sig2 == 0xFFFF:  # bigobj: ANON_OBJECT_HEADER_BIGOBJ
        nsec, symptr, nsym = struct.unpack_from("<III", data, 44)
        first, symsize = 56, 20
    else:
        nsec, = struct.unpack_from("<H", data, 2)
        symptr, nsym = struct.unpack_from("<II", data, 8)
        optsize, = struct.unpack_from("<H", data, 16)
        first, symsize = 20 + optsize, 18
    strtab = symptr + nsym * symsize
    names = []
    for i in range(nsec):
        raw = data[first + 40 * i:first + 40 * i + 8].rstrip(b"\0")
        if raw.startswith(b"/"):  # a long name: an offset into the string table
            at = strtab + int(raw[1:])
            raw = data[at:data.index(b"\0", at)]
        names.append(raw.decode("latin-1"))
    return names


def initialisers(path, elf, all_symbols):
    """What makes the object run code before main()."""
    with open(path, "rb") as f:
        data = f.read()
    found = []
    for name in (elf_sections(data) if elf else coff_sections(data)):
        if (elf and re.match(r"^\.(init_array|ctors|preinit_array)", name)) or (not elf and name.startswith(".CRT$XC")):
            found.append(f"section {name}")
    for name in all_symbols:
        if "_GLOBAL__sub_I_" in name or name.startswith("??__E"):
            found.append(f"symbol {name}")
    return found


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


def violations(nm, family, isa, path):
    if family == "bw":
        entry_ok = re.compile(rf"^sb_bw_{re.escape(isa)}_(read|write|write_nt|copy|copy_nt)$").match
        namespace, scope = f"sb::ram::bw_{isa}::", rf"bw_{re.escape(isa)}@ram@sb@@"
    else:
        entry_ok = re.compile(rf"^sb_simd_{re.escape(isa)}_unit$").match
        namespace, scope = f"sb::cpu::simd_{isa}::", rf"simd_{re.escape(isa)}@cpu@sb@@"
    elf = is_elf(path)
    bad = []
    listed = list(symbols(nm, path, demangle=elf))
    raw = [name for _, name in symbols(nm, path, demangle=False)] if elf else [name for _, name in listed]
    for what in initialisers(path, elf, raw):
        bad.append(f"dynamic initialiser: {what}")
    for kind, name in listed:
        if not exported(kind) or entry_ok(name):
            continue
        if elf:
            n = name
            for p in ELF_PREFIXES:
                if n.startswith(p):
                    n = n[len(p):]
            if n.startswith(namespace):
                continue
        else:
            if MSVC_CONSTANTS.match(name):
                continue
            # ?name@<scopes...>@simd_<isa>@cpu@sb@@...: in our namespace.
            if re.match(rf"^\?+\$?[^@]*@(?:[^@]+@)*?{scope}", name):
                continue
        bad.append(f"{kind} {name}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--expect-violation", action="store_true")
    ap.add_argument("objects", nargs="+", metavar="[bw:]ISA=OBJECT")
    args = ap.parse_args()
    found = False
    for item in args.objects:
        isa, _, path = item.partition("=")
        family, _, rest = isa.partition(":")
        family, isa = (family, rest) if rest else ("simd", isa)
        if not path or family not in ("simd", "bw"):
            print(f"simd-odr: '{item}' is not [bw:]<isa>=<object>", file=sys.stderr)
            return 2
        try:
            bad = violations(args.nm, family, isa, path)
        except OSError as e:
            print(f"simd-odr: {e}", file=sys.stderr)
            return 2
        if bad:
            found = True
            where = f"sb::ram::bw_{isa} other than sb_bw_{isa}_*" if family == "bw" else \
                f"sb::cpu::simd_{isa} other than sb_simd_{isa}_unit"
            print(f"{path} ({isa}): {len(bad)} problem(s): symbols outside {where}, or dynamic initialisers:")
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

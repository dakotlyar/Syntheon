#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A Windows executable's DependentLoadFlags (the /DEPENDENTLOADFLAG linker
option): the DLL search the loader uses for its import table, read from the
PE load-configuration directory (Phase 2 §5.5, P17, AP-5 L3).

    pe-loadflags.py <exe> --expect 0xA00

0xA00 = LOAD_LIBRARY_SEARCH_SYSTEM32 (0x800) | LOAD_LIBRARY_SEARCH_APPLICATION_DIR
(0x200): the elevated encryption worker loads its DLLs from System32 and its
own folder only, never from the current directory or PATH.
Exit 0: the flags are as expected; 1: they aren't (or there is no load
configuration); 2: not a PE32+ file.
"""
import argparse
import struct
import sys


def dependent_load_flags(data):
    if data[:2] != b"MZ":
        raise ValueError("not an MZ/PE file")
    pe, = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    coff = pe + 4
    nsec, = struct.unpack_from("<H", data, coff + 2)
    optsize, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    if magic != 0x20B:
        raise ValueError("not PE32+ (x64 / ARM64)")
    ndirs, = struct.unpack_from("<I", data, opt + 108)
    if ndirs <= 10:
        return None
    rva, size = struct.unpack_from("<II", data, opt + 112 + 8 * 10)  # IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG
    if not rva:
        return None
    for i in range(nsec):
        sec = opt + optsize + 40 * i
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            at = rawptr + (rva - vaddr)
            declared, = struct.unpack_from("<I", data, at)
            # IMAGE_LOAD_CONFIG_DIRECTORY64: DependentLoadFlags is the WORD at 78.
            if declared < 80:
                return 0
            flags, = struct.unpack_from("<H", data, at + 78)
            return flags
    raise ValueError("the load configuration isn't in any section")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--expect", required=True, type=lambda v: int(v, 0))
    a = ap.parse_args()
    try:
        with open(a.exe, "rb") as f:
            flags = dependent_load_flags(f.read())
    except (OSError, ValueError, struct.error) as e:
        print(f"pe-loadflags: {a.exe}: {e}", file=sys.stderr)
        return 2
    if flags is None:
        print(f"FAIL {a.exe}: no load configuration directory")
        return 1
    if flags != a.expect:
        print(f"FAIL {a.exe}: DependentLoadFlags 0x{flags:X}, expected 0x{a.expect:X}")
        return 1
    print(f"ok   {a.exe}: DependentLoadFlags 0x{flags:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

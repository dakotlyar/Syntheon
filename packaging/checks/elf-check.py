#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks for a Linux AppDir (design P27); runs on any host, never
executes the files it inspects, so it also works for the ARM64 AppImage that
no machine here can run.

  elf-check.py <AppDir> --arch x86_64|aarch64 [--cross-prefix aarch64-linux-gnu-]

For every ELF file under the tree:
  1. its machine matches --arch (no stray other-architecture binary)
  2. every NEEDED library is in the tree or on the system allowlist
     (packaging/linux/excludelist.txt)
  3. the newest GLIBC_ symbol it needs is <= the architecture's floor, and the
     newest GLIBCXX_/CXXABI_ symbols are within that distro's libstdc++:
       x86_64   glibc 2.35, GLIBCXX_3.4.30, CXXABI_1.3.13  (Ubuntu 22.04, D11)
       aarch64  glibc 2.39, GLIBCXX_3.4.33, CXXABI_1.3.15  (Ubuntu 24.04; the
                official Qt arm64 build needs 2.38 / 3.4.32, Dave 2026-10-16)
     (--glibc/--glibcxx/--cxxabi override)
A newer symbol means the AppImage would not start on the baseline distros.
Exit status 1 and a list of findings if anything fails.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

MACHINES = {"x86_64": "Advanced Micro Devices X86-64", "aarch64": "AArch64"}
# The floor per architecture (glibc, GLIBCXX, CXXABI): the oldest distro each is supported on.
# aarch64 (noble): read from the real libstdc++.so.6 of both libstdc++6 14-20240412-0ubuntu1
# (24.04.0) and 14.2.0-4ubuntu2~24.04.1 (updates) with objdump -T: both export up to
# GLIBCXX_3.4.33 and CXXABI_1.3.15. x86_64 (jammy): libstdc++6 12.x, GLIBCXX_3.4.30, CXXABI_1.3.13.
FLOORS = {"x86_64": ("2.35", "3.4.30", "1.3.13"), "aarch64": ("2.39", "3.4.33", "1.3.15")}


def ver(s):
    return tuple(int(x) for x in s.split("."))


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--arch", required=True, choices=MACHINES)
    ap.add_argument("--cross-prefix", default="")
    ap.add_argument("--glibc", help="default: the architecture's floor")
    ap.add_argument("--glibcxx")
    ap.add_argument("--cxxabi")
    ap.add_argument("--print-limits", action="store_true", help="print the limits that would be used and exit")
    ap.add_argument("--allowlist", default=str(Path(__file__).resolve().parent.parent / "linux" / "excludelist.txt"))
    a = ap.parse_args()

    floor = FLOORS[a.arch]
    a.glibc, a.glibcxx, a.cxxabi = a.glibc or floor[0], a.glibcxx or floor[1], a.cxxabi or floor[2]
    if a.print_limits:
        print(f"{a.arch}: glibc {a.glibc}, GLIBCXX {a.glibcxx}, CXXABI {a.cxxabi}")
        return 0
    tree = Path(a.tree)
    allow = [l.strip() for l in Path(a.allowlist).read_text().splitlines() if l.strip() and not l.startswith("#")]
    readelf, objdump = a.cross_prefix + "readelf", a.cross_prefix + "objdump"
    elfs = []
    for p in sorted(tree.rglob("*")):
        if p.is_file() and not p.is_symlink():
            with open(p, "rb") as f:
                if f.read(4) == b"\x7fELF":
                    elfs.append(p)
    if not elfs:
        print(f"no ELF files under {tree}", file=sys.stderr)
        return 1
    bundled = {p.name for p in elfs}
    problems = []
    limits = {"GLIBC": ver(a.glibc), "GLIBCXX": ver(a.glibcxx), "CXXABI": ver(a.cxxabi)}
    worst = {k: (0,) for k in limits}
    for p in elfs:
        rel = p.relative_to(tree)
        head = run([readelf, "-h", str(p)])
        m = re.search(r"Machine:\s+(.+)", head)
        if not m or m.group(1).strip() != MACHINES[a.arch]:
            problems.append(f"{rel}: machine is '{m.group(1).strip() if m else '?'}', expected {a.arch}")
            continue
        dyn = run([readelf, "-d", str(p)])
        for soname in re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", dyn):
            # The GPU worker opens the Vulkan loader (volk) and the OpenGL driver (glad) at run
            # time, so a machine without one still starts it (Phase 4a task 16). The GLVND
            # dispatch libraries Qt itself links (libGLX, libOpenGL) are fine.
            if p.name == "syntheon-gpu" and re.match(r"lib(vulkan|GL|EGL)\.so", soname):
                problems.append(f"{rel}: links {soname}; the worker must load it at run time")
            if soname in bundled or any(soname.startswith(x) for x in allow):
                continue
            problems.append(f"{rel}: needs {soname}, which is neither bundled nor a system library")
        syms = run([objdump, "-T", str(p)])
        for kind, text in re.findall(r"\b(GLIBCXX|CXXABI|GLIBC)_([0-9][0-9.]*)", syms):
            v = ver(text.rstrip("."))
            worst[kind] = max(worst[kind], v)
            if v > limits[kind]:
                problems.append(f"{rel}: needs {kind}_{text}, newer than the baseline {kind}_{'.'.join(map(str, limits[kind]))}")
    for kind, v in worst.items():
        print(f"highest {kind}: {'.'.join(map(str, v))} (limit {'.'.join(map(str, limits[kind]))})")
    if problems:
        for line in sorted(set(problems)):
            print("FAIL:", line, file=sys.stderr)
        return 1
    print(f"OK: {len(elfs)} ELF files, all {a.arch}, all libraries accounted for")
    return 0


if __name__ == "__main__":
    sys.exit(main())

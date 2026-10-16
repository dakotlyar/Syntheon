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
  3. the newest GLIBC_ symbol it needs is <= --glibc (2.35, D11), and the
     newest GLIBCXX_/CXXABI_ symbols are within Ubuntu 22.04's libstdc++
A newer symbol means the AppImage would not start on the baseline distros.
Exit status 1 and a list of findings if anything fails.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

MACHINES = {"x86_64": "Advanced Micro Devices X86-64", "aarch64": "AArch64"}


def ver(s):
    return tuple(int(x) for x in s.split("."))


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--arch", required=True, choices=MACHINES)
    ap.add_argument("--cross-prefix", default="")
    ap.add_argument("--glibc", default="2.35")
    ap.add_argument("--glibcxx", default="3.4.30", help="jammy's libstdc++6 (GCC 12)")
    ap.add_argument("--cxxabi", default="1.3.13")
    ap.add_argument("--allowlist", default=str(Path(__file__).resolve().parent.parent / "linux" / "excludelist.txt"))
    a = ap.parse_args()

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

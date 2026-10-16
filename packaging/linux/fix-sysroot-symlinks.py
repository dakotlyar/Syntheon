#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rewrites absolute symlinks inside a sysroot into relative ones.

A symlink like usr/lib/aarch64-linux-gnu/libfoo.so -> /lib/aarch64-linux-gnu/libfoo.so.1
points at the HOST's /lib when a cross linker follows it. Made relative to
the link's own folder it stays inside the sysroot.

  fix-sysroot-symlinks.py <sysroot>
Prints what it changed; links whose target is missing from the sysroot are reported (not fatal).
"""
import os
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = os.path.abspath(sys.argv[1])
    fixed = dangling = 0
    for folder, dirs, files in os.walk(root):
        for name in dirs + files:
            path = os.path.join(folder, name)
            if not os.path.islink(path):
                continue
            target = os.readlink(path)
            if not os.path.isabs(target):
                continue
            inside = os.path.join(root, target.lstrip("/"))
            if not os.path.lexists(inside):
                dangling += 1
                print(f"dangling after rewrite: {os.path.relpath(path, root)} -> {target}", file=sys.stderr)
            rel = os.path.relpath(inside, folder)
            os.unlink(path)
            os.symlink(rel, path)
            fixed += 1
    print(f"{fixed} absolute symlinks made relative, {dangling} dangling")


if __name__ == "__main__":
    main()

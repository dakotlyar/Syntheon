#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A-W: a local Windows syntax check, run before a checkpoint (see README.md).

mingw g++ -fsyntax-only on every tracked translation unit whose Windows
preprocessing reaches <windows.h> (directly or through a header), plus the GPU
files listed in FORCE, WITHOUT a global -DNOMINMAX, so a header that leaks the
min/max macros is caught (the checkpoint 6 B1 class). It uses the Linux Qt
headers and some stubs, so it is not a Windows Qt build and not a replacement
for the windows-x64 CI job. It never modifies the repo.

Usage:
  aw.py [--build-dir DIR] [FILE ...]

  --build-dir DIR  a configured Linux build tree whose compile_commands.json
                   gives the include paths and defines (default:
                   ~/.cache/synbench/build/local-linux-x64-debug)
  FILE ...         repo-relative sources to check (always checked, whether or
                   not they reach <windows.h>); none: every one that does,
                   plus FORCE

Needs x86_64-w64-mingw32-g++ (mingw-w64-gcc). Exit 0 if all are clean, 1 if
any fails.
"""
import argparse
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
CACHE = pathlib.Path(os.environ.get("XDG_CACHE_HOME", pathlib.Path.home() / ".cache")) / "synbench" / "windows-syntax-check"
STUB = CACHE / "stubs"  # generated on every run
GXX = "x86_64-w64-mingw32-g++"
# Checked even though they don't reach <windows.h> (B1: gpu_test.cpp once did, through hags.h).
FORCE = {"src/tests/gpu/gpu_test.cpp", "src/tests/gpu/gpu_detail.cpp", "tests/unit/tst_gpu_test.cpp"}
BUILD = pathlib.Path.home() / ".cache/synbench/build/local-linux-x64-debug"

def make_stubs():
    (STUB / "QtGui").mkdir(parents=True, exist_ok=True)
    # Qt's qwindowdefs.h includes this on Q_OS_WIN; the Linux install doesn't ship it.
    (STUB / "QtGui/qwindowdefs_win.h").write_text(
        "#pragma once\n"
        "struct HWND__; typedef HWND__ *HWND; struct HDC__; typedef HDC__ *HDC; struct HINSTANCE__; typedef HINSTANCE__ *HINSTANCE;\n"
        "struct HMONITOR__; typedef HMONITOR__ *HMONITOR; struct HGLRC__; typedef HGLRC__ *HGLRC; typedef HINSTANCE HMODULE;\n"
        "struct HRGN__; typedef HRGN__ *HRGN; struct HBITMAP__; typedef HBITMAP__ *HBITMAP; struct HICON__; typedef HICON__ *HICON;\n"
        "struct HFONT__; typedef HFONT__ *HFONT; struct HBRUSH__; typedef HBRUSH__ *HBRUSH; struct HMENU__; typedef HMENU__ *HMENU;\n"
        "typedef HICON HCURSOR;\n")
    # MSVC's minwindef.h defines min/max as macros in C++ too unless NOMINMAX is set; mingw's only in C.
    # This wrapper restores MSVC's behaviour, so a <windows.h> without NOMINMAX breaks std::max as it does in CI (B1).
    (STUB / "windows.h").write_text(
        "#include_next <windows.h>\n"
        "#if defined(__cplusplus) && !defined(NOMINMAX)\n"
        "#ifndef max\n#define max(a, b) (((a) > (b)) ? (a) : (b))\n#endif\n"
        "#ifndef min\n#define min(a, b) (((a) < (b)) ? (a) : (b))\n#endif\n#endif\n")
    # ...and libstdc++'s bits/os_defines.h defines NOMINMAX for everyone on mingw (MSVC's STL doesn't): undo that, so only
    # a NOMINMAX the project sets counts.
    (STUB / "bits").mkdir(exist_ok=True)
    (STUB / "bits/os_defines.h").write_text(
        "#ifdef NOMINMAX\n#define SB_AW_PROJECT_NOMINMAX\n#endif\n"
        "#include_next <bits/os_defines.h>\n#undef NOMINMAX\n"
        "#ifdef SB_AW_PROJECT_NOMINMAX\n#define NOMINMAX\n#endif\n")
    shaders = ["present", "depth_only", "drawcalls", "geometry", "ground", "particles", "post_bright", "post_composite",
               "post_down", "post_fxaa", "post_tonemap", "post_up", "quads", "sprites", "shading", "sky"]
    for s in shaders:
        camel = "".join(p.capitalize() for p in s.split("_"))
        for stage in ("vs", "ps"):
            for kind in ("dxil", "dxbc"):
                name = f"k{camel}{stage.capitalize()}{kind.capitalize()}"
                (STUB / f"{s}_{stage}.{kind}.inc").write_text(
                    f"static const unsigned char {name}[] = {{0}};\nstatic const std::size_t {name}Size = 1;\n")

def moc_stubs(sources):
    for src in sources:
        for m in re.findall(r'#include "([A-Za-z0-9_]+\.moc)"', src.read_text(errors="ignore")):
            (STUB / m).write_text("")

def tracked_sources():
    out = subprocess.run(["git", "ls-files", "*.cpp", "*.c"], cwd=REPO, capture_output=True, text=True, check=True).stdout.split()
    return [REPO / f for f in out if "/glad/" not in f]

def linux_flags():
    cc = json.load(open(BUILD / "compile_commands.json"))
    by_file = {}
    for e in cc:
        args = shlex.split(e["command"])
        keep, i = [], 1
        while i < len(args):
            a = args[i]
            if a in ("-o", "-c"):
                i += 2
                continue
            if a.startswith(("-D", "-I", "-std=", "-W")) and a != "-Wpedantic":
                keep.append(a)
            elif a == "-isystem":
                keep += [a, args[i + 1]]
                i += 1
            i += 1
        by_file[pathlib.Path(e["file"]).resolve()] = keep
    return by_file

def flags_for(src, by_file):
    if src in by_file:
        return by_file[src]
    # A Windows-only source (not in the Linux build): a sibling's flags, nearest directory first.
    d = src.parent
    while d != REPO.parent:
        for f, fl in by_file.items():
            if f.parent == d and f.suffix == src.suffix:
                return fl
        d = d.parent
    return ["-I" + str(REPO / "src"), "-std=c++20"]

def win_flags(fl, hooks):
    out = ["-I", str(STUB)]
    for a in fl:
        if a == "-DSYNBENCH_DEV_HOOKS=1" and not hooks:
            continue
        out.append(a.replace("mkspecs/linux-g++", "mkspecs/win32-g++"))
    if hooks and "-DSYNBENCH_DEV_HOOKS=1" not in out:
        out.append("-DSYNBENCH_DEV_HOOKS=1")
    vk = BUILD / "_deps/vulkanheaders-src/include"
    out += ["-I", str(REPO / "src/tests/gpu/worker"),
            "-I", str(vk), "-isystem", str(BUILD / "_deps/synbench_volk-src"),
            "-isystem", str(REPO / "src/tests/gpu/worker/gl/glad/include"),
            '-DSYNBENCH_GPU_DXC_VERSION="1.8.2505"', '-DSYNBENCH_GPU_FXC_VERSION="10.1"',
            '-DSYNBENCH_GPU_SHADER_STAMP="c957"']
    return out

def reaches_windows(src, fl):
    r = subprocess.run([GXX, "-M", "-MG", "-w", *fl, str(src)], capture_output=True, text=True)
    deps = r.stdout.lower()
    return re.search(r"[/\\]windows\.h\b", deps) is not None, r

def main():
    global BUILD
    ap = argparse.ArgumentParser(description="mingw -fsyntax-only on the sources that reach <windows.h>")
    ap.add_argument("--build-dir", type=pathlib.Path, default=BUILD)
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()
    BUILD = args.build_dir.expanduser().resolve()
    if not (BUILD / "compile_commands.json").is_file():
        print(f"no compile_commands.json in {BUILD}: configure and build a Linux preset first (--build-dir)", file=sys.stderr)
        return 2
    if shutil.which(GXX) is None:
        print(f"{GXX} not found: install mingw-w64-gcc", file=sys.stderr)
        return 2
    make_stubs()
    by_file = linux_flags()
    sources = tracked_sources()
    moc_stubs(sources)
    only = set(args.files)
    checked, failed = [], []
    for src in sources:
        rel = src.relative_to(REPO).as_posix()
        if only and rel not in only:
            continue
        lang = ["-x", "c++"] if src.suffix == ".cpp" else ["-x", "c"]
        base = flags_for(src.resolve(), by_file)
        fl = win_flags(base, True)
        if src.suffix == ".c":
            fl = [a for a in fl if not a.startswith("-std=c++")]
        hit, _ = reaches_windows(src, lang + fl)
        if not hit and rel not in FORCE and rel not in only:
            continue
        pre = ["-include", "windows.h"] if rel.endswith("qt_glue.cpp") else []  # Qt's GL platform header needs HMODULE
        for hooks in ((True,) if rel.startswith("tests/") else (True, False)):  # the tests are built with the dev hooks only
            f2 = win_flags(base, hooks)
            if src.suffix == ".c":
                f2 = [a for a in f2 if not a.startswith("-std=c++")]
            cmd = [GXX, *lang, "-fsyntax-only", "-Wall", "-Wextra", *pre, *f2, str(src)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            tag = f"{rel} ({'hooks' if hooks else 'no hooks'})"
            if r.returncode != 0:
                failed.append((tag, r.stderr))
            else:
                checked.append((tag, len([l for l in r.stderr.splitlines() if 'warning:' in l])))
    for tag, warns in checked:
        print(f"OK   {tag}" + (f"  [{warns} warnings]" if warns else ""))
    for tag, err in failed:
        print(f"FAIL {tag}\n" + "\n".join(err.splitlines()[:25]))
    print(f"\n{len(checked)} clean, {len(failed)} failed")
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())

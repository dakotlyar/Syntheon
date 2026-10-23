# windows-syntax-check (A-W)

A local pre-checkpoint tool, not a CI job. It runs mingw's `g++ -fsyntax-only`
over every tracked translation unit whose Windows preprocessing reaches
`<windows.h>` (directly or through a header), plus the GPU files that must be
checked anyway (`FORCE` in `aw.py`), so a Windows-only compile error shows up
on Linux before the windows-x64 job finds it.

It catches, for example, a header that includes `<windows.h>` without
`NOMINMAX` and so breaks `std::max` (the checkpoint 6 B1 class): stub
`windows.h` and `bits/os_defines.h` wrappers reproduce MSVC's behaviour, where
`min`/`max` are macros unless the project defines `NOMINMAX`.

It is **not** a Windows build. It uses the Linux Qt headers, mingw instead of
MSVC, none of the Windows-only CMake definitions, and stubbed shader includes,
and it does not catch a translation unit that reaches `minwindef.h` without
`windows.h`. A clean run means "no mingw syntax error", not "windows-x64 will
pass".

## Use

Needs `mingw-w64-gcc` (`x86_64-w64-mingw32-g++`) and a configured Linux build
tree, whose `compile_commands.json` supplies the include paths and defines:

```
python3 tools/windows-syntax-check/aw.py                       # every source that reaches <windows.h>, plus FORCE
python3 tools/windows-syntax-check/aw.py src/tests/gpu/model/hags.cpp  # only these (always checked)
python3 tools/windows-syntax-check/aw.py --build-dir ~/.cache/synbench/build/local-linux-x64-debug
```

The default build tree is `~/.cache/synbench/build/local-linux-x64-debug`. The
stubs are generated into `$XDG_CACHE_HOME/synbench/windows-syntax-check/`
(default `~/.cache/...`) on every run; nothing is written to the repo. Sources
under `src/` are checked with and without `SYNBENCH_DEV_HOOKS`, the tests
with it only (how they are built). The exit code is 1 if any file fails, 2 for
a missing compiler or build tree.

When a file is added that must be checked although it never reaches
`<windows.h>` (as `gpu_test.cpp` once did through `hags.h`), add it to `FORCE`.

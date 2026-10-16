# SynBench

SynBench is a synthetic system benchmark for Windows and Linux (x64 and ARM64):
CPU, memory, storage, graphics, AI inference, NPU presence and battery life.
Every run is saved on your computer, and runs can be compared side by side.

This is **v2**, a native C++/Qt rewrite of the Python SynBench. Phase 1 is the
application shell: the screens, run orchestration, the history database,
system information, the privileged-access model and packaging. The seven
tests are **stubs that produce plausible, seeded numbers**; the real tests
arrive in later phases (see `ideas/` in the staff repository for the plan).
The last Python release is `v0.2.3`, kept in this repository's history.

## Running it

Download an installer or the AppImage from a release:

| Platform | File |
|---|---|
| Windows x64 | `SynBench-<version>-windows-x64-setup.exe` or `SynBench-<version>-windows-x64.msi` |
| Windows ARM64 | `SynBench-<version>-windows-arm64-setup.exe` or `...-arm64.msi` |
| Linux x64 | `SynBench-<version>-x86_64.AppImage` (needs glibc 2.35+: Ubuntu 22.04, Debian 12, Fedora 36 or newer) |

SynBench asks for administrator access **once, when it starts**, to read
firmware details such as the memory modules. On Windows that is the UAC prompt
(a declined prompt means the app does not open). On Linux it is a polkit
(`pkexec`) prompt. Without administrator access the app does not run; there is
no limited mode.

Only one SynBench can be open on a history. Starting it a second time brings
the open window forward.

Your runs are kept in a SQLite database (Settings shows where, and can move
it): `%LOCALAPPDATA%\SynBench` on Windows, `~/.local/share/SynBench` on Linux.

## Building it

See [BUILDING.md](BUILDING.md). To add a test, see
[docs/HOW-TO-ADD-A-TEST.md](docs/HOW-TO-ADD-A-TEST.md).

## Licence

GPL-3.0-or-later (see `LICENSE`).

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

## Security model

SynBench asks for administrator access so it can read firmware tables (the
memory modules) and, in later phases, a few other things only root can do. This
is what that means in practice.

- **Windows:** the whole app runs elevated (its manifest asks for administrator
  rights, so UAC prompts at every start). A declined prompt means the app does
  not open. The installers are not code-signed yet, so Windows shows "Unknown
  publisher".
- **Linux:** the app runs as **you**, never as root, and refuses to start as
  root. Only a small separate program, `synbench-helper`, runs as root, started
  through `pkexec` (polkit). It has no Qt, no network and no files of its own,
  and it accepts exactly **four fixed requests**: `Hello`, `Ping`,
  `ReadSmbios` (read the firmware's DMI tables and send back the raw bytes) and
  `Shutdown`. It is never given a path or a command, and the parsing of what it
  returns happens in the unprivileged app, not in root.
- **The prompt** is polkit's generic one: "Authentication is needed to run
  `<path>` as the super user", where `<path>` is the AppImage file (or, in a
  development build, the `synbench-helper` next to the app). That is the only
  thing it tells you; there is no SynBench-specific wording.
- **The AppImage is a user-writable file.** To get a root helper out of it,
  root re-executes the AppImage file itself. Anything that can already write
  that file as your user (malware running as you, a swapped download) could make
  the next prompt run something else as root. This is the same trust model as
  most AppImages that elevate: it protects against SynBench's own mistakes and
  limits what the helper can do, but it does **not** defend against another
  program already running as you. SynBench checks that the file it re-executes is
  really the mounted image it was started from (against the kernel's mount
  table, not just environment variables), but a user can mount their own FUSE
  filesystem, so this narrows the risk rather than removing it. If that is
  not acceptable for you, do not elevate: SynBench has no limited mode, so it
  will not run.
- Only one SynBench runs per history folder; a second launch brings the first
  window forward. Its lock and socket live in your own runtime directory, not
  in the (possibly synced) history folder.

## Building it

See [BUILDING.md](BUILDING.md). To add a test, see
[docs/HOW-TO-ADD-A-TEST.md](docs/HOW-TO-ADD-A-TEST.md).

## Licence

GPL-3.0-or-later (see `LICENSE`).

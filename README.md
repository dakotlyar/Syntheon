# Syntheon

Formerly SynBench.

Syntheon is a synthetic system benchmark for Windows and Linux (x64 and ARM64):
CPU, memory, storage, graphics, AI inference, NPU presence and battery life.
Every run is saved on your computer, and runs can be compared side by side.

This is **v2**, a native C++/Qt rewrite of the Python SynBench. Phase 1 is the
application shell: the screens, run orchestration, the history database,
system information, the privileged-access model and packaging. Phase 2 makes
the **CPU test real**: seven groups on all threads (integer, floating point +
primes, SIMD, compression, encryption, sorting, physics) plus a single-thread
mark, about 4½ minutes, with clock and temperature telemetry. It is
calibrated so that the reference machine (Knowhere, a Ryzen AI Max+ 392)
scores 1,000. The other six tests are still **stubs
that produce plausible, seeded numbers**; the real ones arrive in later
phases (see `ideas/` in the staff repository for the plan).
The last Python release is `v0.2.3`, kept in this repository's history.

## Running it

Download an installer or the AppImage from a release:

| Platform | File |
|---|---|
| Windows x64 | `Syntheon-<version>-windows-x64-setup.exe` or `Syntheon-<version>-windows-x64.msi` |
| Windows ARM64 | `Syntheon-<version>-windows-arm64-setup.exe` or `...-arm64.msi`. Cross-compiled and only **statically checked** in CI (no ARM code runs there): it needs a hand test on a Snapdragon PC before it can be trusted |
| Linux x64 | `Syntheon-<version>-x86_64.AppImage` (needs glibc 2.35+: Ubuntu 22.04, Debian 12, Fedora 36 or newer) |
| Linux ARM64 | not released yet: built in CI as an untested artifact. It needs **glibc 2.39 (Ubuntu 24.04 or newer)**, because the official Qt for ARM64 Linux requires it |

Syntheon asks for administrator access **once, when it starts**, to read
firmware details such as the memory modules. On Windows that is the UAC prompt
(a declined prompt means the app does not open). On Linux it is a polkit
(`pkexec`) prompt. Without administrator access the app does not run; there is
no limited mode.

**Installing on Windows.** There are two installers per architecture: the setup
program and the MSI. They are not interchangeable: if one is installed, the other
refuses (uninstall the first). The setup program **upgrades** the old Python
SynBench (v0.2.3) in place; the **MSI refuses** when the old SynBench is
installed ("Uninstall it first, or use the setup program, which upgrades it"). The
x64 and ARM64 builds exclude each other the same way (the ARM64 setup replaces an
x64 install; the x64 setup refuses over ARM64). Both install only into Program
Files; there is no folder choice. Uninstalling leaves your history (and the
small `run` folder under `%LOCALAPPDATA%\Syntheon`) in place.

Only one Syntheon can be open on a history. Starting it a second time brings
the open window forward.

Your runs are kept in a SQLite database (Settings shows where, and can move
it): `%LOCALAPPDATA%\Syntheon` on Windows, `~/.local/share/Syntheon` on Linux.
Your settings are in `settings.ini`, under `%APPDATA%\Syntheon` on Windows and
`~/.config/Syntheon` on Linux.

If you used SynBench, the first launch copies your saved runs and settings
across and tells you how many runs came over. Close SynBench first, because
Syntheon won't copy a history that is still open. A history you kept in a folder
of your own stays in that folder, as `syntheon.db` beside the old `synbench.db`.

The old files are left exactly as they were, and Syntheon doesn't look at them
again, so anything you do in the old app after that won't show up here. Once
you're happy that everything arrived, the old folders are yours to delete:
`%LOCALAPPDATA%\SynBench` and `%APPDATA%\SynBench` on Windows,
`~/.local/share/SynBench` and `~/.config/SynBench` on Linux.

## Security model

Syntheon asks for administrator access so it can read firmware tables (the
memory modules) and, in later phases, a few other things only root can do. This
is what that means in practice.

- **Not signed:** neither the Windows installers nor the AppImage are code-signed
  or GPG-signed yet; check the download against the checksum on its release page.
- **Windows:** the whole app runs elevated (its manifest asks for administrator
  rights, so UAC prompts at every start). A declined prompt means the app does
  not open. The installers are not code-signed yet, so Windows shows "Unknown
  publisher".
- **Linux:** the app runs as **you**, never as root, and refuses to start as
  root. Only a small separate program, `syntheon-helper`, runs as root, started
  through `pkexec` (polkit). It has no Qt, no network and no files of its own,
  and it accepts exactly **four fixed requests**: `Hello`, `Ping`,
  `ReadSmbios` (read the firmware's DMI tables and send back the raw bytes) and
  `Shutdown`. It is never given a path or a command, and the parsing of what it
  returns happens in the unprivileged app, not in root.
- **The prompt** is polkit's generic one: "Authentication is needed to run
  `<path>` as the super user", where `<path>` is the AppImage file (or, in a
  development build, the `syntheon-helper` next to the app). That is the only
  thing it tells you; there is no Syntheon-specific wording.
- **The AppImage is a user-writable file.** To get a root helper out of it,
  root re-executes the AppImage file itself. Anything that can already write
  that file as your user (malware running as you, a swapped download) could make
  the next prompt run something else as root. This is the same trust model as
  most AppImages that elevate: it protects against Syntheon's own mistakes and
  limits what the helper can do, but it does **not** defend against another
  program already running as you. Syntheon checks, against the kernel's mount table
  and not just environment variables, that `$APPDIR` is a FUSE mount whose name is
  the image's file name. That is **all** it proves: it does not prove the mount
  really comes from `$APPIMAGE`, and a user can mount their own FUSE filesystem
  under that name, so the check narrows the risk rather than removing it. If that is
  not acceptable for you, do not elevate: Syntheon has no limited mode, so it
  will not run.
- **`--syntheon-selfcheck`**: `Syntheon.AppImage --syntheon-selfcheck` is a tiny,
  read-only, unprivileged mode (no root, no polkit prompt, no window; it changes
  nothing). It runs the same decision the real launch uses on the live
  environment and prints the mount it saw plus `PASS` or `FAIL` and the reason
  (exit status 0 or 1). In an AppImage it passes only if the image's mount is
  verified; CI runs it on the real AppImage. If it says FAIL, Syntheon will not be
  able to ask for administrator access from that copy.
- Only one Syntheon runs per history folder; a second launch brings the first
  window forward. Its lock and socket live in your own runtime directory, not
  in the (possibly synced) history folder.
- **The encryption worker.** The CPU test's encryption part runs in a separate
  program, `syntheon-cpucrypto` (next to `Syntheon.exe` on Windows,
  `usr/libexec/` in the AppImage), with an environment built from scratch (no
  OpenSSL configuration from yours reaches it), started fresh for each
  measurement, read through its standard output and killed on Stop. It
  inherits the app's rights (so it runs elevated on Windows, as you on Linux)
  and takes no input but its fixed options.

## Known limits

- **A backslash in the AppImage's file name.** The AppImage runtime drops `\`
  from the name it gives its mount, so Syntheon can't verify the image and
  refuses to ask for administrator access (it fails closed). You see:
  `The AppImage's mount could not be verified (the mount is named
  'fuse.Syntheon.AppImage' / 'Syntheon.AppImage', the image is 'Syn\Bench.AppImage'
  (expected fs type 'fuse.Syn\Bench.AppImage')). Syntheon won't ask for
  administrator access from an image it can't verify.`
  **Workaround:** rename the file without the backslash (spaces, brackets and
  `#` are fine). `--syntheon-selfcheck` shows the same message.
- **Root must be able to enter the AppImage's FUSE mount.** The helper runs as
  root straight from the mounted image; there is no `--appimage-extract-and-run`
  fallback (it would extract to a predictable `/tmp` directory, and pkexec clears
  `TMPDIR`). On a system where root can't reach your FUSE mount, asking for
  administrator access fails with an error instead of falling back.

## Known harmless messages

When started from a terminal on KDE/GNOME you may see
`qt.qpa.services: Failed to register with host portal ... App info not found for
'io.github.dakotlyar.Syntheon'`. Qt tries to register the app with the desktop
portal, which only knows installed apps (a `.desktop` file in
`~/.local/share/applications`). It changes nothing in Syntheon and is left
visible on purpose: the only switch that silences it (`QT_NO_XDG_DESKTOP_PORTAL`)
would also turn off the portal's light/dark preference and file dialogs.

## Building it

See [BUILDING.md](BUILDING.md). To add a test, see
[docs/HOW-TO-ADD-A-TEST.md](docs/HOW-TO-ADD-A-TEST.md).

## Licence

GPL-3.0-or-later (see `LICENSE`). Syntheon includes zlib, Zstandard, LZ4, XZ
Utils (liblzma), OpenSSL (libcrypto), Qt (with the third-party code Qt itself
bundles, and ICU on Linux) and the Archivo and IBM Plex fonts, each under its
own licence: their notices are in `THIRD-PARTY-NOTICES.txt`,
which every package installs beside `LICENSE.txt` (`share/doc/syntheon/` in
the AppImage) and which **Settings > Licences** shows.

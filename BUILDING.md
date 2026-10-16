# Building SynBench

Requirements: CMake 3.25+, Ninja, a C++20 compiler (MSVC 2022 or GCC 11+),
Python 3 (packaging scripts) and **Qt 6.12.0** (pinned in `CMakePresets.json`;
the official binaries from `aqtinstall` are what CI uses).

## Presets

`CMakePresets.json` has two families:

| Preset | Developer hooks | Tests | For |
|---|---|---|---|
| `linux-x64-debug`, `linux-x64-release`, `windows-x64-debug`, `windows-x64-release` | **on** | on | day-to-day work and CI |
| `linux-x64-package`, `windows-x64-package`, `linux-arm64-cross`, `windows-arm64-cross` | **off** | off | what ships |

The *developer hooks* are the `SYNBENCH_SMOKE`, `SYNBENCH_TOUR*`,
`SYNBENCH_STUB_SCALE`, `SYNBENCH_DATA_DIR`, `SYNBENCH_SETTINGS_FILE` and
`SYNBENCH_NO_ELEVATION` environment variables, the screenshot tour and a few
test-only functions. They are compiled out when `SYNBENCH_DEV_HOOKS=OFF`
(packaging presets); the CI packaging jobs prove it with
`packaging/checks/no-dev-hooks.sh` / `.ps1`. Tests need the hooks, so
configuring tests without them is an error.

`QT_ROOT_DIR` must point at the Qt prefix (CI's `install-qt-action` sets it).
A local `CMakeUserPresets.json` (gitignored) can set `CMAKE_PREFIX_PATH`
instead.

## Linux x64

```sh
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
ctest --preset linux-x64-release           # QT_QPA_PLATFORM=offscreen
SYNBENCH_SMOKE=1 QT_QPA_PLATFORM=offscreen build/linux-x64-release/bin/SynBench
```

Never start a dev build without `SYNBENCH_SMOKE=1` (or `SYNBENCH_NO_ELEVATION=1`)
unless you want the polkit prompt: it runs `pkexec` on the helper next to the binary.

### The AppImage

```sh
cmake --preset linux-x64-package && cmake --build --preset linux-x64-package
cmake --install build/linux-x64-package --prefix stage
python3 packaging/linux/make-appdir.py --install-tree stage --appdir AppDir --qt "$QT_ROOT_DIR" \
    --qml-root src/app/qml --qml-scanner "$QT_ROOT_DIR/libexec/qmlimportscanner"
python3 packaging/checks/elf-check.py AppDir --arch x86_64
bash packaging/linux/make-appimage.sh AppDir SynBench-x86_64.AppImage x86_64
```

`make-appdir.py` walks `readelf` NEEDED entries (it does not use linuxdeploy),
bundles Qt, its plugins and QML imports and everything they need except the
system libraries in `packaging/linux/excludelist.txt`. `AppRun` runs
`usr/libexec/synbench-helper` when started with `--synbench-helper` (root
re-executes the AppImage through `pkexec`). `elf-check.py` checks the machine
type, that every library is bundled or a system library, and that no symbol is
newer than the architecture's floor: glibc 2.35 and jammy's libstdc++ on x64, glibc 2.39 and noble's on ARM64. The tools are downloaded by
`make-appimage.sh` and checked against pinned SHA-256 sums.

## Windows x64

From a Visual Studio x64 developer prompt:

```powershell
cmake --preset windows-x64-package "-DSYNBENCH_CRT_DIRS=$(./packaging/windows/find-crt.ps1 -Arch x64)"
cmake --build --preset windows-x64-package
cmake --install build/windows-x64-package --prefix stage
./packaging/windows/deploy.ps1 -Stage stage -HostQt $env:QT_ROOT_DIR
./packaging/checks/pe-check.ps1 -Tree stage -Arch x64
iscc packaging\windows\synbench.iss /DAppVersion=2026.11.0 /DArch=x64 /DSourceDir=$PWD\stage /DOutputDir=$PWD\out
wix build -arch x64 -d Version=26.11.0 -d SourceDir=$PWD\stage -d IconPath=$PWD\assets\synbench.ico -d "UpgradeCode={5E2C6D1B-8B1A-4C3E-9F55-0A6B2D7E4C11}" -o out\SynBench-windows-x64.msi packaging\windows\wix\Package.wxs
```

The C and C++ runtime ships next to the exe (`SYNBENCH_CRT_DIRS`), so neither
installer needs a redistributable. The exe embeds a manifest asking for
administrator rights. The Inno installer reuses the Python SynBench's AppId, so
it upgrades that install in place (`[InstallDelete]` removes the Python
leftovers). The MSI version is `YY.MM.N` (`26.11.0`); everything else shows
`2026.11.0`. `packaging/windows/test-install.ps1` is the silent
install / launch / uninstall / upgrade test CI runs.

## Cross builds (ARM64)

CI never runs ARM code. Everything is compiled, deployed and **statically
checked** (`pe-check.ps1`, `elf-check.py`); Windows ARM64 is then validated by
hand on a Snapdragon machine, and the Linux ARM64 AppImage is a CI artifact only.

### Windows ARM64 (on an x64 Windows machine)

1. Install the host Qt (`win64_msvc2022_64`) and the target Qt
   (`win64_msvc2022_arm64_cross_compiled`), the same 6.12.x, with `aqt`.
2. Open an `amd64_arm64` developer prompt (`vcvarsall.bat amd64_arm64`).
3. ```powershell
   cmake --preset windows-arm64-cross -DQT_HOST_PATH=<host Qt> -DCMAKE_PREFIX_PATH=<target Qt> `
         "-DSYNBENCH_CRT_DIRS=$(./packaging/windows/find-crt.ps1 -Arch arm64)"
   cmake --build --preset windows-arm64-cross
   cmake --install build/windows-arm64-cross --prefix stage
   ./packaging/windows/deploy.ps1 -Stage stage -HostQt <host Qt> -TargetQt <target Qt>
   ./packaging/checks/pe-check.ps1 -Tree stage -Arch arm64
   ```
   `deploy.ps1` runs the host `windeployqt` with `--qtpaths` of the target Qt.
4. The Inno and MSI steps are the x64 ones with `/DArch=arm64` and `-arch arm64`
   (and the other `UpgradeCode`); both tools are x64 programs.

### Linux ARM64 (on Ubuntu 24.04)

**Floor: glibc 2.39 (Ubuntu 24.04 or newer).** The official Qt 6.12 `gcc_arm64`
build needs glibc 2.38 and libstdc++ 3.4.32, so it cannot link against Ubuntu
22.04; the ARM64 sysroot is therefore 24.04 "noble", with GCC 13 (the same
libstdc++). Linux x64 stays on 22.04 / glibc 2.35.

```sh
sudo apt-get install g++-aarch64-linux-gnu binutils-aarch64-linux-gnu mmdebstrap
bash packaging/linux/make-sysroot.sh sysroot-arm64          # noble arm64 -dev packages, no qemu (runs mmdebstrap via sudo)
aqt install-qt linux desktop 6.12.0 linux_gcc_64             # host Qt
aqt install-qt linux_arm64 desktop 6.12.0 linux_gcc_arm64   # target Qt
cmake --preset linux-arm64-cross -DSYNBENCH_SYSROOT=$PWD/sysroot-arm64 -DSYNBENCH_TARGET_QT=<target Qt> \
      -DQT_HOST_PATH=<host Qt> -DCMAKE_PREFIX_PATH=<target Qt>
cmake --build --preset linux-arm64-cross && cmake --install build/linux-arm64-cross --prefix stage
python3 packaging/linux/make-appdir.py --install-tree stage --appdir AppDir --qt <target Qt> \
    --qml-root src/app/qml --qml-scanner <host Qt>/libexec/qmlimportscanner \
    --cross-prefix aarch64-linux-gnu- --sysroot sysroot-arm64
python3 packaging/checks/elf-check.py AppDir --arch aarch64 --cross-prefix aarch64-linux-gnu-
bash packaging/linux/make-appimage.sh AppDir SynBench-aarch64.AppImage aarch64
```

The sysroot is cached in CI by a hash of `packaging/linux/linux-arm64-sysroot.txt`
and the scripts. `elf-check.py` takes its limits per architecture (x86_64:
glibc 2.35; aarch64: glibc 2.39, GLIBCXX_3.4.33, CXXABI_1.3.15).

## CI

`.github/workflows/ci.yml` builds, tests and smoke-launches both OSes, then
the packaging and cross jobs and a lint job. `release.yml` runs it for a tag
`v<VERSION>` and opens a **draft** release with five files (the Linux ARM64
AppImage stays an artifact). `sysroot-arm64.yml` pre-warms the sysroot cache.

## Version

`VERSION` holds `YYYY.MM.N`. Dev builds show `-dev+<sha>`; packaging presets
set `SYNBENCH_RELEASE_BUILD=ON`, which drops it. The Windows file version is
`YYYY.MM.N.0` and the MSI version `YY.MM.N`.

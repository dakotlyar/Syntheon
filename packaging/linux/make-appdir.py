#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds an AppDir from `cmake --install` output plus the Qt it was built with.

Used for BOTH x86_64 and aarch64 (design 8): linuxdeploy can't inspect a
foreign-architecture binary on an x64 host, so the dependency walk uses
`readelf` (a cross one when --cross-prefix is given) and never executes
anything from the target.

  make-appdir.py --install-tree stage --appdir AppDir --qt <target Qt prefix>
                 --qml-root src/app/qml [--qml-scanner <host>/libexec/qmlimportscanner]
                 [--cross-prefix aarch64-linux-gnu-] [--sysroot <sysroot>]

Layout produced:
  AppDir/AppRun                       (dispatches --synbench-helper)
  AppDir/<app-id>.desktop, synbench.png, .DirIcon
  AppDir/usr/bin/SynBench (+ qt.conf), usr/libexec/synbench-helper
  AppDir/usr/lib       bundled libraries (Qt and whatever Qt needs, minus the exclude list)

The bundling rule, the same for both architectures (one exclude list):
  * a library is NOT bundled if it is on packaging/linux/excludelist.txt: glibc
    and its friends, the graphics drivers/loaders, the windowing core
    (libX11, libxcb, Wayland), fontconfig/freetype/harfbuzz, D-Bus/systemd/glib,
    xkbcommon. Those belong to the user's system and must match its drivers.
  * EVERYTHING ELSE that a bundled file needs (libSM/libICE, the libxcb-*
    helper libraries the xcb plugin uses, libbrotli, libzstd, ICU, ...) is
    bundled, taken from the Qt prefix first, then from the build machine (x64)
    or the sysroot (arm64). So whatever Qt needs beyond the exclude list must be
    present there: for arm64 that is the package list in linux-arm64-sysroot.txt.
  * a needed library that is neither excluded nor found is an error, not a
    silent omission.
  AppDir/usr/plugins   Qt plugins     AppDir/usr/qml   QML imports
"""
import argparse
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

APP_ID = "io.github.dakotlyar.SynBench"

# Qt plugin folders that SynBench can use. (tls, sqldrivers other than sqlite,
# designer, ... are left out on purpose.)
PLUGIN_DIRS = {
    "platforms": ["libqxcb.so", "libqwayland-egl.so", "libqwayland-generic.so", "libqoffscreen.so", "libqminimal.so"],
    "xcbglintegrations": None,  # everything
    "wayland-decoration-client": None,
    "wayland-graphics-integration-client": None,
    "wayland-shell-integration": None,
    "imageformats": ["libqpng.so", "libqjpeg.so", "libqsvg.so", "libqico.so", "libqgif.so"],
    "iconengines": ["libqsvgicon.so"],
    "platforminputcontexts": None,
    "sqldrivers": ["libqsqlite.so"],
}


# Pieces of the QML modules the scanner returns that SynBench never loads
# (it uses the Basic style only). Pruned BEFORE the library walk, so the Qt
# libraries only they needed are not bundled either.
PRUNE_QML = [
    "QtQuick/Controls/Material", "QtQuick/Controls/Universal", "QtQuick/Controls/Fusion", "QtQuick/Controls/Imagine",
    "QtQuick/Controls/FluentWinUI3", "QtQuick/Controls/macOS", "QtQuick/Controls/iOS", "QtQuick/Controls/Windows",
    "QtQuick/Controls/designer", "QtQuick/Controls/Native", "QtQuick/NativeStyle", "QtQuick/Particles", "QtQuick/Scene2D", "QtQuick/Scene3D",
    "QtQuick/VirtualKeyboard", "QtQuick/tooling", "QtQml/XmlListModel", "QtQuick/LocalStorage", "QtQml/StateMachine",
    "QtQuick/Dialogs/quickimpl/qml/+Material", "QtQuick/Dialogs/quickimpl/qml/+Universal",
]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


class Deployer:
    def __init__(self, a):
        self.a = a
        self.readelf = a.cross_prefix + "readelf"
        self.appdir = Path(a.appdir)
        self.usr = self.appdir / "usr"
        self.qt = Path(a.qt)
        self.exclude = [l.strip() for l in Path(a.exclude).read_text().splitlines() if l.strip() and not l.startswith("#")]
        self.search = [self.qt / "lib"]
        if a.sysroot:
            sr = Path(a.sysroot)
            for d in ("lib/aarch64-linux-gnu", "usr/lib/aarch64-linux-gnu", "lib/x86_64-linux-gnu", "usr/lib/x86_64-linux-gnu", "usr/lib", "lib"):
                self.search.append(sr / d)
        else:
            for d in ("/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", "/lib/x86_64-linux-gnu", "/lib64", "/lib"):
                self.search.append(Path(d))
        self.done = set()
        self.problems = []

    def excluded(self, soname):
        return any(soname.startswith(p) for p in self.exclude)

    def needed(self, elf):
        out = run([self.readelf, "-d", str(elf)])
        return re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", out)

    def is_elf(self, p):
        try:
            with open(p, "rb") as f:
                return f.read(4) == b"\x7fELF"
        except OSError:
            return False

    def resolve(self, soname):
        # An already-bundled copy first, then Qt's lib dir, then the system / sysroot.
        for d in [self.usr / "lib"] + self.search:
            c = d / soname
            if c.exists():
                return c
        return None

    def bundle_deps(self, elf):
        for soname in self.needed(elf):
            if self.excluded(soname):
                continue
            dest = self.usr / "lib" / soname
            if soname in self.done or dest.exists():
                continue
            src = self.resolve(soname)
            if src is None:
                self.problems.append(f"{elf.name}: needs {soname}, not found and not on the exclude list")
                continue
            self.done.add(soname)
            (self.usr / "lib").mkdir(parents=True, exist_ok=True)
            shutil.copy2(src.resolve(), dest)  # the real file, under the soname it is asked for
            self.bundle_deps(dest)

    def copy_tree(self, src, dst):
        shutil.copytree(src, dst, symlinks=False, dirs_exist_ok=True)

    def run(self):
        a = self.a
        if self.appdir.exists():
            shutil.rmtree(self.appdir)
        self.copy_tree(a.install_tree, self.usr)
        app = self.usr / "bin" / "SynBench"
        if not app.exists():
            sys.exit(f"{app} is missing: run cmake --install first")

        # 1. Qt plugins
        for folder, only in PLUGIN_DIRS.items():
            src = self.qt / "plugins" / folder
            if not src.is_dir():
                continue
            dst = self.usr / "plugins" / folder
            dst.mkdir(parents=True, exist_ok=True)
            for f in sorted(src.iterdir()):
                if f.is_file() and (only is None or f.name in only):
                    shutil.copy2(f, dst / f.name)

        # 2. QML imports the UI uses (found by Qt's own scanner)
        qml_src = self.qt / "qml"
        modules = set()
        if a.qml_scanner:
            out = run([a.qml_scanner, "-rootPath", a.qml_root, "-importPath", str(qml_src)])
            for item in json.loads(out):
                if item.get("type") == "module" and item.get("path"):
                    try:
                        modules.add(Path(item["path"]).resolve().relative_to(qml_src.resolve()))
                    except ValueError:
                        pass
        else:
            modules = {Path(m) for m in ("QtQml", "QtQuick", "QtCore")}
        for rel in sorted(modules):
            src = qml_src / rel
            if src.is_dir():
                self.copy_tree(src, self.usr / "qml" / rel)

        for rel in PRUNE_QML:
            shutil.rmtree(self.usr / "qml" / rel, ignore_errors=True)
        # Controls' plugin loads every style it finds; the Basic one is all that's left.

        # 3. libraries: every ELF now in the AppDir, closed over NEEDED
        for p in sorted(self.appdir.rglob("*")):
            if p.is_file() and not p.is_symlink() and self.is_elf(p):
                self.bundle_deps(p)
        # libraries pulled in last may themselves be plugins' targets: loop to a fixpoint
        for p in sorted((self.usr / "lib").glob("*")):
            if p.is_file() and self.is_elf(p):
                self.bundle_deps(p)

        # 4. Qt configuration next to the app, so it finds the bundled pieces
        (self.usr / "bin" / "qt.conf").write_text("[Paths]\nPrefix = ..\nPlugins = plugins\nQml2Imports = qml\nLibraries = lib\n")

        # 5. AppRun, desktop file, icon
        run_script = self.appdir / "AppRun"
        run_script.write_text(
            "#!/bin/sh\n"
            "# SPDX-License-Identifier: GPL-3.0-or-later\n"
            "# pkexec runs `<this AppImage> --synbench-helper` as root (design 6).\n"
            'HERE="$(dirname "$(readlink -f "$0")")"\n'
            'case "${1:-}" in\n'
            '    --synbench-helper) shift; exec "$HERE/usr/libexec/synbench-helper" "$@" ;;\n'
            "esac\n"
            'exec "$HERE/usr/bin/SynBench" "$@"\n')
        run_script.chmod(run_script.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        share = self.usr / "share"
        shutil.copy2(share / "applications" / f"{APP_ID}.desktop", self.appdir / f"{APP_ID}.desktop")
        icon = share / "icons" / "hicolor" / "256x256" / "apps" / "synbench.png"
        shutil.copy2(icon, self.appdir / "synbench.png")
        diricon = self.appdir / ".DirIcon"
        if diricon.exists() or diricon.is_symlink():
            diricon.unlink()
        shutil.copy2(icon, diricon)

        if self.problems:
            for line in self.problems:
                print("ERROR:", line, file=sys.stderr)
            sys.exit(1)
        n = sum(1 for _ in (self.usr / "lib").glob("*"))
        print(f"AppDir ready: {self.appdir} ({n} bundled libraries)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--install-tree", required=True)
    ap.add_argument("--appdir", required=True)
    ap.add_argument("--qt", required=True, help="the TARGET Qt prefix (the one the app was linked against)")
    ap.add_argument("--qml-root", default="src/app/qml")
    ap.add_argument("--qml-scanner", help="qmlimportscanner (the host's is fine for a cross build)")
    ap.add_argument("--cross-prefix", default="", help="e.g. aarch64-linux-gnu- for readelf")
    ap.add_argument("--sysroot")
    ap.add_argument("--exclude", default=str(Path(__file__).with_name("excludelist.txt")))
    Deployer(ap.parse_args()).run()


if __name__ == "__main__":
    main()

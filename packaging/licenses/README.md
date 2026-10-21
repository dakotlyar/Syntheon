# Licence texts kept in the repository

For THIRD-PARTY-NOTICES.txt (cmake/ThirdPartyNotices.cmake, cmake/QtNotices.cmake):

- `LGPL-3.0.txt`: Qt's licence (LGPL-3.0-only), which Qt's binaries don't carry.
- `ICU-73.txt`: `icu4c/LICENSE` of ICU 73.2 (tag `release-73-2` of
  github.com/unicode-org/icu), the version the official Qt 6.12 Linux binaries
  bundle (libicu*.so.73, shipped in the AppImage).
- `spdx/<id>.txt`: the SPDX licence texts (spdx/license-list-data, tag v3.27.0,
  `text/<id>.txt`) of the licences that code bundled inside Qt uses, as Qt's
  SBOM (`<Qt>/sbom/*.spdx.json`) names them, checked against the SBOMs of
  every Qt build CI uses (linux_gcc_64, linux_gcc_arm64, win64_msvc2022_64,
  win64_msvc2022_arm64_cross_compiled; the Windows ones also bundle zlib and
  FreeType, hence `Zlib` and `MIT-open-group`). A licence a newer Qt adds and
  that has no file here fails the configure with its id: add its text from the
  same place.
- The GPU test's third-party code (`cmake/GpuThirdParty.cmake`, Phase 4a):
  volk (MIT; its `LICENSE.md` comes with the pinned tarball) and glad's
  generated OpenGL loader, whose own header says `(WTFPL OR CC0-1.0) AND
  Apache-2.0`. SynBench uses it under CC0-1.0, so the notices carry
  `spdx/CC0-1.0.txt` for the loader and `spdx/Apache-2.0.txt` for the Khronos
  registry parts of its output. `khrplatform-MIT.txt` is the MIT-style notice
  at the top of `KHR/khrplatform.h`.
- `misc-fixed-5x7.txt`: the public-domain statement of X.Org's misc-fixed 5x7
  font (the GPU overlay's bitmap font, `font-misc-misc` 1.1.3).

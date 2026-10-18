# Licence texts kept in the repository

For THIRD-PARTY-NOTICES.txt (cmake/ThirdPartyNotices.cmake, cmake/QtNotices.cmake):

- `LGPL-3.0.txt`: Qt's licence (LGPL-3.0-only), which Qt's binaries don't carry.
- `ICU-73.txt`: `icu4c/LICENSE` of ICU 73.2 (tag `release-73-2` of
  github.com/unicode-org/icu), the version the official Qt 6.12 Linux binaries
  bundle (libicu*.so.73, shipped in the AppImage).
- `spdx/<id>.txt`: the SPDX licence texts (spdx/license-list-data, tag v3.27.0,
  `text/<id>.txt`) of the licences that code bundled inside Qt uses, as Qt's
  SBOM (`<Qt>/sbom/*.spdx.json`) names them. A licence a newer Qt adds and
  that has no file here fails the configure with its id: add its text from the
  same place.

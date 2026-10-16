// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sb {

// Registers the bundled interface fonts (Archivo, IBM Plex Sans / Mono; OFL,
// assets/fonts) with Qt. Returns false if one couldn't be loaded; the theme
// then falls back to the system fonts.
bool loadBundledFonts();

} // namespace sb

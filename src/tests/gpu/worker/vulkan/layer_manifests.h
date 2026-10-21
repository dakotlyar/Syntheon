// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Vulkan implicit-layer manifests and which of them are really loaded (Phase 4a
// design §4.4, Antilles' rule: warn only about layers actually active in the
// worker process). Pure and Qt-free; vk_context.cpp supplies the manifests (the
// loader's folders on Linux, the HKLM/HKCU ImplicitLayers registry values on
// Windows) and the loaded modules (/proc/self/maps, EnumProcessModules).

#include <string>
#include <vector>

namespace sb::gpu::vk {

struct LayerEntry {
    std::string name;     // "VK_LAYER_VALVE_steam_overlay_64"
    std::string library;  // library_path: absolute when the manifest gave a path (relative ones resolved
                          // against the manifest's folder), else the bare name the loader searches for
};

// The layers a manifest declares: `layer` (one object) or each entry of `layers[]`. Entries without a
// name or a library_path are skipped; malformed JSON gives none.
std::vector<LayerEntry> parseLayerManifest(const std::string &json, const std::string &manifestPath);

// Layers that come with a graphics driver and are not overlays or recorders: NVIDIA Optimus and
// present layers, AMD switchable graphics, Mesa's device select. They are never reported.
bool isDriverInternalLayer(const std::string &name);

// Lower case with '/' separators (Windows paths compare case-insensitively).
std::string normalisePath(std::string path);
// Full paths equal after normalisation; a bare library name matches a loaded path ending in "/name".
bool libraryMatches(const std::string &library, const std::string &loadedPath);

// The names (sorted, once each) of the entries whose library is among `loadedPaths`, minus the
// validation layer (ours) and the driver-internal ones.
std::vector<std::string> activeLayers(const std::vector<LayerEntry> &entries, const std::vector<std::string> &loadedPaths);

} // namespace sb::gpu::vk

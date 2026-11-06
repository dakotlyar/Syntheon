// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sb::legacy {

// What the app's files were called before the rename to Syntheon. People have
// history and settings under these names, and an old build can still be running
// or be gone back to, so they live on as read-only addresses: nothing in
// src/storage or src/app spells them any other way. See legacy_migration.h.
inline constexpr const char *kFolderName = "SynBench";       // data, config and Windows run folders
inline constexpr const char *kDbFileName = "synbench.db";
inline constexpr const char *kRuntimeDirName = "synbench";   // under $XDG_RUNTIME_DIR
inline constexpr const char *kFilePrefix = "synbench-";      // lock files: <prefix><key>.lock

} // namespace sb::legacy

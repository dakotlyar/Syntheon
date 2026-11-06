// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace sb::legacy {

// Where the build from before the rename kept things. Asking never creates
// anything: on a machine without old data these folders must stay absent.

// GenericDataLocation + the old folder name.
QString defaultDataDir();
// The old per-user settings.ini.
QString settingsFile();
// The old build's single-instance lock for a history folder (see
// instance_paths.h for the layout), or empty if the folder it would sit in
// doesn't exist: an old build that isn't running has no live lock.
QString lockFilePath(const QString &folder);

} // namespace sb::legacy

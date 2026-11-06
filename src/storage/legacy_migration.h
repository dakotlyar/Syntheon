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

// Copying to the new names. The rules for everything below: the old file is only
// ever read; work happens in a temp file beside the new one, which appears by an
// atomic rename; and the new file existing is the only sign that it's done, so
// there is no marker to get out of step, and a cut-short run just starts over.
enum class SettingsStatus { NothingToDo, Migrated, Failed };
struct SettingsResult {
    SettingsStatus status = SettingsStatus::NothingToDo;
    QString error;
};
// Copies the old settings.ini to `newIni` unless that exists. A dataLocation that
// is just the old default folder is dropped from the copy: kept, the new app would
// go on using the old folder as if the user had chosen it. On Failed the caller
// should use `legacyIni` for the session, so that a custom data location is still
// honored and the next launch tries again.
SettingsResult migrateSettings(const QString &legacyIni, const QString &newIni, const QString &legacyDefaultDir);

} // namespace sb::legacy

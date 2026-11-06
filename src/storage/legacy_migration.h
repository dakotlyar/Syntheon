// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <functional>

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

enum class AdoptStatus {
    NothingToDo,  // the new file exists, or there is no old one
    Adopted,
    LegacyInUse,  // an old build has the old history open: nothing was done
    Failed,       // nothing was created: opening an empty history now would strand the old one
};
struct AdoptResult {
    AdoptStatus status = AdoptStatus::NothingToDo;
    QString error;
    int runCount = 0;      // Adopted: how many runs came across
    QString legacyFolder;  // Adopted: where the old copy stays
};
// The points adoptHistory() reports to its hook, for the tests. A hook that
// returns false ends the call there with no cleanup: what a kill looks like.
enum class Stage { BeforeCopy, AfterCopy, AfterVerify, AfterRename };

// Copies the old history to `newDb` unless that exists. The old file's WAL is
// folded in first (a crash can leave recent runs only there), which changes no data
// and leaves it readable by the old build. The copy is checked against the old file
// before it gets its final name. `legacyLockPath` (may be empty) is held throughout,
// so a running old build can't write during the copy and one that starts is refused.
AdoptResult adoptHistory(const QString &legacyDb, const QString &newDb, const QString &legacyLockPath,
                         const std::function<bool(Stage)> &hook = {});

} // namespace sb::legacy

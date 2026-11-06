// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QUuid>

namespace sb::fs {

// Deletes `path` and everything under it without ever following a symbolic
// link (or Windows junction) at any level: a link is removed as a link, its
// target is never touched. Returns true if nothing is left at `path`.
bool removeTreeNoFollow(const QString &path);

// True for a symbolic link, a Windows junction, or any other Windows reparse
// point (checked from the file attributes, not only Qt's isSymLink()).
bool isLinkOrReparsePoint(const QString &path);

// Removes `path` if it is a link or reparse point: the link itself (remove(),
// then rmdir() for a Windows directory link), never what it points to.
bool removeLinkItself(const QString &path);

// A test's scratch folder is <scratch root>/syntheon-<run uuid>/<test id>.
// True only if `path` is exactly that shape: its parent not a link, and the
// parent's canonical path equal to the scratch root's canonical path +
// "/syntheon-<uuid>". With allowLinkAtPath false (the default) `path` itself
// must not be a link either; true lets a link sit at that exact place (so it
// can be removed as a link). Anything else must never be touched.
bool isSafeScratchPath(const QString &path, const QString &scratchRoot, const QUuid &runUuid,
                       bool allowLinkAtPath = false);

enum class ScratchCleanup {
    Removed,      // the folder and everything in it
    LinkRemoved,  // a link (or reparse point) stood there: only it was removed
    Unsafe,       // not where a scratch folder of this run can be: nothing touched
    Failed,       // couldn't delete; try again later
};
// THE one scratch cleanup, shared by the orchestrator (after a test) and by
// recovery (at startup): never follows a link or reparse point at any level.
ScratchCleanup cleanupScratch(const QString &path, const QString &scratchRoot, const QUuid &runUuid);

QString scratchRunFolderName(const QUuid &runUuid);
// "synbench-<uuid>": accepted by isSafeScratchPath for rows left by a pre-rename run, never created.
QString legacyScratchRunFolderName(const QUuid &runUuid);

} // namespace sb::fs

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QUuid>

namespace sb::fs {

// Deletes `path` and everything under it without ever following a symbolic
// link (or Windows junction) at any level: a link is removed as a link, its
// target is never touched. Returns true if nothing is left at `path`.
bool removeTreeNoFollow(const QString &path);

// A test's scratch folder is <scratch root>/synbench-<run uuid>/<test id>.
// True only if `path` is exactly that shape: not itself a link, its parent
// not a link, and the parent's canonical path equal to the scratch root's
// canonical path + "/synbench-<uuid>". Anything else must never be deleted.
bool isSafeScratchPath(const QString &path, const QString &scratchRoot, const QUuid &runUuid);

QString scratchRunFolderName(const QUuid &runUuid);

} // namespace sb::fs

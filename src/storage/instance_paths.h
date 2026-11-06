// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace sb::instance {

// Where the single-instance lock and socket for a history folder live: a
// per-user directory, never the history folder itself (which may be synced by
// Nextcloud/OneDrive or sit on NFS, where a lock file and its host name are
// meaningless). Keyed by a hash of the folder's CANONICAL path, so a symlinked
// path and the real one are the same history.
//
// Linux: $XDG_RUNTIME_DIR/syntheon (0700, owned by us; cleared at logout, so
// a lock left by a crash can't outlive the session). Windows: the user's
// %LOCALAPPDATA%\Syntheon\run. Two users on one machine therefore don't share
// a lock even for a shared folder: Syntheon is a per-user app.

// The folder's canonical path (the cleaned absolute path if it doesn't exist yet).
QString canonicalFolder(const QString &folder);
// 16 hex digits.
QString key(const QString &folder);
// The per-user directory, created with mode 0700 and checked to be ours;
// empty if it can't be made safe.
QString stateDir();
QString lockFilePath(const QString &folder);
// Unix: an absolute socket path in stateDir(). Windows: a pipe name that
// includes the user name.
QString socketName(const QString &folder);

} // namespace sb::instance

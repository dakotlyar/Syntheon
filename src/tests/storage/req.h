// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Storage test's requirement logic (Phase 4b design §6): the network-drive
// blocklist, the free-space guard, the wear-note arithmetic and the target
// scratch-folder name. Pure (QtCore only), fully unit-testable — never touches
// the disk. Mirrors src/tests/ram/sweep/* (small pure files, one namespace).

#include <QString>
#include <QtGlobal>

#include <QUuid>
#include <cstdint>

namespace sb::storage {

enum class RejectReason { None, Network, ReadOnly, TooSmall, NoFreeSpace, NotFixed };

// Is this mount's filesystem/root a network (SMB/NFS/CIFS/9p/sshfs/…) one?
// Also answers for a Windows drive type. Never touches the disk: a pure
// classifier over the mount's strings + type.
bool isNetworkMount(const QString &rootPath, const QString &fileSystem);   // Linux + Windows fs names
bool isNetworkDriveType(std::uint32_t winDriveType);                       // DRIVE_REMOTE, DRIVE_UNKNOWN etc.

// Free-space guard (locked): free < fileSize x 1.1 + 2 GiB => refuse.
constexpr qint64 kFreeMarginBytes = 2ll * 1024 * 1024 * 1024;   // 2 GiB
bool hasEnoughFreeSpace(qint64 freeBytes, qint64 fileSizeBytes); // free >= fileSize*1.1 + margin

// The "writes about N GB to this SSD" number: prep (full file written) +
// worst-case measured writes (every write phase re-writes the whole file in
// the worst case; a 5 s window on a fast NVMe can cover the whole 4 GiB).
qint64 worstCaseWriteBytes(qint64 fileSizeBytes, int runs);     // = fileSize * (1 + runs) conservatively

// Map a mount root to a writable test folder name that is always inside the
// mount and hidden (P6 / D5).
QString targetScratchName(const QString &mountRoot, const QUuid &runUuid);  // ".../.syntheon-storage-<uuid>"

} // namespace sb::storage
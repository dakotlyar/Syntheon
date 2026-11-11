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

// The pre-run wear-note estimate (D3 "no surprise writes"): the full-file prep
// plus a fast drive's write throughput over the total write-window time. The
// write phases are time-boxed (they keep writing the whole 5 s x 3 runs), so a
// naive "prep + one rewrite" under-reports a fast NVMe by 8-15x; this
// over-estimates instead.
qint64 conservativeWriteBytes(qint64 fileSizeBytes);  // = fileSize + 3000 MB/s x 65 s

// Map a mount root to a writable test folder name that is always inside the
// mount and hidden (P6 / D5).
QString targetScratchName(const QString &mountRoot, const QUuid &runUuid);  // ".../.syntheon-storage-<uuid>"

// The exact user-facing strings (locked by tst_storage_ids, Phase 4b §3.3,
// §3.5, §3.4). Pure: these are the wording the picker, the preconditions and
// the run's refusal messages show, so they are never reworded without a test.

// A short reason for each rejection.
QString rejectReasonText(RejectReason);
//   None -> "" ; Network -> "network — blocked" ; ReadOnly -> "read-only" ;
//   TooSmall -> "too small" ; NoFreeSpace -> "not enough free space" ;
//   NotFixed -> "not a fixed drive"

// The disabled picker item's reason for a network mount (mockup).
QString networkBlockReason(const QString &fileSystem);  // "cifs · network — blocked"

// The refuse-to-run message when free space is short (the guard, §3.3).
QString freeSpaceRefusal(qint64 freeBytes, qint64 fileSizeBytes);
//   "Not enough free space: the test needs about %1, the drive has %2",
//   %1 = fileSize*1.1 + kFreeMarginBytes, %2 = freeBytes, both as GiB (1
//   decimal, a trailing ".0" dropped).

// The wear note before the run (§3.4): N = writeBytes (the worst-case write
// footprint), in GB (1'000'000'000, rounded to a whole number).
QString wearNote(qint64 writeBytes, const QString &kind);
//   kind == "SSD" -> "writes about %1 GB to this SSD", else "… to this drive"

} // namespace sb::storage
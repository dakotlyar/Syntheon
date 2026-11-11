// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/storage/req.h"

#include <QDir>

namespace sb::storage {

namespace {

// The network-filesystem denylist (§6), matched case-insensitively: the
// filesystem type strings produced by QStorageInfo and the /proc/self/mounts
// readout. CDFS and UDF (optical) are deliberately NOT here — they are
// removable, which is warned rather than blocked (D2).
const char *const kNetworkFileSystems[] = {
    "nfs",     "nfs4",      "smbfs",      "cifs", "9p",      "sshfs", "fuse.sshfs",
    "fuse",    "davfs2",    "rclone",     "s3fs", "afpfs",   "gvfs",  "fuse.rclone",
};

} // namespace

bool isNetworkMount(const QString &rootPath, const QString &fileSystem)
{
    // The filesystem type is the authoritative signal; match it against the
    // denylist case-insensitively (CIFS/SMB and NFS spellings vary).
    const QString dehyphen = fileSystem.simplified().toLower();
    for (const char *name : kNetworkFileSystems) {
        if (dehyphen == QLatin1StringView(name))
            return true;
    }
    // A non-"/dev/" root on Unix implies a user-space or network mount
    // (FUSE, NFS, SMB): the snapshot already filters those, but a caller that
    // passes one in directly should not have it misclassified as a local disk.
    const QString cleaned = QDir::cleanPath(rootPath);
    if (!cleaned.isEmpty() && !cleaned.startsWith(QLatin1Char('/')))
        return true;  // a relative/windows-style path is not a fixed Unix mount
    return false;
}

bool isNetworkDriveType(std::uint32_t winDriveType)
{
    // DRIVE_REMOTE (4) and DRIVE_UNKNOWN (0) are treated as network / not a
    // fixed local volume; DRIVE_FIXED (3), DRIVE_REMOVABLE (2), DRIVE_CDROM (5)
    // and DRIVE_RAMDISK (6) are not network (removable/optical are warned, D2).
    return winDriveType == 4u || winDriveType == 0u;
}

bool hasEnoughFreeSpace(qint64 freeBytes, qint64 fileSizeBytes)
{
    if (freeBytes < 0 || fileSizeBytes < 0)
        return false;
    // fileSize x 1.1, computed without floating point: 11/10 of the size.
    const qint64 needed = fileSizeBytes + fileSizeBytes / 10;
    return freeBytes >= needed + kFreeMarginBytes;
}

qint64 worstCaseWriteBytes(qint64 fileSizeBytes, int runs)
{
    if (fileSizeBytes < 0 || runs < 0)
        return 0;
    return fileSizeBytes * (1 + runs);
}

QString targetScratchName(const QString &mountRoot, const QUuid &runUuid)
{
    // The hidden per-run folder on the target mount (§6, P6 / D5). Using
    // QDir::filePath keeps the separator right on both Linux ("/") and
    // Windows ("\"), and the compact uuid form keeps the path short (P-PATH).
    return QDir::cleanPath(QDir(mountRoot).filePath(QStringLiteral(".syntheon-storage-")
                                                    + runUuid.toString(QUuid::WithoutBraces)));
}

} // namespace sb::storage
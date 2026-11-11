// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/storage/req.h"

#include <QDir>

#include <cmath>

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

// bytes -> a GiB string (1024-based) with one decimal, the trailing ".0"
// dropped (e.g. 6.5 GiB, 8 GiB). Used for the free-space refusal.
QString giBText(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("0 GiB");
    const double gib = double(bytes) / (1024.0 * 1024.0 * 1024.0);
    QString s = QString::number(gib, 'f', 1);
    if (s.endsWith(QLatin1StringView(".0")))
        s.chop(2);
    return s + QStringLiteral(" GiB");
}

// bytes -> a whole GB string (1000-based), rounded to the nearest whole GB.
// Used for the wear note, which reads "about N GB".
QString gbText(qint64 bytes)
{
    const qint64 gb = qint64(std::llround(double(bytes) / 1'000'000'000.0));
    if (gb < 0)
        return QStringLiteral("0 GB");
    return QString::number(gb) + QStringLiteral(" GB");
}

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
    // A UNC share ("//nas/mjolnir", or "\\nas\mjolnir" normalised) is a
    // network mount whatever the filesystem string claims (e.g. NTFS). Both
    // separator styles are folded to "/" (fromNativeSeparators only converts
    // the host's native one, so a Windows-style path wouldn't normalise here).
    QString slash = rootPath.trimmed();
    slash.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (slash.startsWith(QLatin1StringView("//")))
        return true;
    // A Windows drive root ("C:", "C:/", "D:\", or longer "C:/data") is a
    // local volume, never network.
    if (slash.size() >= 2 && slash.at(0).isLetter() && slash.at(1) == QLatin1Char(':')
        && (slash.size() == 2 || slash.at(2) == QLatin1Char('/')))
        return false;
    // Any other path that isn't absolute on Unix (a relative path or an
    // unknown scheme) is not a fixed local mount: the snapshot already
    // filters these, but a direct caller shouldn't misclassify one.
    const QString cleaned = QDir::cleanPath(rootPath);
    if (!cleaned.isEmpty() && !cleaned.startsWith(QLatin1Char('/')))
        return true;
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

QString rejectReasonText(RejectReason r)
{
    switch (r) {
    case RejectReason::None:
        return QString();
    case RejectReason::Network:
        return QStringLiteral("network — blocked");
    case RejectReason::ReadOnly:
        return QStringLiteral("read-only");
    case RejectReason::TooSmall:
        return QStringLiteral("too small");
    case RejectReason::NoFreeSpace:
        return QStringLiteral("not enough free space");
    case RejectReason::NotFixed:
        return QStringLiteral("not a fixed drive");
    }
    return QString();
}

QString networkBlockReason(const QString &fileSystem)
{
    return fileSystem.toLower() + QStringLiteral(" · network — blocked");
}

QString freeSpaceRefusal(qint64 freeBytes, qint64 fileSizeBytes)
{
    // The guard (§3.3): what the run needs is fileSize x 1.1 + the 2 GiB
    // margin (the same arithmetic as hasEnoughFreeSpace).
    const qint64 needed = fileSizeBytes + fileSizeBytes / 10 + kFreeMarginBytes;
    return QStringLiteral("Not enough free space: the test needs about %1, the drive has %2")
        .arg(giBText(needed), giBText(freeBytes));
}

QString wearNote(qint64 writeBytes, const QString &kind)
{
    if (kind == QLatin1StringView("SSD"))
        return QStringLiteral("writes about %1 to this SSD").arg(gbText(writeBytes));
    return QStringLiteral("writes about %1 to this drive").arg(gbText(writeBytes));
}

} // namespace sb::storage
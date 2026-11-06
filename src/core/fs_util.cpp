// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/fs_util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sb::fs {

namespace {

bool isLink(const QFileInfo &fi)
{
    if (fi.isSymbolicLink() || fi.isJunction())
        return true;
#ifdef Q_OS_WIN
    // Qt doesn't flag every kind of reparse point; ask the file system.
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(fi.filePath()).utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
}

} // namespace

bool isLinkOrReparsePoint(const QString &path)
{
    return isLink(QFileInfo(path));
}

bool removeLinkItself(const QString &path)
{
    if (!isLink(QFileInfo(path)))
        return false;
    // A file link goes with remove(); a directory link (junction or
    // directory symlink) needs rmdir(), which removes the reparse point and
    // never its target.
    return QFile::remove(path) || QDir().rmdir(path);
}

QString scratchRunFolderName(const QUuid &runUuid)
{
    return QStringLiteral("syntheon-%1").arg(runUuid.toString(QUuid::WithoutBraces));
}

QString legacyScratchRunFolderName(const QUuid &runUuid)
{
    // The name before the rename: a crashed old run can leave a row pointing here.
    return QStringLiteral("synbench-%1").arg(runUuid.toString(QUuid::WithoutBraces));
}

bool removeTreeNoFollow(const QString &path)
{
    const QFileInfo fi(path);  // QFileInfo describes the link itself (lstat)
    if (isLink(fi))
        return removeLinkItself(path);  // never what it points to
    if (!fi.exists())
        return true;
    if (!fi.isDir())
        return QFile::remove(path);
    const QFileInfoList children =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    bool ok = true;
    for (const QFileInfo &child : children)
        ok = removeTreeNoFollow(child.filePath()) && ok;
    return QDir().rmdir(path) && ok;
}

bool isSafeScratchPath(const QString &path, const QString &scratchRoot, const QUuid &runUuid, bool allowLinkAtPath)
{
    const QFileInfo fi(path);
    if (!fi.isAbsolute() || (!allowLinkAtPath && isLink(fi)))
        return false;
    const QString name = fi.fileName();
    if (name.isEmpty() || name == QLatin1StringView(".") || name == QLatin1StringView(".."))
        return false;
    const QFileInfo parent(fi.path());
    if (isLink(parent))
        return false;
    const QString current = scratchRunFolderName(runUuid);
    const QString legacy = legacyScratchRunFolderName(runUuid);
    const QString matched = parent.fileName() == current ? current : (parent.fileName() == legacy ? legacy : QString());
    if (matched.isEmpty())
        return false;
    const QString root = QFileInfo(scratchRoot).canonicalFilePath();
    const QString canonicalParent = parent.canonicalFilePath();
    if (root.isEmpty() || canonicalParent.isEmpty())
        return false;
    return canonicalParent == QDir(root).filePath(matched);
}

ScratchCleanup cleanupScratch(const QString &path, const QString &scratchRoot, const QUuid &runUuid)
{
    // Only a path of exactly the right shape is ever touched; a link there is
    // removed as a link, anything else is a real folder to delete without
    // following links.
    if (!isSafeScratchPath(path, scratchRoot, runUuid, /*allowLinkAtPath=*/true))
        return ScratchCleanup::Unsafe;
    if (isLink(QFileInfo(path)))
        return removeLinkItself(path) ? ScratchCleanup::LinkRemoved : ScratchCleanup::Failed;
    return removeTreeNoFollow(path) ? ScratchCleanup::Removed : ScratchCleanup::Failed;
}

} // namespace sb::fs

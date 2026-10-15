// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/fs_util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace sb::fs {

namespace {

bool isLink(const QFileInfo &fi)
{
    return fi.isSymbolicLink() || fi.isJunction();
}

} // namespace

QString scratchRunFolderName(const QUuid &runUuid)
{
    return QStringLiteral("synbench-%1").arg(runUuid.toString(QUuid::WithoutBraces));
}

bool removeTreeNoFollow(const QString &path)
{
    const QFileInfo fi(path);  // QFileInfo describes the link itself (lstat)
    if (isLink(fi)) {
        // Remove the link, never what it points to.
        return fi.isJunction() ? QDir().rmdir(path) : QFile::remove(path);
    }
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

bool isSafeScratchPath(const QString &path, const QString &scratchRoot, const QUuid &runUuid)
{
    const QFileInfo fi(path);
    if (!fi.isAbsolute() || isLink(fi))
        return false;
    const QString name = fi.fileName();
    if (name.isEmpty() || name == QLatin1StringView(".") || name == QLatin1StringView(".."))
        return false;
    const QFileInfo parent(fi.path());
    if (isLink(parent) || parent.fileName() != scratchRunFolderName(runUuid))
        return false;
    const QString root = QFileInfo(scratchRoot).canonicalFilePath();
    const QString canonicalParent = parent.canonicalFilePath();
    if (root.isEmpty() || canonicalParent.isEmpty())
        return false;
    return canonicalParent == QDir(root).filePath(scratchRunFolderName(runUuid));
}

} // namespace sb::fs

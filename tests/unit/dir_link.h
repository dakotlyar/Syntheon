// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QString>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace sb::testing {

// Makes `link` a directory link to `target`: a symbolic link on Unix, a
// junction on Windows (QFile::link would create a .lnk shortcut file there,
// and symlinks need Developer Mode; junctions need neither). Returns false if
// the platform refuses, so tests can QSKIP instead of failing.
inline bool makeDirLink(const QString &target, const QString &link)
{
#ifdef Q_OS_WIN
    QProcess p;
    p.start(QStringLiteral("cmd"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                    QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)});
    if (!p.waitForFinished(15000) || p.exitCode() != 0)
        return false;
    const QFileInfo fi(link);
    return fi.isJunction() || fi.isSymLink();
#else
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0;
#endif
}

} // namespace sb::testing

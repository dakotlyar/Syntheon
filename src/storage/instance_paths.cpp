// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/instance_paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace sb::instance {

QString canonicalFolder(const QString &folder)
{
    const QFileInfo fi(folder);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : canonical;
}

QString key(const QString &folder)
{
    const QByteArray h = QCryptographicHash::hash(canonicalFolder(folder).toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(h.left(16));
}

QString stateDir()
{
#ifdef Q_OS_WIN
    // %LOCALAPPDATA%\SynBench\run, spelled out like the history folder is
    // (AppLocalDataLocation would use whatever the application/organisation
    // names happen to be, which differ in tests and tools).
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        return {};
    const QString dir = QDir(base).filePath(QStringLiteral("SynBench/run"));
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        return {};
    const QString dir = QDir(base).filePath(QStringLiteral("synbench"));
#endif
    if (!QDir().mkpath(dir))
        return {};
#ifdef Q_OS_UNIX
    // Other users must not be able to plant a lock or socket here.
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QFileInfo fi(dir);
    if (fi.ownerId() != uid_t(geteuid()) || (fi.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)))
        return {};
#endif
    return dir;
}

QString lockFilePath(const QString &folder)
{
    const QString dir = stateDir();
    return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("synbench-%1.lock").arg(key(folder)));
}

QString socketName(const QString &folder)
{
#ifdef Q_OS_WIN
    const QByteArray h = QCryptographicHash::hash((qEnvironmentVariable("USERNAME") + QLatin1Char('|') + key(folder)).toUtf8(),
                                                  QCryptographicHash::Sha1).toHex();
    return QStringLiteral("synbench-") + QString::fromLatin1(h.left(16));
#else
    const QString dir = stateDir();
    return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("synbench-%1.sock").arg(key(folder)));
#endif
}

} // namespace sb::instance

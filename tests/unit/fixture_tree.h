// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fixture trees are committed with path characters Windows can't hold
// (: * ? " < > | and %) escaped as %XX, in names and in symlink targets
// (tests/fixtures/capture-linux.sh does this). materializeFixture() rebuilds
// a tree with the real names (e.g. sys/dev/block/259:7) under `dest`, so the
// Linux readers see true sysfs paths. Linux only (symlinks).

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <unistd.h>

namespace sb::testing {

inline QString decodeFixtureName(const QString &s)
{
    QString out;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('%') && i + 2 < s.size()) {
            bool ok = false;
            const int c = s.mid(i + 1, 2).toInt(&ok, 16);
            if (ok) {
                out += QChar(c);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

inline QString decodeFixturePath(const QString &path)
{
    QStringList parts = path.split(QLatin1Char('/'));
    for (QString &p : parts)
        p = decodeFixtureName(p);
    return parts.join(QLatin1Char('/'));
}

// Returns false if anything couldn't be recreated.
inline bool materializeFixture(const QString &encodedRoot, const QString &dest)
{
    const QDir src(encodedRoot);
    QDirIterator it(encodedRoot, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);  // never descends through links
    bool ok = true;
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        const QString target = QDir(dest).filePath(decodeFixturePath(src.relativeFilePath(fi.filePath())));
        QDir().mkpath(QFileInfo(target).path());
        if (fi.isSymLink()) {
            char buf[4096];
            const ssize_t n = ::readlink(QFile::encodeName(fi.filePath()).constData(), buf, sizeof buf - 1);
            if (n < 0) {
                ok = false;
                continue;
            }
            const QString link = decodeFixturePath(QString::fromLocal8Bit(buf, n));
            ok = ::symlink(QFile::encodeName(link).constData(), QFile::encodeName(target).constData()) == 0 && ok;
        } else if (fi.isDir()) {
            ok = QDir().mkpath(target) && ok;
        } else {
            ok = QFile::copy(fi.filePath(), target) && ok;
        }
    }
    return ok;
}

} // namespace sb::testing

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moving people's history and settings across the rename. Every path here is a
// QTemporaryDir or a Qt test-mode location, and on Unix the runtime folder is a
// QTemporaryDir too (initTestCase), so the old build's lock folder is never the
// real one even outside CTest.
#include "storage/instance_paths.h"
#include "storage/legacy_migration.h"
#include "storage/legacy_names.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace sb;

namespace {

// The folder the old build keeps its locks in.
QString legacyLockDir()
{
#ifdef Q_OS_WIN
    return QDir(legacy::defaultDataDir()).filePath(QStringLiteral("run"));
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath(QString::fromLatin1(legacy::kRuntimeDirName));
#endif
}

} // namespace

class TestLegacyMigration : public QObject
{
    Q_OBJECT

    QTemporaryDir m_runtime;

private slots:
    void initTestCase()
    {
        QVERIFY(m_runtime.isValid());
#ifdef Q_OS_UNIX
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(m_runtime.path()));
#endif
    }

    void init()
    {
        // Test-mode locations survive between test functions: start each one clean.
        // Never without test mode: this is a delete of the old default folder.
        QVERIFY(legacy::defaultDataDir().contains(QLatin1StringView("qttest"), Qt::CaseInsensitive));
        QVERIFY(QDir(legacy::defaultDataDir()).removeRecursively());
        QVERIFY(QDir(legacyLockDir()).removeRecursively());
    }

    void legacyPathsPointAtTheOldNames()
    {
        QVERIFY(legacy::defaultDataDir().endsWith(QLatin1StringView("/SynBench")));
        QVERIFY(legacy::settingsFile().endsWith(QLatin1StringView("/SynBench/settings.ini")));
    }

    void legacyLockPathNeverCreatesTheFolder()
    {
        QTemporaryDir history;
        QVERIFY(legacy::lockFilePath(history.path()).isEmpty());
        QVERIFY(!QFileInfo::exists(legacyLockDir()));
        QVERIFY(!QFileInfo::exists(legacy::defaultDataDir()));

        QVERIFY(QDir().mkpath(legacyLockDir()));
        const QString expected = QStringLiteral("synbench-%1.lock").arg(instance::key(history.path()));
        QCOMPARE(legacy::lockFilePath(history.path()), QDir(legacyLockDir()).filePath(expected));
    }
};

QTEST_GUILESS_MAIN(TestLegacyMigration)
#include "tst_legacy_migration.moc"

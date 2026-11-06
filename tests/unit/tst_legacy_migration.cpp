// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moving people's history and settings across the rename. Every path here is a
// QTemporaryDir or a Qt test-mode location, and on Unix the runtime folder is a
// QTemporaryDir too (initTestCase), so the old build's lock folder is never the
// real one even outside CTest.
#include "storage/instance_paths.h"
#include "storage/legacy_migration.h"
#include "storage/legacy_names.h"
#include "storage/settings_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
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

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// An old settings.ini, as the old build's SettingsStore wrote it.
void writeLegacyIni(const QString &path, const QString &dataLocation)
{
    SettingsStore s(path);
    s.setDataLocation(dataLocation);
    s.setLayout(QStringLiteral("tabs"));
    s.setTheme(QStringLiteral("dark"));
    s.setFirstRunDone(true);
    QVERIFY(s.sync());
}

QVariantMap allValues(const QString &ini)
{
    QSettings s(ini, QSettings::IniFormat);
    QVariantMap m;
    for (const QString &key : s.allKeys())
        m.insert(key, s.value(key));
    return m;
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

    // M13
    void settingsCopied()
    {
        QTemporaryDir old, fresh, custom;
        const QString legacyIni = old.filePath(QStringLiteral("settings.ini"));
        const QString newIni = fresh.filePath(QStringLiteral("Syntheon/settings.ini"));
        writeLegacyIni(legacyIni, custom.path());
        const QByteArray before = readAll(legacyIni);

        const auto s = legacy::migrateSettings(legacyIni, newIni, old.filePath(QStringLiteral("default")));
        QCOMPARE(s.status, legacy::SettingsStatus::Migrated);
        QCOMPARE(allValues(newIni), allValues(legacyIni));
        QCOMPARE(SettingsStore(newIni).dataLocation(), custom.path());  // a custom path is carried over
        QCOMPARE(readAll(newIni), before);
        QCOMPARE(readAll(legacyIni), before);
        QVERIFY(!QFileInfo::exists(newIni + QStringLiteral(".migrating")));
    }

    void existingNewSettingsAreNotTouched()
    {
        QTemporaryDir dir;
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, {});
        writeFile(newIni, "[ui]\ntheme=light\n");
        const QByteArray before = readAll(newIni);
        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, {}).status, legacy::SettingsStatus::NothingToDo);
        QCOMPARE(readAll(newIni), before);
    }

    void noOldSettingsNothingHappens()
    {
        QTemporaryDir dir;
        const QString newIni = dir.filePath(QStringLiteral("sub/new.ini"));
        QCOMPARE(legacy::migrateSettings(dir.filePath(QStringLiteral("old.ini")), newIni, {}).status,
                 legacy::SettingsStatus::NothingToDo);
        QVERIFY(!QFileInfo::exists(QFileInfo(newIni).path()));
    }

    void staleSettingsTempFileIsReplaced()
    {
        QTemporaryDir dir;
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, {});
        writeFile(newIni + QStringLiteral(".migrating"), "half a file");
        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, {}).status, legacy::SettingsStatus::Migrated);
        QCOMPARE(readAll(newIni), readAll(legacyIni));
        QVERIFY(!QFileInfo::exists(newIni + QStringLiteral(".migrating")));
    }

    // M14
    void oldDefaultLocationIsDropped_data()
    {
        QTest::addColumn<QString>("suffix");
        QTest::newRow("exact") << QString();
        QTest::newRow("trailing slash") << QStringLiteral("/");
    }
    void oldDefaultLocationIsDropped()
    {
        QFETCH(QString, suffix);
        QTemporaryDir dir;
        const QString oldDefault = dir.filePath(QStringLiteral("SynBench"));
        QVERIFY(QDir().mkpath(oldDefault));
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, oldDefault + suffix);
        const QByteArray before = readAll(legacyIni);

        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, oldDefault).status, legacy::SettingsStatus::Migrated);
        SettingsStore migrated(newIni);
        QVERIFY(migrated.dataLocation().isEmpty());
        QCOMPARE(migrated.theme(), QStringLiteral("dark"));  // the rest came across
        QCOMPARE(migrated.layout(), QStringLiteral("tabs"));
        QVERIFY(migrated.firstRunDone());
        QCOMPARE(readAll(legacyIni), before);  // removed from the new file only
    }

    // M15
    void windowsPathsCompareCorrectly()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows paths");
#else
        QTemporaryDir dir;
        const QString oldDefault = dir.filePath(QStringLiteral("SynBench"));
        QVERIFY(QDir().mkpath(oldDefault));
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini")), newIni = dir.filePath(QStringLiteral("new.ini"));
        writeLegacyIni(legacyIni, QDir::toNativeSeparators(oldDefault).toUpper());
        QCOMPARE(legacy::migrateSettings(legacyIni, newIni, oldDefault).status, legacy::SettingsStatus::Migrated);
        QVERIFY(SettingsStore(newIni).dataLocation().isEmpty());
#endif
    }

    // M16
    void unusableNewSettingsPathFails()
    {
        QTemporaryDir dir;
        const QString legacyIni = dir.filePath(QStringLiteral("old.ini"));
        writeLegacyIni(legacyIni, {});
        const QByteArray before = readAll(legacyIni);
        const QString blocker = dir.filePath(QStringLiteral("a-file"));
        writeFile(blocker, "not a folder");
        const QString newIni = blocker + QStringLiteral("/settings.ini");

        const auto s = legacy::migrateSettings(legacyIni, newIni, {});
        QCOMPARE(s.status, legacy::SettingsStatus::Failed);
        QVERIFY(!s.error.isEmpty());
        QVERIFY(!QFileInfo::exists(newIni));
        QVERIFY(!QFileInfo::exists(newIni + QStringLiteral(".migrating")));
        QCOMPARE(readAll(blocker), QByteArray("not a folder"));
        QCOMPARE(readAll(legacyIni), before);
    }
};

QTEST_GUILESS_MAIN(TestLegacyMigration)
#include "tst_legacy_migration.moc"

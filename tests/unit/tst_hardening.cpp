// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/process_hardening.h"

#include <QCoreApplication>
#include <QDir>
#include <QTest>

using namespace sb;

class TestHardening : public QObject
{
    Q_OBJECT

private slots:
    // The environment a caller could use to make an elevated Syntheon load
    // plugins, QML or a style config from somewhere it controls.
    void scrubRemovesTheLoadPathVariablesAndOnlyThose()
    {
        const QByteArrayList names = qtEnvironmentVariablesToScrub();
        for (const char *must : {"QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH",
                                 "QT_QUICK_CONTROLS_CONF", "QT_QPA_GENERIC_PLUGINS"})
            QVERIFY2(names.contains(must), must);
        QVERIFY(!names.contains("QT_QPA_PLATFORM"));  // choosing a platform is harmless

        for (const QByteArray &n : names)
            qputenv(n.constData(), "/tmp/evil");
        qputenv("QT_QPA_PLATFORM", "offscreen");

        const QStringList removed = scrubQtEnvironment();
        QCOMPARE(removed.size(), names.size());
        for (const QByteArray &n : names)
            QVERIFY2(!qEnvironmentVariableIsSet(n.constData()), n.constData());
        QCOMPARE(qgetenv("QT_QPA_PLATFORM"), QByteArray("offscreen"));
        QVERIFY(scrubQtEnvironment().isEmpty());  // nothing left to remove
    }

    void pinningReplacesTheLibraryPaths()
    {
        const QStringList before = QCoreApplication::libraryPaths();
        const QString only = QDir::tempPath();
        pinLibraryPaths({only});
        QCOMPARE(QCoreApplication::libraryPaths(), QStringList{QDir::cleanPath(only)});
        QCoreApplication::setLibraryPaths(before);
    }

    void executableFolderIsWhereTheProgramLives()
    {
        QCOMPARE(QDir::cleanPath(executableFolder()), QDir::cleanPath(QCoreApplication::applicationDirPath()));
    }
};

QTEST_MAIN(TestHardening)
#include "tst_hardening.moc"

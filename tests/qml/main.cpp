// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/fonts.h"
#include "core/result.h"
#include "fixtures.h"
#include "storage/run_repository.h"
#include "tests/common/stub_clock.h"

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtQuickTest>

#include <memory>

// Real controllers on a temporary settings file and history (no elevation, no
// helper), registered as the QML singletons before any test file loads. Two
// runs are seeded so Clear history has something to clear.
class Setup : public QObject
{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {
        sb::loadBundledFonts();
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        m_dir = std::make_unique<QTemporaryDir>();
        sb::AppRuntime::Options o;
        o.settingsFile = m_dir->filePath(QStringLiteral("settings.ini"));
        o.dataFolder = m_dir->filePath(QStringLiteral("data"));
        o.smoke = true;  // no helper, no polkit
        m_runtime = std::make_unique<sb::AppRuntime>(o);
        m_runtime->storageController().open();
        sb::RunRepository repo(m_runtime->storage().connection());
        for (int i = 0; i < 2; ++i) {
            sb::RunRecord r;
            r.uuid = QUuid::createUuid();
            r.status = sb::RunStatus::Completed;
            r.startedAt = QDateTime::currentDateTimeUtc();
            repo.saveRun(r);
        }
        m_runtime->registerQml();
        Fixtures::instance = &m_fixtures;
        // Stubs at a fast time scale; probes and the (elevation-free) gate.
        qputenv("SYNBENCH_STUB_SCALE", "0.02");
        m_runtime->app().start();
        m_runtime->run().refreshProbes();
    }

    void qmlEngineAvailable(QQmlEngine *) {}

    // The controllers own QObjects, timers and a process; they must be gone
    // while the application object still exists (this runs after the last
    // test, before QtQuickTest destroys it).
    void cleanupTestCase()
    {
        m_runtime.reset();
        m_dir.reset();
    }

private:
    Fixtures m_fixtures;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<sb::AppRuntime> m_runtime;
};

QUICK_TEST_MAIN_WITH_SETUP(synbench, Setup)

#include "main.moc"

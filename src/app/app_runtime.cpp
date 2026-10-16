// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"

#include <QDir>
#include <QtQml>

namespace sb {

AppRuntime::Options AppRuntime::optionsFromEnvironment()
{
    Options o;
    o.smoke = qEnvironmentVariableIntValue("SYNBENCH_SMOKE") != 0;
    o.noElevation = o.smoke || !qEnvironmentVariable("SYNBENCH_TOUR").isEmpty();
    return o;
}

AppRuntime::AppRuntime(Options options, QObject *parent) : QObject(parent), m_options(std::move(options))
{
    QString ini = m_options.settingsFile;
    QString data = m_options.dataFolder;
    // The tour needs a throwaway history too, never the user's.
    if (m_options.smoke || (m_options.noElevation && m_options.settingsFile.isEmpty())) {
        m_smokeDir = std::make_unique<QTemporaryDir>();
        if (ini.isEmpty())
            ini = m_smokeDir->filePath(QStringLiteral("settings.ini"));
        if (data.isEmpty())
            data = m_smokeDir->filePath(QStringLiteral("data"));
    }
    m_store = ini.isEmpty() ? std::make_unique<SettingsStore>() : std::make_unique<SettingsStore>(ini);
    if (!data.isEmpty()) {
        QDir().mkpath(data);
        m_store->setDataLocation(data);
    }
    m_storage = std::make_unique<Storage>(*m_store);
    m_settings = std::make_unique<SettingsController>(*m_store);
    m_storageController = std::make_unique<StorageController>(*m_storage, *m_store);
    m_app = std::make_unique<AppController>();
    m_app->setElevationDisabled(m_options.smoke || m_options.noElevation);
    m_run = std::make_unique<RunController>(*m_storage, *m_storageController, *m_app);
    m_results = std::make_unique<ResultsController>(*m_run, *m_storage, *m_storageController);
    m_startup = std::make_unique<StartupController>(*m_run, *m_storageController, *m_settings);
    m_history = std::make_unique<HistoryModel>(*m_run, *m_storage, *m_storageController);
    m_compare = std::make_unique<CompareController>(*m_run, *m_storage, *m_storageController);
}

AppRuntime::~AppRuntime() = default;

void AppRuntime::registerQml()
{
    qmlRegisterSingletonInstance("SynBench", 1, 0, "Settings", m_settings.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "AppController", m_app.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "StorageController", m_storageController.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "RunController", m_run.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "Results", m_results.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "Startup", m_startup.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "History", m_history.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "Compare", m_compare.get());
}

void AppRuntime::start()
{
    m_storageController->open();
    m_startup->recover();  // before any UI: cut-short runs become interrupted or recovered
    m_results->openRun({});  // the newest saved run, if any
    m_history->reload();
    m_app->start();
    if (m_options.smoke)
        m_run->runSmoke();
    else
        m_run->refreshProbes();
}

} // namespace sb

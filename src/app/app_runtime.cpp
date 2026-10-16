// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"

#include <QDir>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcRuntime, "synbench.runtime")
#include <QtQml>

namespace sb {

AppRuntime::Options AppRuntime::optionsFromEnvironment()
{
    Options o;
    o.smoke = qEnvironmentVariableIntValue("SYNBENCH_SMOKE") != 0;
    o.noElevation = o.smoke || !qEnvironmentVariable("SYNBENCH_TOUR").isEmpty();
    // Developer hooks for tests that start the real executable.
    o.settingsFile = qEnvironmentVariable("SYNBENCH_SETTINGS_FILE");
    o.dataFolder = qEnvironmentVariable("SYNBENCH_DATA_DIR");
    if (qEnvironmentVariableIntValue("SYNBENCH_NO_ELEVATION") != 0)
        o.noElevation = true;
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

bool AppRuntime::claimInstance()
{
    if (m_claimed)
        return true;
    if (m_redirected)
        return false;
    m_storageController->open();
    if (m_storageController->startupKind() == QLatin1StringView("inuse")) {
        m_redirected = true;
        const bool answered = SingleInstance::requestFocus(m_storage->dataDir());
        qCInfo(lcRuntime) << "another SynBench has this history; asked it to come to the front:" << answered;
        return false;
    }
    m_claimed = true;
    m_instance.listen(m_storage->dataDir());
    // The socket follows the history folder, like the lock does.
    connect(m_storageController.get(), &StorageController::locationChanged, this,
            [this] { m_instance.listen(m_storage->dataDir()); });
    return true;
}

void AppRuntime::start()
{
    if (!claimInstance())
        return;
    m_results->openRun({});  // the newest saved run, if any
    m_history->reload();
    // The storage gate comes first: no administrator prompt (and no helper)
    // until the history has opened. Until then QML shows the storage dialog.
    const auto begin = [this] {
        if (m_begun)
            return;
        m_begun = true;
        m_app->start();
        if (m_options.smoke)
            m_run->runSmoke();
        else
            m_run->refreshProbes();
    };
    if (m_storageController->startupKind() == QLatin1StringView("none")) {
        begin();
    } else {
        connect(m_storageController.get(), &StorageController::startupProblemChanged, this, [this, begin] {
            if (m_storageController->startupKind() == QLatin1StringView("none"))
                begin();
        });
    }
}

} // namespace sb

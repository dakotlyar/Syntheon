// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"

#include "app/qml_singletons.h"
#include "storage/legacy_migration.h"

#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcRuntime, "syntheon.runtime")
#include <QtQml>

namespace sb {

AppRuntime::Options AppRuntime::optionsFromEnvironment()
{
    Options o;
#ifdef SYNTHEON_DEV_HOOKS
    // Developer hooks only; a packaged build reads none of these.
    o.smoke = qEnvironmentVariableIntValue("SYNTHEON_SMOKE") != 0;
    o.noElevation = o.smoke || !qEnvironmentVariable("SYNTHEON_TOUR").isEmpty();
    // Developer hooks for tests that start the real executable.
    o.settingsFile = qEnvironmentVariable("SYNTHEON_SETTINGS_FILE");
    o.dataFolder = qEnvironmentVariable("SYNTHEON_DATA_DIR");
    if (qEnvironmentVariableIntValue("SYNTHEON_NO_ELEVATION") != 0)
        o.noElevation = true;
#endif
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
    // Settings from before the rename, ahead of anything that reads the new file.
    // Only for the real per-user settings: the smoke run, the tour and the dev
    // hooks all set one of these, and never come here.
    QString fallbackIni;
    if (ini.isEmpty() && data.isEmpty()) {
        SettingsStore probe;  // never synced: only asks where the new file goes
        const auto s = legacy::migrateSettings(legacy::settingsFile(), probe.fileName(), legacy::defaultDataDir());
        if (s.status == legacy::SettingsStatus::Failed) {
            qCWarning(lcRuntime) << s.error;
            fallbackIni = legacy::settingsFile();
        }
    }
    if (!fallbackIni.isEmpty())
        ini = fallbackIni;
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

AppRuntime::~AppRuntime()
{
    // No engine may be handed a destroyed object (unit tests make several runtimes).
    const auto release = [](auto *&slot, const void *mine) { if (slot == mine) slot = nullptr; };
    release(qml::Instance<SettingsController>::object, m_settings.get());
    release(qml::Instance<AppController>::object, m_app.get());
    release(qml::Instance<StorageController>::object, m_storageController.get());
    release(qml::Instance<RunController>::object, m_run.get());
    release(qml::Instance<ResultsController>::object, m_results.get());
    release(qml::Instance<StartupController>::object, m_startup.get());
    release(qml::Instance<HistoryModel>::object, m_history.get());
    release(qml::Instance<CompareController>::object, m_compare.get());
}

void AppRuntime::registerQml()
{
    // The types are declared to the module at build time (qml_singletons.h);
    // this hands over the objects the engines will get.
    qml::Instance<SettingsController>::object = m_settings.get();
    qml::Instance<AppController>::object = m_app.get();
    qml::Instance<StorageController>::object = m_storageController.get();
    qml::Instance<RunController>::object = m_run.get();
    qml::Instance<ResultsController>::object = m_results.get();
    qml::Instance<StartupController>::object = m_startup.get();
    qml::Instance<HistoryModel>::object = m_history.get();
    qml::Instance<CompareController>::object = m_compare.get();
}

bool AppRuntime::claimInstance()
{
    if (m_claimed)
        return true;
    if (m_redirected)
        return false;
    m_storageController->open();
    if (m_storageController->startupKind() == QLatin1StringView("inuse")) {
        // Somebody holds the lock. Believe it only once it answers: a copy
        // that is just starting needs a moment to open its socket.
        QElapsedTimer waited;
        waited.start();
        do {
            if (SingleInstance::requestFocus(m_storage->dataDir(), 300)) {
                m_redirected = true;
                qCInfo(lcRuntime) << "another Syntheon has this history and answered; this copy exits";
                return false;
            }
            QThread::msleep(100);
        } while (waited.elapsed() < m_options.focusWaitMs);
        // Nobody answered: a crash left the lock (or the copy is hung). The UI
        // offers Take over / Quit (start() waits for it).
        qCWarning(lcRuntime) << "the history is locked but no copy answered";
    }
    m_claimed = true;
    // The socket follows the history folder, like the lock does.
    connect(m_storageController.get(), &StorageController::locationChanged, this, [this] { refreshInstanceSocket(); });
    refreshInstanceSocket();
    return true;
}

void AppRuntime::refreshInstanceSocket()
{
    if (!m_storage->isOpen())
        return;
    const bool ok = m_instance.listen(m_storage->dataDir());
    m_startup->setInstanceWarning(ok ? QString() : m_instance.error());
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

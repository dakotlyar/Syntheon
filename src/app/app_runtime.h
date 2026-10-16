// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/controllers/app_controller.h"
#include "app/controllers/compare_controller.h"
#include "app/controllers/results_controller.h"
#include "app/controllers/startup_controller.h"
#include "app/controllers/run_controller.h"
#include "app/controllers/settings_controller.h"
#include "app/controllers/storage_controller.h"
#include "app/models/history_model.h"
#include "storage/settings_store.h"
#include "storage/storage.h"

#include <QObject>
#include <QTemporaryDir>

#include <memory>

namespace sb {

// Owns the app's backend objects in dependency order and exposes the QML
// singletons. main() builds one; the unit tests build their own with a
// temporary settings file and data folder.
class AppRuntime : public QObject
{
    Q_OBJECT

public:
    struct Options {
        // SYNBENCH_SMOKE=1: a temporary settings file and history, no
        // elevation (there is no polkit agent under xvfb), stubs at a fast
        // time scale. Used by CI.
        bool smoke = false;
        // No polkit prompt, no helper: set by the developer screenshot tour
        // (SYNBENCH_TOUR) and by smoke runs, so neither ever asks for a password.
        bool noElevation = false;
        QString settingsFile;  // empty = the per-user settings.ini
        QString dataFolder;    // empty = from the settings
    };

    explicit AppRuntime(Options options, QObject *parent = nullptr);
    ~AppRuntime() override;

    static Options optionsFromEnvironment();

    // Registers the controllers as the QML singletons Settings, AppController
    // and StorageController (module SynBench).
    void registerQml();
    // Opens the history and starts the gates (storage, elevation).
    void start();

    SettingsStore &settingsStore() { return *m_store; }
    Storage &storage() { return *m_storage; }
    SettingsController &settings() { return *m_settings; }
    StorageController &storageController() { return *m_storageController; }
    AppController &app() { return *m_app; }
    RunController &run() { return *m_run; }
    ResultsController &results() { return *m_results; }
    StartupController &startup() { return *m_startup; }
    HistoryModel &history() { return *m_history; }
    CompareController &compare() { return *m_compare; }
    bool smoke() const { return m_options.smoke; }

private:
    Options m_options;
    std::unique_ptr<QTemporaryDir> m_smokeDir;
    std::unique_ptr<SettingsStore> m_store;
    std::unique_ptr<Storage> m_storage;
    std::unique_ptr<SettingsController> m_settings;
    std::unique_ptr<StorageController> m_storageController;
    std::unique_ptr<AppController> m_app;
    std::unique_ptr<RunController> m_run;
    std::unique_ptr<ResultsController> m_results;
    std::unique_ptr<StartupController> m_startup;
    std::unique_ptr<HistoryModel> m_history;
    std::unique_ptr<CompareController> m_compare;
};

} // namespace sb

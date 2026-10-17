// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/controllers/app_controller.h"
#include "app/controllers/compare_controller.h"
#include "app/controllers/results_controller.h"
#include "app/controllers/run_controller.h"
#include "app/controllers/settings_controller.h"
#include "app/controllers/startup_controller.h"
#include "app/controllers/storage_controller.h"
#include "app/models/history_model.h"

#include <QJSEngine>
#include <QQmlEngine>

// The app's C++ singletons, declared to the QML module SynBench at build time
// (QML_FOREIGN), so qmlcachegen and qmllint know their types: a typo in
// "RunController.selectedCount" is a lint warning, not a silent undefined.
//
// The objects themselves are AppRuntime's (it creates them before the engine
// and outlives it). AppRuntime::registerQml() hands them over through
// Instance<T>::object; each engine that asks gets the same object, owned by
// C++. The Qt Quick tests create one engine per test file, which is why there
// is no one-engine assertion here.
namespace sb::qml {

template <typename T>
struct Instance {
    inline static T *object = nullptr;

    static T *create(QQmlEngine *, QJSEngine *engine)
    {
        Q_ASSERT_X(object, "SynBench QML singleton", "AppRuntime::registerQml() must run before the engine loads QML");
        Q_ASSERT(engine->thread() == object->thread());
        QJSEngine::setObjectOwnership(object, QJSEngine::CppOwnership);
        return object;
    }
};

struct SettingsForeign : Instance<SettingsController> {
    Q_GADGET
    QML_FOREIGN(sb::SettingsController)
    QML_NAMED_ELEMENT(Settings)
    QML_SINGLETON
};

struct AppControllerForeign : Instance<AppController> {
    Q_GADGET
    QML_FOREIGN(sb::AppController)
    QML_NAMED_ELEMENT(AppController)
    QML_SINGLETON
};

struct StorageControllerForeign : Instance<StorageController> {
    Q_GADGET
    QML_FOREIGN(sb::StorageController)
    QML_NAMED_ELEMENT(StorageController)
    QML_SINGLETON
};

struct RunControllerForeign : Instance<RunController> {
    Q_GADGET
    QML_FOREIGN(sb::RunController)
    QML_NAMED_ELEMENT(RunController)
    QML_SINGLETON
};

struct ResultsForeign : Instance<ResultsController> {
    Q_GADGET
    QML_FOREIGN(sb::ResultsController)
    QML_NAMED_ELEMENT(Results)
    QML_SINGLETON
};

struct StartupForeign : Instance<StartupController> {
    Q_GADGET
    QML_FOREIGN(sb::StartupController)
    QML_NAMED_ELEMENT(Startup)
    QML_SINGLETON
};

struct HistoryForeign : Instance<HistoryModel> {
    Q_GADGET
    QML_FOREIGN(sb::HistoryModel)
    QML_NAMED_ELEMENT(History)
    QML_SINGLETON
};

struct CompareForeign : Instance<CompareController> {
    Q_GADGET
    QML_FOREIGN(sb::CompareController)
    QML_NAMED_ELEMENT(Compare)
    QML_SINGLETON
};

} // namespace sb::qml

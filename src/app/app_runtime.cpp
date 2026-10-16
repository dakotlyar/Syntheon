// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"

#include <QDir>
#include <QtQml>

namespace sb {

AppRuntime::Options AppRuntime::optionsFromEnvironment()
{
    Options o;
    o.smoke = qEnvironmentVariableIntValue("SYNBENCH_SMOKE") != 0;
    return o;
}

AppRuntime::AppRuntime(Options options, QObject *parent) : QObject(parent), m_options(std::move(options))
{
    QString ini = m_options.settingsFile;
    QString data = m_options.dataFolder;
    if (m_options.smoke) {
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
    m_app->setElevationDisabled(m_options.smoke);
}

AppRuntime::~AppRuntime() = default;

void AppRuntime::registerQml()
{
    qmlRegisterSingletonInstance("SynBench", 1, 0, "Settings", m_settings.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "AppController", m_app.get());
    qmlRegisterSingletonInstance("SynBench", 1, 0, "StorageController", m_storageController.get());
}

void AppRuntime::start()
{
    m_storageController->open();
    m_app->start();
}

} // namespace sb

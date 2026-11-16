// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/settings_store.h"

#include <QStringList>
#include <QVariantMap>

namespace sb {

namespace {
const QString kDataLocation = QStringLiteral("storage/dataLocation");
const QString kLayout = QStringLiteral("ui/layout");
const QString kTheme = QStringLiteral("ui/theme");
const QString kNpuModelsBaseUrl = QStringLiteral("npu/modelsBaseUrl");
const QString kFirstRunDone = QStringLiteral("app/firstRunDone");
} // namespace

SettingsStore::SettingsStore()
    : m_settings(std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Syntheon"),
                                             QStringLiteral("settings")))
{
}

SettingsStore::SettingsStore(const QString &iniPath)
    : m_settings(std::make_unique<QSettings>(iniPath, QSettings::IniFormat))
{
}

QString SettingsStore::fileName() const
{
    return m_settings->fileName();
}

QString SettingsStore::dataLocation() const
{
    return m_settings->value(kDataLocation).toString();
}

void SettingsStore::setDataLocation(const QString &folder)
{
    if (folder.isEmpty())
        m_settings->remove(kDataLocation);
    else
        m_settings->setValue(kDataLocation, folder);
}

QString SettingsStore::choice(const QString &key, const QStringList &allowed) const
{
    const QString v = m_settings->value(key).toString();
    return allowed.contains(v) ? v : allowed.first();
}

QString SettingsStore::layout() const
{
    return choice(kLayout, {QStringLiteral("rail"), QStringLiteral("tabs")});
}

void SettingsStore::setLayout(const QString &layout)
{
    m_settings->setValue(kLayout, layout);
}

QString SettingsStore::theme() const
{
    return choice(kTheme, {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")});
}

void SettingsStore::setTheme(const QString &theme)
{
    m_settings->setValue(kTheme, theme);
}

QString SettingsStore::npuModelsBaseUrl() const
{
    return m_settings->value(kNpuModelsBaseUrl).toString();
}

void SettingsStore::setNpuModelsBaseUrl(const QString &url)
{
    if (url.isEmpty())
        m_settings->remove(kNpuModelsBaseUrl);
    else
        m_settings->setValue(kNpuModelsBaseUrl, url);
}

bool SettingsStore::firstRunDone() const
{
    return m_settings->value(kFirstRunDone, false).toBool();
}

void SettingsStore::setFirstRunDone(bool done)
{
    m_settings->setValue(kFirstRunDone, done);
}

bool SettingsStore::sync()
{
    m_settings->sync();
    if (m_settings->status() == QSettings::NoError)
        return true;

    // F1: QSettings::status() is sticky; once a write fails it never reports
    // success again. Rebuild the object from the current (in-memory) values
    // and retry, so a later sync can succeed once the file is writable.
    QVariantMap values;
    for (const QString &key : m_settings->allKeys())
        values.insert(key, m_settings->value(key));
    const QString file = m_settings->fileName();
    m_settings = std::make_unique<QSettings>(file, QSettings::IniFormat);
    for (const QString &key : m_settings->allKeys()) {
        if (!values.contains(key))
            m_settings->remove(key);  // removed in memory, still on disk
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        m_settings->setValue(it.key(), it.value());
    m_settings->sync();
    return m_settings->status() == QSettings::NoError;
}

} // namespace sb

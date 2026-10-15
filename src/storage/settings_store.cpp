#include "storage/settings_store.h"

#include <QStringList>

namespace sb {

namespace {
const QString kDataLocation = QStringLiteral("storage/dataLocation");
const QString kLayout = QStringLiteral("ui/layout");
const QString kTheme = QStringLiteral("ui/theme");
const QString kFirstRunDone = QStringLiteral("app/firstRunDone");
} // namespace

SettingsStore::SettingsStore()
    : m_settings(std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("SynBench"),
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
    return m_settings->status() == QSettings::NoError;
}

} // namespace sb

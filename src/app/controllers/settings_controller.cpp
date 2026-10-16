// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/settings_controller.h"

#include "storage/settings_store.h"

namespace sb {

SettingsController::SettingsController(SettingsStore &store, QObject *parent) : QObject(parent), m_store(store) {}

QString SettingsController::layout() const
{
    return m_store.layout();
}

QString SettingsController::theme() const
{
    return m_store.theme();
}

bool SettingsController::firstRunDone() const
{
    return m_store.firstRunDone();
}

void SettingsController::setLayout(const QString &layout)
{
    if (layout != QLatin1StringView("rail") && layout != QLatin1StringView("tabs"))
        return;
    if (layout == m_store.layout())
        return;
    m_store.setLayout(layout);
    m_store.sync();
    emit layoutChanged();
}

void SettingsController::setTheme(const QString &theme)
{
    if (theme != QLatin1StringView("system") && theme != QLatin1StringView("light") && theme != QLatin1StringView("dark"))
        return;
    if (theme == m_store.theme())
        return;
    m_store.setTheme(theme);
    m_store.sync();
    emit themeChanged();
}

void SettingsController::setFirstRunDone(bool done)
{
    if (done == m_store.firstRunDone())
        return;
    m_store.setFirstRunDone(done);
    m_store.sync();
    emit firstRunDoneChanged();
}

} // namespace sb

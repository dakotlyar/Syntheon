// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

namespace sb {

class SettingsStore;

// QML singleton "Settings": navigation layout and theme, persisted in
// settings.ini (P10). Both change live. The theme is set only here (no title
// bar button).
class SettingsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY layoutChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(bool firstRunDone READ firstRunDone WRITE setFirstRunDone NOTIFY firstRunDoneChanged)

public:
    explicit SettingsController(SettingsStore &store, QObject *parent = nullptr);

    QString layout() const;  // "rail" | "tabs"
    QString theme() const;   // "system" | "light" | "dark"
    bool firstRunDone() const;

    void setLayout(const QString &layout);
    void setTheme(const QString &theme);
    void setFirstRunDone(bool done);

signals:
    void layoutChanged();
    void themeChanged();
    void firstRunDoneChanged();

private:
    SettingsStore &m_store;
};

} // namespace sb

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSettings>
#include <QString>

#include <memory>

namespace sb {

// Settings live in settings.ini via QSettings (IniFormat), not in the
// database (P10): the data location is itself a setting, so it can't live
// inside the database it points to.
//   Windows: %APPDATA%\Syntheon\settings.ini
//   Linux:   ~/.config/Syntheon/settings.ini
class SettingsStore
{
public:
    // The per-user default file above.
    SettingsStore();
    // An explicit file (tests, portable setups).
    explicit SettingsStore(const QString &iniPath);

    QString fileName() const;

    // Empty = the default data location.
    QString dataLocation() const;
    void setDataLocation(const QString &folder);

    // "rail" (default) | "tabs"
    QString layout() const;
    void setLayout(const QString &layout);

    // "system" (default) | "light" | "dark"
    QString theme() const;
    void setTheme(const QString &theme);

    // The NPU model-pack base URL (Phase 4c, D2): "https://…" or "file:///…".
    // Empty = not configured; the pack download is then unavailable, and the
    // test reports the model pack as missing (design §6.2).
    QString npuModelsBaseUrl() const;
    void setNpuModelsBaseUrl(const QString &url);

    bool firstRunDone() const;
    void setFirstRunDone(bool done);

    // Writes pending changes to disk; false if the file couldn't be written.
    bool sync();

private:
    QString choice(const QString &key, const QStringList &allowed) const;

    std::unique_ptr<QSettings> m_settings;
};

} // namespace sb

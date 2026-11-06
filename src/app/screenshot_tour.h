// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QQuickWindow;

namespace sb {

class AppRuntime;

// Developer tool, not a feature: SYNTHEON_TOUR=<folder> walks the sections in
// light and dark and saves a PNG of the window for each ("01-run-light.png"),
// then quits. Works on the offscreen platform, so no one has to click through
// the app to see that every screen renders. The sections come from
// SYNTHEON_TOUR_SECTIONS (comma separated, default: all).
class ScreenshotTour : public QObject
{
    Q_OBJECT

public:
    ScreenshotTour(QQuickWindow *window, AppRuntime &runtime, QString folder, QObject *parent = nullptr);
    void start();

private:
    void step();
    void seedRuns(int n);
    void seedBatteryRun();
    void stage();  // the per-screenshot hooks: log dialog, scroll, popup
    void openDialog(const QString &name);
    void prepare();

    QQuickWindow *m_window;
    AppRuntime &m_runtime;
    QString m_folder;
    QStringList m_sections;
    int m_index = 0;
};

} // namespace sb

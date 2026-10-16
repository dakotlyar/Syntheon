// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/screenshot_tour.h"

#include "app/app_runtime.h"
#include "app/controllers/run_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QQuickWindow>
#include <QTimer>

namespace sb {

ScreenshotTour::ScreenshotTour(QQuickWindow *window, AppRuntime &runtime, QString folder, QObject *parent)
    : QObject(parent), m_window(window), m_runtime(runtime), m_folder(std::move(folder))
{
    const QString wanted = qEnvironmentVariable("SYNBENCH_TOUR_SECTIONS");
    m_sections = wanted.isEmpty()
                     ? QStringList{QStringLiteral("run"), QStringLiteral("progress"), QStringLiteral("results"),
                                   QStringLiteral("history"), QStringLiteral("compare"), QStringLiteral("system"),
                                   QStringLiteral("settings")}
                     : wanted.split(QLatin1Char(','), Qt::SkipEmptyParts);
    QDir().mkpath(m_folder);
}

void ScreenshotTour::start()
{
    QTimer::singleShot(1500, this, &ScreenshotTour::prepare);  // let the first frame, system info and fonts settle
}

// SYNBENCH_TOUR_SELECT=cpu,battery ticks those tests first; SYNBENCH_TOUR_TAB
// picks the option tab to show.
void ScreenshotTour::prepare()
{
    for (const QString &id : qEnvironmentVariable("SYNBENCH_TOUR_SELECT").split(QLatin1Char(','), Qt::SkipEmptyParts))
        m_runtime.run().selection().setSelected(id, true);
    const QString tab = qEnvironmentVariable("SYNBENCH_TOUR_TAB");
    if (!tab.isEmpty())
        m_runtime.run().setActiveTab(tab);
    QTimer::singleShot(600, this, &ScreenshotTour::step);
}

void ScreenshotTour::step()
{
    const int perSection = 2;  // light, dark
    const int total = int(m_sections.size()) * perSection;
    if (m_index >= total) {
        QCoreApplication::quit();
        return;
    }
    const QString section = m_sections[m_index / perSection];
    const bool dark = (m_index % perSection) == 1;
    m_runtime.app().setSection(section);
    m_runtime.settings().setTheme(dark ? QStringLiteral("dark") : QStringLiteral("light"));
    QTimer::singleShot(450, this, [this, section, dark] {
        const QImage image = m_window->grabWindow();
        const QString name = QStringLiteral("%1-%2-%3.png")
                                 .arg(m_index / 2 + 1, 2, 10, QLatin1Char('0'))
                                 .arg(section, dark ? QStringLiteral("dark") : QStringLiteral("light"));
        image.save(QDir(m_folder).filePath(name));
        ++m_index;
        step();
    });
}

} // namespace sb

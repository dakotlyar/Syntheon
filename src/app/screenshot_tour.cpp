// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/screenshot_tour.h"

#include "app/app_runtime.h"
#include "storage/run_repository.h"
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
    // Section names end up in file names: only the known ones are accepted,
    // so nothing here can name a path ("../x").
    const QStringList known{QStringLiteral("run"),     QStringLiteral("progress"), QStringLiteral("results"), QStringLiteral("history"),
                            QStringLiteral("compare"), QStringLiteral("system"),   QStringLiteral("settings")};
    for (const QString &s : wanted.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (known.contains(s) && !m_sections.contains(s))
            m_sections.append(s);
    }
    if (m_sections.isEmpty())
        m_sections = known;
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
    // The first-run dialog covers every screen; SYNBENCH_TOUR_SECTIONS=firstrun
    // leaves it up (the tour then shows the Run screen behind it).
    if (qEnvironmentVariable("SYNBENCH_TOUR_FIRSTRUN").isEmpty())
        m_runtime.settings().setFirstRunDone(true);
    for (const QString &id : qEnvironmentVariable("SYNBENCH_TOUR_SELECT").split(QLatin1Char(','), Qt::SkipEmptyParts))
        m_runtime.run().selection().setSelected(id, true);
    const QString tab = qEnvironmentVariable("SYNBENCH_TOUR_TAB");
    if (!tab.isEmpty())
        m_runtime.run().setActiveTab(tab);
    // SYNBENCH_TOUR_SEED=N adds N made-up saved runs (History / Compare
    // screenshots); the newest N are ticked for Compare.
    if (const int n = qEnvironmentVariableIntValue("SYNBENCH_TOUR_SEED"); n > 0)
        seedRuns(n);
    // SYNBENCH_TOUR_RUN=1 starts the ticked tests; screenshots then wait
    // SYNBENCH_TOUR_DELAY ms (default 600) so a run can get under way, or finish.
    if (qEnvironmentVariableIntValue("SYNBENCH_TOUR_RUN") != 0)
        m_runtime.run().start();
    bool ok = false;
    const int delay = qEnvironmentVariable("SYNBENCH_TOUR_DELAY").toInt(&ok);
    QTimer::singleShot(ok ? delay : 600, this, &ScreenshotTour::step);
}

void ScreenshotTour::seedRuns(int n)
{
    RunRepository repo(m_runtime.storage().connection());
    QStringList ids;
    for (int i = 0; i < n; ++i) {
        RunRecord r;
        r.uuid = QUuid::createUuid();
        r.status = i == 1 ? RunStatus::Stopped : RunStatus::Completed;
        r.startedAt = QDateTime::currentDateTimeUtc().addDays(-(n - i) * 3);
        r.finishedAt = r.startedAt.addSecs(900);
        r.appVersion = QStringLiteral("2026.11.0");
        r.system.machineLabel = QStringLiteral("Knowhere · ASUS TUF Gaming A14");
        for (const QString &id : {QStringLiteral("cpu"), QStringLiteral("ram"), QStringLiteral("storage")}) {
            if (i == 1 && id != QLatin1StringView("cpu"))
                continue;
            TestResult t;
            t.testId = id;
            t.status = Status::Ok;
            t.score = (id == QLatin1StringView("cpu") ? 1200.0 : id == QLatin1StringView("ram") ? 600.0 : 900.0) * (1.0 + 0.07 * i);
            t.formulaVersion = (i == 0 && id == QLatin1StringView("storage")) ? 0 : 1;
            t.metrics = {{QStringLiteral("headline"), {}, QStringLiteral("Headline"), *t.score * 3.1, QStringLiteral("points"), Direction::HigherIsBetter, true},
                         {QStringLiteral("latency"), {}, QStringLiteral("Latency"), 80.0 - i * 3, QStringLiteral("ns"), Direction::LowerIsBetter, false}};
            r.tests.append(t);
        }
        r.overall = computeOverall(r.tests, [](const QString &) { return true; });
        repo.saveRun(r);
        ids << r.uuid.toString(QUuid::WithoutBraces);
    }
    m_runtime.history().reload();
    for (const QString &id : ids.mid(ids.size() - qMin(4, int(ids.size()))))
        m_runtime.history().toggleChecked(id);
    m_runtime.compare().setRuns(m_runtime.history().checkedUuids());
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
    if (section == QLatin1StringView("results")) {
        const QString detail = qEnvironmentVariable("SYNBENCH_TOUR_DETAIL");
        m_runtime.results().openCurrent();
        if (!detail.isEmpty())
            m_runtime.results().showDetail(detail);
    }
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

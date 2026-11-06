// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/screenshot_tour.h"

#include "app/app_runtime.h"
#include "storage/run_repository.h"
#include "app/controllers/run_controller.h"

#include <QCoreApplication>
#include <QPointF>
#include <QDir>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

namespace sb {

ScreenshotTour::ScreenshotTour(QQuickWindow *window, AppRuntime &runtime, QString folder, QObject *parent)
    : QObject(parent), m_window(window), m_runtime(runtime), m_folder(std::move(folder))
{
    const QString wanted = qEnvironmentVariable("SYNTHEON_TOUR_SECTIONS");
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

// SYNTHEON_TOUR_SELECT=cpu,battery ticks those tests first; SYNTHEON_TOUR_TAB
// picks the option tab to show.
void ScreenshotTour::prepare()
{
    // SYNTHEON_TOUR_SIZE=1100x2600 resizes the window first, so a whole
    // Detail page fits in one image (a mockup comparison).
    if (const QStringList size = qEnvironmentVariable("SYNTHEON_TOUR_SIZE").split(QLatin1Char('x')); size.size() == 2) {
        bool okW = false, okH = false;
        const int w = size[0].toInt(&okW), h = size[1].toInt(&okH);
        if (okW && okH && w >= 640 && h >= 480 && w <= 4000 && h <= 8000)
            m_window->resize(w, h);
    }
    // The first-run dialog covers every screen; SYNTHEON_TOUR_SECTIONS=firstrun
    // leaves it up (the tour then shows the Run screen behind it).
    if (qEnvironmentVariable("SYNTHEON_TOUR_FIRSTRUN").isEmpty())
        m_runtime.settings().setFirstRunDone(true);
    for (const QString &id : qEnvironmentVariable("SYNTHEON_TOUR_SELECT").split(QLatin1Char(','), Qt::SkipEmptyParts))
        m_runtime.run().selection().setSelected(id, true);
    const QString tab = qEnvironmentVariable("SYNTHEON_TOUR_TAB");
    if (!tab.isEmpty())
        m_runtime.run().setActiveTab(tab);
    // SYNTHEON_TOUR_SEED=N adds N made-up saved runs (History / Compare
    // screenshots); the newest N are ticked for Compare.
    if (const int n = qEnvironmentVariableIntValue("SYNTHEON_TOUR_SEED"); n > 0)
        seedRuns(n);
    // SYNTHEON_TOUR_SIMULATE=cpu=error,ram=na sets those stubs' Simulate
    // option (Error / N/A outcomes on Progress and Results).
    for (const QString &pair : qEnvironmentVariable("SYNTHEON_TOUR_SIMULATE").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList kv = pair.split(QLatin1Char('='));
        if (kv.size() == 2)
            m_runtime.run().setOption(kv[0], QStringLiteral("simulate"), kv[1]);
    }
    // SYNTHEON_TOUR_SEED_BATTERY=1 adds a made-up Battery run, the newest.
    if (qEnvironmentVariableIntValue("SYNTHEON_TOUR_SEED_BATTERY") != 0)
        seedBatteryRun();
    // SYNTHEON_TOUR_BASELINE=oldest makes the oldest compared run the baseline.
    if (qEnvironmentVariable("SYNTHEON_TOUR_BASELINE") == QLatin1StringView("oldest")) {
        const QVariantList runs = m_runtime.compare().runs();
        if (!runs.isEmpty())
            m_runtime.compare().setBaseline(runs.last().toMap().value(QStringLiteral("uuid")).toString());
    }
    // SYNTHEON_TOUR_BANNER=1 shows a notice banner across the top.
    if (qEnvironmentVariableIntValue("SYNTHEON_TOUR_BANNER") != 0)
        m_runtime.startup().setInstanceWarning(QStringLiteral("A second Syntheon can't ask this one to come to the front (sample notice)."));
    // SYNTHEON_TOUR_RUN=1 starts the ticked tests; screenshots then wait
    // SYNTHEON_TOUR_DELAY ms (default 600) so a run can get under way, or finish.
    if (qEnvironmentVariableIntValue("SYNTHEON_TOUR_RUN") != 0)
        m_runtime.run().start();
    // SYNTHEON_TOUR_DIALOG=<name> opens one of the app's dialogs over every
    // screenshot, with sample text (only the dialogs listed here).
    if (const QString dialog = qEnvironmentVariable("SYNTHEON_TOUR_DIALOG"); !dialog.isEmpty())
        openDialog(dialog);
    bool ok = false;
    const int delay = qEnvironmentVariable("SYNTHEON_TOUR_DELAY").toInt(&ok);
    QTimer::singleShot(ok ? delay : 600, this, &ScreenshotTour::step);
}

void ScreenshotTour::openDialog(const QString &name)
{
    static const QHash<QString, QPair<QString, QVariantMap>> known{
        {QStringLiteral("storage"), {QStringLiteral("StorageProblemDialog"),
                                     {{QStringLiteral("message"), QStringLiteral("The history file at /home/you/.local/share/Syntheon/syntheon.db "
                                                                                  "is damaged or isn't a Syntheon history (file is not a database). "
                                                                                  "It was left untouched.")}}}},
        {QStringLiteral("newer"), {QStringLiteral("StorageProblemDialog"),
                                   {{QStringLiteral("newerVersion"), true},
                                    {QStringLiteral("message"), QStringLiteral("The saved runs in /home/you/.local/share/Syntheon were made by a newer Syntheon.")}}}},
        {QStringLiteral("auth"), {QStringLiteral("AuthDialog"), {{QStringLiteral("message"), QStringLiteral("The administrator prompt was dismissed.")}}}},
        {QStringLiteral("inuse"), {QStringLiteral("InUseDialog"), {}}},
        {QStringLiteral("recovery"), {QStringLiteral("RecoveryNoticeDialog"), {{QStringLiteral("message"), QStringLiteral("The run from 14:02 was interrupted")}}}},
        {QStringLiteral("firstrun"), {QStringLiteral("FirstRunDialog"), {}}},
    };
    const auto it = known.constFind(name);
    QQmlEngine *engine = qmlEngine(m_window);
    if (it == known.constEnd() || !engine || !m_window->contentItem()) {
        qWarning("screenshot tour: unknown dialog '%s'", qPrintable(name));
        return;
    }
    QQmlComponent component(engine, QStringLiteral("Syntheon"), it->first);
    QVariantMap props = it->second;
    props.insert(QStringLiteral("parent"), QVariant::fromValue(static_cast<QObject *>(m_window->contentItem())));
    QObject *dialog = component.createWithInitialProperties(props);
    if (!dialog) {
        qWarning("screenshot tour: %s", qPrintable(component.errorString()));
        return;
    }
    dialog->setParent(this);
    QMetaObject::invokeMethod(dialog, "open");
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

// Per screenshot, once the section is showing:
//   SYNTHEON_TOUR_LOG=1           opens the Results screen's log dialog
//   SYNTHEON_TOUR_SCROLL=end      scrolls the visible screen to its end
//   SYNTHEON_TOUR_POPUP=<name>    opens the popup of the combo box <name>
void ScreenshotTour::stage()
{
    // From the window: items declared in Main.qml are its QObject children
    // (the content item is only their visual parent).
    QObject *root = m_window;
    if (qEnvironmentVariableIntValue("SYNTHEON_TOUR_LOG") != 0) {
        QObject *log = root->findChild<QObject *>(QStringLiteral("logDialog"));
        if (!log)
            qWarning("screenshot tour: no log dialog on this screen");
        else if (!log->property("opened").toBool())
            QMetaObject::invokeMethod(log, "openLog");
    }
    if (qEnvironmentVariable("SYNTHEON_TOUR_SCROLL") == QLatin1StringView("end")) {
        for (QQuickItem *f : root->findChildren<QQuickItem *>(QStringLiteral("screenScroll"))) {
            if (!f->isVisible())
                continue;
            const qreal end = f->property("contentHeight").toReal() - f->height();
            qInfo("screenshot tour: scrolling %s to %.0f", qPrintable(f->parentItem() ? f->parentItem()->metaObject()->className() : "?"), end);
            f->setProperty("contentY", qMax<qreal>(0, end));
        }
    }
    if (const QString name = qEnvironmentVariable("SYNTHEON_TOUR_POPUP"); !name.isEmpty()) {
        for (QQuickItem *combo : root->findChildren<QQuickItem *>(name)) {
            if (!combo->isVisible())
                continue;
            if (auto *popup = combo->property("popup").value<QObject *>(); popup && !popup->property("opened").toBool())
                QMetaObject::invokeMethod(popup, "open");
            break;
        }
    }
}

void ScreenshotTour::seedBatteryRun()
{
    RunRepository repo(m_runtime.storage().connection());
    RunRecord r;
    r.uuid = QUuid::createUuid();
    r.status = RunStatus::Completed;
    r.startedAt = QDateTime::currentDateTimeUtc().addSecs(-3600);
    r.finishedAt = r.startedAt.addSecs(402 * 60);
    r.appVersion = QStringLiteral("2026.11.0");
    r.system.machineLabel = QStringLiteral("Knowhere · ASUS TUF Gaming A14");
    TestResult t;
    t.testId = QStringLiteral("battery");
    t.status = Status::Ok;
    t.score = 900;
    t.formulaVersion = 0;
    t.options.insert(QStringLiteral("profile"), QStringLiteral("mixed"));
    t.metrics = {{QStringLiteral("runtime"), {}, QStringLiteral("Battery life"), 402.0, QStringLiteral("min"), Direction::HigherIsBetter, true},
                 {QStringLiteral("avg_drain"), {}, QStringLiteral("Average drain"), 12.6, QStringLiteral("W"), Direction::LowerIsBetter, true},
                 {QStringLiteral("health"), {}, QStringLiteral("Battery health"), 94.0, QStringLiteral("%"), Direction::Neutral, true},
                 {QStringLiteral("full_wh"), {}, QStringLiteral("Full-charge capacity"), 84.6, QStringLiteral("Wh"), Direction::HigherIsBetter, false}};
    QVector<QPointF> samples;
    for (int m = 0; m <= 402; m += 6)
        samples.append({double(m), 100.0 - 95.0 * m / 402.0});
    t.series.append({QStringLiteral("drain"), QStringLiteral("Charge over time"), QStringLiteral("Time"), QStringLiteral("min"),
                     QStringLiteral("%"), samples});
    r.tests.append(t);
    r.overall = computeOverall(r.tests, [](const QString &) { return false; });
    repo.saveRun(r);
    m_runtime.history().reload();
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
        const QString detail = qEnvironmentVariable("SYNTHEON_TOUR_DETAIL");
        m_runtime.results().openCurrent();
        if (!detail.isEmpty())
            m_runtime.results().showDetail(detail);
    }
    m_runtime.settings().setTheme(dark ? QStringLiteral("dark") : QStringLiteral("light"));
    QTimer::singleShot(250, this, [this] { stage(); });
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

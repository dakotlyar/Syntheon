// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/fonts.h"
#include "app/screenshot_tour.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtQml>

#include "synbench_version.h"

#include <cstdio>
#include <memory>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    // P25: never run the GUI as root. A root GUI breaks on Wayland and leaves
    // root-owned history; SynBench asks for administrator access itself.
    if (geteuid() == 0) {
        std::fputs("SynBench must not be run as root (for example with sudo). Start it as your normal user;\n"
                   "it asks for administrator access itself when it starts.\n",
                   stderr);
        return 1;
    }
#endif

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("SynBench"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("SynBench"));
    QGuiApplication::setApplicationVersion(QStringLiteral(SYNBENCH_DISPLAY_VERSION));

    sb::loadBundledFonts();
    // P15: Qt Quick Controls Basic, re-skinned in QML.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    sb::AppRuntime runtime(sb::AppRuntime::optionsFromEnvironment());
    QObject::connect(&runtime.app(), &sb::AppController::quitRequested, &app, &QCoreApplication::quit);
    runtime.registerQml();

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("SynBench", "Main");
    runtime.start();

    // Developer tool: SYNBENCH_TOUR=<folder> saves a screenshot of every screen.
    const QString tourFolder = qEnvironmentVariable("SYNBENCH_TOUR");
    std::unique_ptr<sb::ScreenshotTour> tour;
    if (!tourFolder.isEmpty() && !engine.rootObjects().isEmpty()) {
        tour = std::make_unique<sb::ScreenshotTour>(qobject_cast<QQuickWindow *>(engine.rootObjects().first()), runtime,
                                                    tourFolder);
        tour->start();
    }

    return QGuiApplication::exec();
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/fonts.h"
#include "app/process_hardening.h"
#ifdef Q_OS_LINUX
#include "app/self_check.h"
#endif
#ifdef SYNTHEON_DEV_HOOKS
#include "app/screenshot_tour.h"
#endif

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtQml>

#include "syntheon_version.h"

#include <cstdio>
#include <cstring>
#include <memory>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    // `Syntheon --syntheon-selfcheck`: read-only, unprivileged, prints PASS/FAIL.
    // (Ahead of everything else, so it works as any user and without a display.)
    if (argc > 1 && std::strcmp(argv[1], "--syntheon-selfcheck") == 0)
        return sb::runSelfCheck(argc, argv);
#endif
#ifndef SYNTHEON_DEV_HOOKS
    // Syntheon runs elevated: first of all, before Qt (or anything else) loads a
    // library, restrict where DLLs come from (Windows). The exe's own
    // /DEPENDENTLOADFLAG covers the libraries that load before main().
    // PACKAGED builds only: they have every DLL and plugin beside the exe
    // (windeployqt). A development build finds Qt through PATH and its prefix,
    // which these restrictions would break (and a missing DLL at load time makes
    // Windows show a modal dialog, hanging an unattended run).
    sb::hardenDllSearch();
    // A packaged build takes its plugins and QML only from its own folders, never
    // from environment variables a caller set. (A development build keeps them:
    // it runs from a Qt prefix and tests rely on that.)
    sb::scrubQtEnvironment();
    {
        const QString exeDir = sb::executableFolder();
#ifdef Q_OS_WIN
        sb::pinLibraryPaths({exeDir});               // windeployqt layout: plugins next to the exe
#else
        sb::pinLibraryPaths({exeDir + QStringLiteral("/../plugins")});  // the AppDir's usr/plugins (also in qt.conf)
#endif
    }
#endif

#ifdef Q_OS_LINUX
    // P25: never run the GUI as root. A root GUI breaks on Wayland and leaves
    // root-owned history; Syntheon asks for administrator access itself.
    if (geteuid() == 0) {
        std::fputs("Syntheon must not be run as root (for example with sudo). Start it as your normal user;\n"
                   "it asks for administrator access itself when it starts.\n",
                   stderr);
        return 1;
    }
#endif

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Syntheon"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Syntheon"));
    // Matches the .desktop file, so Wayland compositors show the right icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.dakotlyar.Syntheon"));
    QGuiApplication::setApplicationVersion(QStringLiteral(SYNTHEON_DISPLAY_VERSION));

    sb::loadBundledFonts();
    // P15: Qt Quick Controls Basic, re-skinned in QML.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    sb::AppRuntime runtime(sb::AppRuntime::optionsFromEnvironment());
    // One Syntheon per history: a second launch asks the first to come to the
    // front and leaves quietly (exit code 0). This is before the window and
    // before the administrator prompt.
    if (!runtime.claimInstance())
        return runtime.redirectedToRunningInstance() ? 0 : 1;
    QObject::connect(&runtime.app(), &sb::AppController::quitRequested, &app, &QCoreApplication::quit);
    runtime.registerQml();

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Syntheon", "Main");
    runtime.start();

    // A second launch asks us to come to the front. Wayland may refuse to
    // raise a window it didn't focus itself; alert() (attention request) is the
    // portable fallback, so both are done.
    QObject::connect(&runtime.instance(), &sb::SingleInstance::focusRequested, &app, [&engine](const QString &) {
        if (engine.rootObjects().isEmpty())
            return;
        auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!w)
            return;
        if (w->windowStates() & Qt::WindowMinimized)
            w->showNormal();
        else
            w->show();
        w->raise();
        w->requestActivate();
        w->alert(0);
    });

#ifdef SYNTHEON_DEV_HOOKS
    // Developer tool: SYNTHEON_TOUR=<folder> saves a screenshot of every screen.
    const QString tourFolder = qEnvironmentVariable("SYNTHEON_TOUR");
    std::unique_ptr<sb::ScreenshotTour> tour;
    if (!tourFolder.isEmpty() && !engine.rootObjects().isEmpty()) {
        tour = std::make_unique<sb::ScreenshotTour>(qobject_cast<QQuickWindow *>(engine.rootObjects().first()), runtime,
                                                    tourFolder);
        tour->start();
    }
#endif

    return QGuiApplication::exec();
}

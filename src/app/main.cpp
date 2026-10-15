// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/app_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QtQml>

#include "synbench_version.h"

#include <cstdio>

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

    // P15: Qt Quick Controls Basic, re-skinned in QML.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    sb::AppController controller;
    QObject::connect(&controller, &sb::AppController::quitRequested, &app, &QCoreApplication::quit);
    qmlRegisterSingletonInstance("SynBench", 1, 0, "AppController", &controller);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("SynBench", "Main");
    controller.start();

    return QGuiApplication::exec();
}

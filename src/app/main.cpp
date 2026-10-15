// SPDX-License-Identifier: GPL-3.0-or-later
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include "synbench_version.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("SynBench"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("SynBench"));
    QGuiApplication::setApplicationVersion(QStringLiteral(SYNBENCH_DISPLAY_VERSION));

    // P15: Qt Quick Controls Basic, re-skinned in QML.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("SynBench", "Main");

    return QGuiApplication::exec();
}

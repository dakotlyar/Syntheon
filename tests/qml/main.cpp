// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app_runtime.h"
#include "app/fonts.h"
#include "app/models/options_model.h"
#include "core/result.h"
#include "storage/run_repository.h"

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtQuickTest>

#include <memory>

// An OptionsModel with one option of every kind, for the editor tests.
class Fixtures : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE QObject *allKindsModel()
    {
        sb::OptionSchema s;
        sb::OptionDef b;
        b.key = QStringLiteral("flag"); b.label = QStringLiteral("A flag"); b.kind = sb::OptionKind::Bool; b.defaultValue = true;
        s.options.append(b);
        sb::OptionDef m;
        m.key = QStringLiteral("multi"); m.label = QStringLiteral("Runtimes"); m.kind = sb::OptionKind::MultiSelect;
        m.items = {{QStringLiteral("a"), QStringLiteral("Alpha")}, {QStringLiteral("b"), QStringLiteral("Beta")},
                   {QStringLiteral("c"), QStringLiteral("Gamma"), false, QStringLiteral("no Intel device")}};
        m.defaultValue = QStringList{QStringLiteral("a")};
        s.options.append(m);
        sb::OptionDef seg;
        seg.key = QStringLiteral("seg"); seg.label = QStringLiteral("Profile"); seg.kind = sb::OptionKind::SingleSelect;
        seg.presentation = sb::SelectPresentation::Segmented;
        seg.items = {{QStringLiteral("l"), QStringLiteral("Light")}, {QStringLiteral("m"), QStringLiteral("Mixed")}};
        seg.defaultValue = QStringLiteral("m");
        s.options.append(seg);
        sb::OptionDef drop;
        drop.key = QStringLiteral("drop"); drop.label = QStringLiteral("Buffer"); drop.kind = sb::OptionKind::SingleSelect;
        drop.items = {{QStringLiteral("s"), QStringLiteral("256 MiB")}, {QStringLiteral("g"), QStringLiteral("1 GiB")}};
        drop.defaultValue = QStringLiteral("g");
        s.options.append(drop);
        sb::OptionDef dyn;
        dyn.key = QStringLiteral("dyn"); dyn.label = QStringLiteral("Drive"); dyn.kind = sb::OptionKind::DynamicSelect;
        dyn.items = {{QStringLiteral("/"), QStringLiteral("/"), true, {}, QStringLiteral("NVMe · 812 GB free · btrfs")},
                     {QStringLiteral("/mnt"), QStringLiteral("/mnt")}};
        dyn.defaultValue = QStringLiteral("/");
        s.options.append(dyn);
        sb::OptionDef num;
        num.key = QStringLiteral("num"); num.label = QStringLiteral("Duration"); num.kind = sb::OptionKind::Number;
        num.minimum = 5; num.maximum = 60; num.step = 5; num.unit = QStringLiteral("s"); num.defaultValue = 10.0;
        s.options.append(num);
        sb::OptionDef path;
        path.key = QStringLiteral("path"); path.label = QStringLiteral("Folder"); path.kind = sb::OptionKind::Path;
        path.defaultValue = QString();
        s.options.append(path);
        sb::OptionDef info;
        info.key = QStringLiteral("info"); info.label = QStringLiteral("Window"); info.kind = sb::OptionKind::Info;
        info.infoValue = QStringLiteral("1280×720"); info.infoTone = sb::Tone::Good;
        s.options.append(info);

        auto *values = new sb::OptionValues;
        m_values.push_back(std::unique_ptr<sb::OptionValues>(values));
        auto *model = new sb::OptionsModel(s, values, this);
        model->setSchema(s);
        return model;
    }

private:
    std::vector<std::unique_ptr<sb::OptionValues>> m_values;
};

// Real controllers on a temporary settings file and history (no elevation, no
// helper), registered as the QML singletons before any test file loads. Two
// runs are seeded so Clear history has something to clear.
class Setup : public QObject
{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {
        sb::loadBundledFonts();
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        m_dir = std::make_unique<QTemporaryDir>();
        sb::AppRuntime::Options o;
        o.settingsFile = m_dir->filePath(QStringLiteral("settings.ini"));
        o.dataFolder = m_dir->filePath(QStringLiteral("data"));
        o.smoke = true;  // no helper, no polkit
        m_runtime = std::make_unique<sb::AppRuntime>(o);
        m_runtime->storageController().open();
        sb::RunRepository repo(m_runtime->storage().connection());
        for (int i = 0; i < 2; ++i) {
            sb::RunRecord r;
            r.uuid = QUuid::createUuid();
            r.status = sb::RunStatus::Completed;
            r.startedAt = QDateTime::currentDateTimeUtc();
            repo.saveRun(r);
        }
        m_runtime->registerQml();
        qmlRegisterSingletonInstance("SynBenchTest", 1, 0, "Fixtures", &m_fixtures);
        // Stubs at a fast time scale; probes and the (elevation-free) gate.
        qputenv("SYNBENCH_STUB_SCALE", "0.02");
        m_runtime->app().start();
        m_runtime->run().refreshProbes();
    }

    void qmlEngineAvailable(QQmlEngine *) {}

    // The controllers own QObjects, timers and a process; they must be gone
    // while the application object still exists (this runs after the last
    // test, before QtQuickTest destroys it).
    void cleanupTestCase()
    {
        m_runtime.reset();
        m_dir.reset();
    }

private:
    Fixtures m_fixtures;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<sb::AppRuntime> m_runtime;
};

QUICK_TEST_MAIN_WITH_SETUP(synbench, Setup)

#include "main.moc"

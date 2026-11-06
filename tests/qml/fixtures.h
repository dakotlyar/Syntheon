// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/controllers/app_controller.h"
#include "app/controllers/run_controller.h"
#include "app/models/detail_blocks.h"
#include "app/models/options_model.h"
#include "core/options.h"
#include "tests/common/stub_clock.h"

#include <QJSEngine>
#include <QObject>
#include <QQmlEngine>

#include <memory>
#include <vector>

// QML singleton "Fixtures" (module SyntheonTest): an OptionsModel with one
// option of every kind, for the editor tests, and the stub time scale. Declared
// at build time so qmllint sees it; main.cpp's Setup owns the one instance.
class Fixtures : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    inline static Fixtures *instance = nullptr;
    static Fixtures *create(QQmlEngine *, QJSEngine *)
    {
        Q_ASSERT(instance);
        QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
        return instance;
    }

    // Stubs run at this fraction of their real time (tests only).
    Q_INVOKABLE void setStubScale(double scale) { sb::tests::setStubTimeScale(scale); }

    // The power state the app sees (AppController::setPowerForTest), so a
    // test doesn't depend on the host being on AC, on battery or without one.
    // chargePct -1 is "unknown". clearPower() goes back to the real readings.
    inline static sb::AppController *app = nullptr;
    // The interface hold (Phase 4a P10): what a running GPU test would ask for,
    // sent through the orchestrator's own signals.
    inline static sb::RunController *run = nullptr;
    Q_INVOKABLE void holdInterface(bool hold) { emit run->orchestrator().interfaceHold(0, hold); }
    Q_INVOKABLE void heldProgress(const QString &part, double fraction = 0.1)
    {
        sb::ProgressUpdate u;
        u.index = 0;
        u.testId = QStringLiteral("ai");
        u.fraction = fraction;
        u.stage = QStringLiteral("stage");
        u.part = part;
        emit run->orchestrator().progress(u);
    }
    // Zeb D1: a table block as the C++ export builds it (buildDetailBlocks), so the QML test sees the real shape
    // of boldCells: "Shading" on DX12 bold.
    Q_INVOKABLE QVariantMap exportedBoldTable() const
    {
        sb::TableBlock t;
        t.columns = {QStringLiteral("Scene"), QStringLiteral("Vulkan"), QStringLiteral("DX12")};
        const auto row = [](const char *a, const char *b, const char *c) {
            return QVector<sb::DetailCell>{sb::DetailCell::literal(QString::fromUtf8(a)), sb::DetailCell::literal(QString::fromUtf8(b)),
                                           sb::DetailCell::literal(QString::fromUtf8(c))};
        };
        t.rows = {row("Geometry", "410 fps", "412 fps"), row("Shading", "205 fps", "199 fps"), row("Particles", "88 fps", "90 fps")};
        t.boldCells = {QPoint(2, 1)};
        sb::DetailLayout layout;
        layout.blocks.append({QString(), t});
        return sb::buildDetailBlocks(layout, sb::TestResult{}, QStringLiteral("t-gpu")).first().toMap();
    }
    Q_INVOKABLE void setPower(bool present, bool acOnline, double chargePct)
    {
        sb::BatteryInfo b;
        b.present = present;
        b.acOnline = acOnline;
        b.chargePct = chargePct;
        app->setPowerForTest(b);
    }
    Q_INVOKABLE void clearPower() { app->setPowerForTest(std::nullopt); }

    Q_INVOKABLE QObject *allKindsModel()
    {
        sb::OptionSchema s;
        sb::OptionDef b;
        b.key = QStringLiteral("flag"); b.label = QStringLiteral("A flag"); b.kind = sb::OptionKind::Bool; b.defaultValue = true;
        s.options.append(b);
        sb::OptionDef m;
        m.key = QStringLiteral("multi"); m.label = QStringLiteral("Runtimes"); m.kind = sb::OptionKind::MultiSelect;
        m.items = {{QStringLiteral("a"), QStringLiteral("Alpha")}, {QStringLiteral("b"), QStringLiteral("Beta")},
                   {QStringLiteral("c"), QStringLiteral("Gamma"), false, QStringLiteral("no Intel device")},
                   // a label that already carries its reason: shown once, not twice
                   {QStringLiteral("d"), QStringLiteral("Delta (no NVIDIA GPU)"), false, QStringLiteral("no NVIDIA GPU")}};
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
        sb::OptionDef warn;
        warn.key = QStringLiteral("longinfo"); warn.label = QStringLiteral("Models"); warn.kind = sb::OptionKind::Info;
        warn.infoValue = QStringLiteral("Specific models and precisions are Phase 6 open questions.");
        warn.infoTone = sb::Tone::Warn;
        s.options.append(warn);

        auto *values = new sb::OptionValues;
        m_values.push_back(std::unique_ptr<sb::OptionValues>(values));
        auto *model = new sb::OptionsModel(s, values, this);
        model->setSchema(s);
        return model;
    }

private:
    std::vector<std::unique_ptr<sb::OptionValues>> m_values;
};

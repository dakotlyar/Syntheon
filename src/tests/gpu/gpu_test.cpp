// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/gpu_test.h"

namespace sb::tests {

namespace {

struct Api {
    const char *id, *name;
    double fps, low, fill;
    bool available;
};

#ifdef Q_OS_WIN
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

const Api kApis[] = {
    {"vulkan", "Vulkan", 412, 361, 38.1, true},
    {"dx12", "DirectX 12", 398, 347, 37.4, kWindows},
    {"dx11", "DirectX 11", 371, 322, 35.9, kWindows},
    {"opengl", "OpenGL", 344, 290, 33.2, true},
};

StubSpec makeSpec()
{
    StubSpec s;
    TestInfo &i = s.info;
    i.id = QStringLiteral("gpu");
    i.name = QStringLiteral("GPU");
    i.summary = QStringLiteral("2D and 3D on Vulkan, DirectX 12, DirectX 11, OpenGL");
    i.colorToken = QStringLiteral("t-gpu");
    i.subTests = {{QStringLiteral("scenes3d"), QStringLiteral("3D scenes"), true, true},
                  {QStringLiteral("native2d"), QStringLiteral("Native 2D"), true, true},
                  {QStringLiteral("fill2d"), QStringLiteral("2D fill-rate per API"), true, true}};
    s.estimate = std::chrono::seconds(24);
    s.targetScore = 1206;

    for (const Api &api : kApis) {
        const QString id = QString::fromLatin1(api.id);
        const auto when = [id](const OptionValues &o) { return optionContains(o, QStringLiteral("apis"), id); };
        s.metrics.append({QStringLiteral("scenes3d.") + id + QStringLiteral(".fps"), QStringLiteral("scenes3d"),
                          QString::fromLatin1(api.name) + QStringLiteral(" 3D average"), QStringLiteral("fps"), api.fps,
                          Direction::HigherIsBetter, id == QLatin1StringView("vulkan"), 0.15, when});
        s.metrics.append({QStringLiteral("scenes3d.") + id + QStringLiteral(".low"), QStringLiteral("scenes3d"),
                          QString::fromLatin1(api.name) + QStringLiteral(" 3D 1% low"), QStringLiteral("fps"), api.low,
                          Direction::HigherIsBetter, false, 0, when});
        s.metrics.append({QStringLiteral("fill2d.") + id, QStringLiteral("fill2d"),
                          QString::fromLatin1(api.name) + QStringLiteral(" 2D fill-rate"), QStringLiteral("Gpx/s"), api.fill,
                          Direction::HigherIsBetter, false, 0.05, when});
    }
    const std::pair<const char *, const char *> native[] = {{"shapes", "Shapes and paths"}, {"text", "Text rendering"},
                                                            {"blits", "Image blits"}, {"gradients", "Gradients"}};
    const double nativeBase[] = {2.94, 1.61, 8.82, 4.37};
    for (int n = 0; n < 4; ++n) {
        s.metrics.append({QStringLiteral("native2d.") + QString::fromLatin1(native[n].first), QStringLiteral("native2d"),
                          QString::fromLatin1(native[n].second), QStringLiteral("MOps/s"), nativeBase[n], Direction::HigherIsBetter, false,
                          0.05, {}});
    }

    s.options = [](const ProbeContext &, const Availability &) {
        OptionSchema o;
        OptionDef apis;
        apis.key = QStringLiteral("apis");
        apis.label = QStringLiteral("APIs");
        apis.kind = OptionKind::MultiSelect;
        apis.minSelected = 1;
        QStringList defaults;
        for (const Api &api : kApis) {
            OptionItem item{QString::fromLatin1(api.id), QString::fromLatin1(api.name)};
            if (!api.available) {
                item.enabled = false;
                item.disabledReason = QStringLiteral("Windows only");
            } else {
                defaults.append(item.value);
            }
            apis.items.append(item);
        }
        apis.defaultValue = defaults;
        o.options.append(apis);
        OptionDef window;
        window.key = QStringLiteral("window");
        window.label = QStringLiteral("Window");
        window.kind = OptionKind::Info;
        window.infoValue = QStringLiteral("1280×720 · vsync off");
        o.options.append(window);
        return o;
    };

    s.detail = [](const TestResult &r) {
        DetailLayout d;
        d.subtitle = scoreSubtitle(r, {QStringLiteral("1280×720")});
        d.headlineMetrics = {QStringLiteral("scenes3d.vulkan.fps"), QStringLiteral("fill2d.vulkan")};
        TableBlock table;
        table.columns = {QStringLiteral("API"), QStringLiteral("3D avg fps"), QStringLiteral("3D 1% low"), QStringLiteral("2D fill-rate")};
        for (const Api &api : kApis) {
            const QString id = QString::fromLatin1(api.id);
            if (!r.metric(QStringLiteral("scenes3d.") + id + QStringLiteral(".fps")) && !r.metric(QStringLiteral("fill2d.") + id))
                continue;
            table.rows.append({DetailCell::literal(QString::fromLatin1(api.name)),
                               DetailCell::metric(QStringLiteral("scenes3d.") + id + QStringLiteral(".fps")),
                               DetailCell::metric(QStringLiteral("scenes3d.") + id + QStringLiteral(".low")),
                               DetailCell::metric(QStringLiteral("fill2d.") + id)});
        }
        if (!table.rows.isEmpty())
            d.blocks.append({{}, table});
        if (r.metric(QStringLiteral("native2d.shapes"))) {
            TableBlock native;
            native.columns = {QStringLiteral("Native 2D"), QStringLiteral("Result")};
            for (const char *k : {"shapes", "text", "blits", "gradients"}) {
                const QString key = QStringLiteral("native2d.") + QString::fromLatin1(k);
                native.rows.append({DetailCell::literal(r.metric(key)->label), DetailCell::metric(key)});
            }
            d.blocks.append({{}, native});
        }
        d.blocks.append({{}, NoteBlock{QStringLiteral("Scene contents and how many scenes there are get decided in Phase 5."),
                                       Tone::Warn}});
        return d;
    };
    return s;
}

} // namespace

GpuTest::GpuTest() : StubTest(makeSpec()) {}

} // namespace sb::tests

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/ai/ai_test.h"

namespace sb::tests {

namespace {

struct Workload {
    const char *id, *name, *unit;
    double base;
};
const Workload kWorkloads[] = {
    {"llm", "LLM text generation", "tok/s", 104},
    {"vision", "Image classification / detection", "inf/s", 1920},
    {"imagegen", "Image generation", "it/s", 3.1},
    {"speech", "Speech-to-text (× real time)", "", 25},
};
struct Runtime {
    const char *id, *name;
    double factor;
};
const Runtime kRuntimes[] = {{"llamacpp", "llama.cpp", 1.0}, {"onnx", "ONNX Runtime", 0.9}, {"tensorrt", "TensorRT", 1.25},
                             {"openvino", "OpenVINO", 1.1}};

StubSpec makeSpec()
{
    StubSpec s;
    TestInfo &i = s.info;
    i.id = QStringLiteral("ai");
    i.name = QStringLiteral("AI inference");
    i.summary = QStringLiteral("LLM, vision, image generation, speech");
    i.colorToken = QStringLiteral("t-ai");
    i.subTests = {{QStringLiteral("llm"), QStringLiteral("LLM text generation"), true, true},
                  {QStringLiteral("vision"), QStringLiteral("Image classification / detection"), true, true},
                  {QStringLiteral("imagegen"), QStringLiteral("Image generation"), true, true},
                  {QStringLiteral("speech"), QStringLiteral("Speech-to-text"), true, true}};
    s.estimate = std::chrono::seconds(30);
    s.targetScore = 772;

    for (const Workload &w : kWorkloads) {
        for (const Runtime &rt : kRuntimes) {
            const QString runtime = QString::fromLatin1(rt.id);
            s.metrics.append({QString::fromLatin1(w.id) + QLatin1Char('.') + runtime, QString::fromLatin1(w.id),
                              QStringLiteral("%1 on %2").arg(QString::fromLatin1(w.name), QString::fromLatin1(rt.name)),
                              QString::fromLatin1(w.unit), w.base * rt.factor, Direction::HigherIsBetter,
                              QLatin1StringView(w.id) == QLatin1StringView("llm") && runtime == QLatin1StringView("llamacpp"),
                              0.25 / 3.0, [runtime](const OptionValues &o) {
                                  return optionContains(o, QStringLiteral("runtimes"), runtime) && runtime != QLatin1StringView("openvino");
                              }});
        }
    }

    s.options = [](const ProbeContext &, const Availability &) {
        OptionSchema o;
        OptionDef pack;
        pack.key = QStringLiteral("model_pack");
        pack.label = QStringLiteral("Model pack");
        pack.kind = OptionKind::Info;
        pack.infoValue = QStringLiteral("Installed · v1 (stub)");
        pack.infoTone = Tone::Good;
        o.options.append(pack);
        OptionDef rt;
        rt.key = QStringLiteral("runtimes");
        rt.label = QStringLiteral("Runtimes");
        rt.kind = OptionKind::MultiSelect;
        rt.minSelected = 1;
        QStringList defaults;
        for (const Runtime &r : kRuntimes) {
            OptionItem item{QString::fromLatin1(r.id), QString::fromLatin1(r.name)};
            if (QLatin1StringView(r.id) == QLatin1StringView("openvino")) {
                item.label = QStringLiteral("OpenVINO (no Intel device)");
                item.enabled = false;
                item.disabledReason = QStringLiteral("no Intel device");
            } else {
                defaults.append(item.value);
            }
            rt.items.append(item);
        }
        rt.defaultValue = defaults;
        o.options.append(rt);
        OptionDef note;
        note.key = QStringLiteral("models_note");
        note.label = QStringLiteral("Models");
        note.kind = OptionKind::Info;
        note.infoValue = QStringLiteral("Specific models and precisions are Phase 6 open questions.");
        note.infoTone = Tone::Warn;
        o.options.append(note);
        return o;
    };

    s.detail = [](const TestResult &r) {
        DetailLayout d;
        d.subtitle = scoreSubtitle(r, {QStringLiteral("model pack v1 (stub)")});
        d.headlineMetrics = {QStringLiteral("llm.llamacpp")};
        QStringList runtimes;
        for (const Runtime &rt : kRuntimes) {
            for (const Metric &m : r.metrics) {
                if (m.key.endsWith(QLatin1Char('.') + QString::fromLatin1(rt.id))) {
                    runtimes.append(QString::fromLatin1(rt.id));
                    break;
                }
            }
        }
        TableBlock table;
        table.columns = {QStringLiteral("Workload")};
        for (const QString &rid : runtimes) {
            for (const Runtime &rt : kRuntimes) {
                if (QString::fromLatin1(rt.id) == rid)
                    table.columns.append(QString::fromLatin1(rt.name));
            }
        }
        for (const Workload &w : kWorkloads) {
            const QString wid = QString::fromLatin1(w.id);
            QVector<DetailCell> row{DetailCell::literal(QString::fromLatin1(w.name))};
            bool any = false;
            for (const QString &rid : runtimes) {
                const QString key = wid + QLatin1Char('.') + rid;
                any = any || r.metric(key);
                row.append(DetailCell::metric(key));
            }
            if (any)
                table.rows.append(row);
        }
        if (!table.rows.isEmpty())
            d.blocks.append({{}, table});
        d.blocks.append({{}, NoteBlock{QStringLiteral("Model names are left out on purpose. Models and precisions are Phase 6 open questions."),
                                       Tone::Warn}});
        return d;
    };
    return s;
}

} // namespace

AiTest::AiTest() : StubTest(makeSpec()) {}

} // namespace sb::tests

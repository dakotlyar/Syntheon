// SPDX-License-Identifier: GPL-3.0-or-later
// The NPU engine layer (Phase 4c design §4, §13–§15): the null engine is
// deterministic + sane (fixed fake seconds, realistic per-workload units,
// valid == units), and the real-vendor shims fail cleanly — makeEngine returns
// null for a missing runtime, and a present-but-unloadable/damaged runtime is a
// shim whose init() returns false with a clear error, all without any vendor
// SDK on the build box (P-VENDOR). The real inference loop is §16 validation.
#include "tests/npu/engine/engine.h"
#include "tests/npu/engine/engine_dlopen.h"
#include "tests/npu/engine/genai_runtime.h"
#include "tests/npu/workloads/workloads.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace sb::npu;

namespace {

LoadedModel modelFor(const QString &name)
{
    // Mirrors the §5.1 population in npu_test.cpp: `path` is the model file,
    // `dir` its cache directory (the parent of the first manifest file's path).
    LoadedModel m;
    m.name = name;
    m.precision = QStringLiteral("INT8");
    m.path = QStringLiteral("/fakes/models/") + name;
    m.dir = QFileInfo(m.path).absolutePath();
    return m;
}

bool sane(const Inference &inference)
{
    return inference.seconds > 0.0 && inference.units > 0 && inference.valid == inference.units;
}

// The per-platform GenAI module names the engine probes (engine_openvino.cpp
// genAiModuleName/genAiTokenizersName); the tests mirror them so the fixture
// layouts are realistic.
QString genAiName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("openvino_genai.dll");
#else
    return QStringLiteral("libopenvino_genai.so");
#endif
}

// Writes an empty file so the probe's existence check passes (the file content
// is irrelevant — the probe never reads it).
void touchFile(const QString &path)
{
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(QStringLiteral("cannot create %1").arg(path)));
    f.close();
}

// The per-platform openvino_tokenizers module name (engine_openvino.cpp
// genAiTokenizersName); the fixtures mirror the real pip wheel layout.
QString tokenizersName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("openvino_tokenizers.dll");
#else
    return QStringLiteral("libopenvino_tokenizers.so");
#endif
}

// A fake engine whose llm window reports EOS-on-step-1 (one token, then EOS —
// the §5.3 floor is not met): runWindows must turn it into an Error, never a
// scored rate.
class EosOnStepOneEngine final : public INpuEngine
{
public:
    QString backendName() const override { return QStringLiteral("fake-eos1"); }
    QString runtimeVersion() const override { return {}; }
    bool init(const LoadedModel &, QString *) override { return true; }
    bool runWindow(const QString &, const LoadedModel &, Inference &out, QString *,
                   const std::function<bool()> &) override
    {
        out.seconds = 1.0;
        out.units = 1;      // one token generated...
        out.valid = 0;      // ...then EOS on step 1 → below the §5.3 floor
        out.ttftSeconds = 0.2;
        return true;
    }
};

} // namespace

class TestNpuEngine : public QObject
{
    Q_OBJECT

private slots:
    void nullEngineBackendAndVersion()
    {
        auto engine = nullEngine();
        QVERIFY(engine);
        QCOMPARE(engine->backendName(), QStringLiteral("null"));
    }

    void nullEngineDeterministic()
    {
        // Two fresh null engines, same model, same seed → bit-identical windows
        // in the same order (std::mt19937's raw output is portable across the
        // supported platforms, so this holds on all of them).
        auto a = nullEngine();
        auto b = nullEngine();
        QString err;
        QVERIFY(a->init(modelFor(QStringLiteral("resnet50-int8")), &err));
        QVERIFY(b->init(modelFor(QStringLiteral("resnet50-int8")), &err));

        const char *workloads[] = {"classify", "detect", "llm"};
        for (const char *w : workloads) {
            for (int i = 0; i < 3; ++i) {
                Inference ia, ib;
                QVERIFY(a->runWindow(QString::fromLatin1(w), {}, ia, &err));
                QVERIFY(b->runWindow(QString::fromLatin1(w), {}, ib, &err));
                QCOMPARE(ia.seconds, ib.seconds);
                QCOMPARE(ia.units, ib.units);
                QCOMPARE(ia.valid, ib.valid);
            }
        }
    }

    void nullEngineSanePerWorkload()
    {
        auto engine = nullEngine();
        QString err;
        QVERIFY(engine->init(modelFor(QStringLiteral("any")), &err));

        struct Expect {
            const char *workload;
            std::int64_t minUnits;   // a realistic 5 s window floor
        };
        const Expect expects[] = {
            {"classify", 5000},   // ResNet-50 INT8: thousands of images/s
            {"detect", 3000},     // YOLOv8n INT8
            {"llm", 100},         // ~1.5 B INT4 generation
        };
        for (const Expect &e : expects) {
            Inference out;
            QVERIFY(engine->runWindow(QString::fromLatin1(e.workload), {}, out, &err));
            QCOMPARE(out.seconds, 5.0);        // the fixed fake window
            QVERIFY2(out.units >= e.minUnits, qPrintable(QStringLiteral("%1 units too low: %2")
                                                             .arg(QString::fromLatin1(e.workload))
                                                             .arg(out.units)));
            QCOMPARE(out.valid, out.units);    // every synthetic inference passes the sentinel
        }
    }

    void makeEngineNullReturnsNullEngine()
    {
        QString err;
        auto engine = makeEngine(EngineVendor::Null, QString(), &err);
        QVERIFY(engine);
        QCOMPARE(engine->backendName(), QStringLiteral("null"));
    }

    void makeEngineMissingPathFails()
    {
        QString err;
        QVERIFY(!makeEngine(EngineVendor::Intel, QString(), &err));
        QVERIFY(!err.isEmpty());

        err.clear();
        QVERIFY(!makeEngine(EngineVendor::Amd, QString(), &err));
        QVERIFY(!err.isEmpty());
    }

    void openVinoMissingRuntimeFailsClean()
    {
        // A present-looking path whose runtime isn't loadable: makeEngine returns
        // the shim (the caller knows a runtime *path* was resolved), and init()
        // fails cleanly with a clear error — nothing throws, nothing crashes.
        QString err;
        auto engine = makeEngine(EngineVendor::Intel, QStringLiteral("/nonexistent/openvino"), &err);
        QVERIFY2(engine.get(), qPrintable(err));
        QCOMPARE(engine->backendName(), QStringLiteral("openvino-npu"));
        QVERIFY(!engine->init(modelFor(QStringLiteral("resnet50-int8")), &err));
        QVERIFY(!err.isEmpty());
    }

    void amdMissingRuntimeFailsClean()
    {
        QString err;
        auto engine = makeEngine(EngineVendor::Amd, QStringLiteral("/nonexistent/ryzenai"), &err);
        QVERIFY2(engine.get(), qPrintable(err));
        QCOMPARE(engine->backendName(), QStringLiteral("ryzenai-vitis"));
        QVERIFY(!engine->init(modelFor(QStringLiteral("resnet50-int8")), &err));
        QVERIFY(!err.isEmpty());
    }

    void qualcommPlatformGate()
    {
        // QNN runs only on Windows-on-ARM (N4). Everywhere else makeEngine
        // reports it unsupported; on a Windows-on-ARM build it returns the shim
        // (whose init() then fails cleanly with no runtime — not exercised here).
        QString err;
#if !defined(SYNTHEON_NPU_QNN_AVAILABLE)
        auto engine = makeEngine(EngineVendor::Qualcomm, QStringLiteral("/qaart"), &err);
        QVERIFY(!engine);
        QVERIFY(err.contains(QLatin1StringView("Windows on ARM64")));
#else
        auto engine = makeEngine(EngineVendor::Qualcomm, QStringLiteral("/qaart"), &err);
        QVERIFY2(engine.get(), qPrintable(err));
        QCOMPARE(engine->backendName(), QStringLiteral("qnn"));
        QVERIFY(!engine->init(modelFor(QStringLiteral("llm-1b-int4")), &err));
        QVERIFY(!err.isEmpty());
#endif
    }

    void loadedModelCarriesDir()
    {
        // §5.1: LoadedModel gains `dir` (the per-model cache directory) alongside
        // `path`, populated by npu_test.cpp as the parent of the manifest file.
        const LoadedModel m = modelFor(QStringLiteral("llm-1b-int4"));
        QCOMPARE(m.dir, QStringLiteral("/fakes/models"));
        QCOMPARE(m.path, QStringLiteral("/fakes/models/llm-1b-int4"));
        // The null engine accepts the 4-member model and still runs "llm".
        auto engine = nullEngine();
        QString err;
        QVERIFY(engine->init(m, &err));
        Inference out;
        QVERIFY(engine->runWindow(QStringLiteral("llm"), m, out, &err));
        QVERIFY(sane(out));
    }

    void nullEngineIgnoresModelPathExistence()
    {
        // The null engine never touches disk: init() succeeds regardless of the
        // (fake) model path — that's the whole point of the CI/dev fake.
        auto engine = nullEngine();
        QString err;
        QVERIFY(engine->init(modelFor(QStringLiteral("does-not-exist")), &err));
        Inference out;
        QVERIFY(engine->runWindow(QStringLiteral("classify"), {}, out, &err));
        QVERIFY(sane(out));
    }

    // L3: exercise the dlopen/LoadLibrary shim against a REAL system library so
    // the hardened DLL-search path (LoadLibraryExW + LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
    // and resolve<F>() actually run on windows-x64 CI before any vendor hardware.
    void realSystemLibraryLoadsAndResolves()
    {
#if defined(Q_OS_WIN)
        // The shim normalises paths to absolute; kernel32.dll needs its full
        // System32 path (a bare name would become $CWD/kernel32.dll and miss).
        const QString lib = QDir(qEnvironmentVariable("SystemRoot"))
                                .filePath(QStringLiteral("System32/kernel32.dll"));
        using Fn = std::uint32_t(__stdcall *)(void);
        const char *sym = "GetCurrentProcessId";
#else
        const QString lib = QStringLiteral("libm.so.6");
        using Fn = double (*)(double);
        const char *sym = "cos";
#endif
        QString err;
        engine_internal::Library library(lib, &err);
        QVERIFY2(library.isOpen(), qPrintable(err));
        const Fn fn = engine_internal::resolve<Fn>(library, sym, &err);
        QVERIFY2(fn != nullptr, qPrintable(err));
    }

    // The GenAI second-runtime probe (DD-1 §5.4): genAiRuntimeFile resolves the
    // unversioned file first, then a `name.*` SONAME (pip wheels keep only the
    // versioned .so). Mirrors the engine's runtimeFile() loop-verify.
    void genAiRuntimeFilePreferUnversioned()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString flat = QDir(dir.path()).filePath(genAiName());
        touchFile(flat);
        QCOMPARE(genAiRuntimeFile(dir.path(), genAiName()), flat);
        // A SONAME-only dir still resolves (the versioned file the wheel ships).
        QTemporaryDir sonameDir;
        QVERIFY(sonameDir.isValid());
#if defined(Q_OS_WIN)
        // Windows DLLs are unversioned; the SONAME case is a Linux/pip concern.
        QCOMPARE(genAiRuntimeFile(sonameDir.path(), genAiName()), QString());
#else
        touchFile(QDir(sonameDir.path()).filePath(genAiName() + QStringLiteral(".2641")));
        QCOMPARE(genAiRuntimeFile(sonameDir.path(), genAiName()),
                 QDir(sonameDir.path()).filePath(genAiName() + QStringLiteral(".2641")));
#endif
        // An empty/missing dir resolves to "" — never a bogus path.
        QCOMPARE(genAiRuntimeFile(QDir::tempPath() + QStringLiteral("/definitely-not-a-dir"),
                                  genAiName()),
                 QString());
    }

    // findGenAiModule: the REAL pip layouts (2026.4.1.0 wheel, AP-5 2/10) —
    // runtimePath is <py>/site-packages/openvino/libs; the genai module sits
    // FLAT in its package dir (<py>/site-packages/openvino_genai/, no lib/),
    // while the tokenizers module sits under <py>/site-packages/
    // openvino_tokenizers/lib/. Both must resolve; a missing package must be
    // "" so the engine fails cleanly with "LLM runtime missing".
    void findGenAiModulePipSiblingLayout()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString openvinoLibs = dir.path() + QStringLiteral("/site-packages/openvino/libs");
        const QString genaiPkg = dir.path() + QStringLiteral("/site-packages/openvino_genai");
        const QString tokPkg = dir.path() + QStringLiteral("/site-packages/openvino_tokenizers");
        QVERIFY(QDir().mkpath(openvinoLibs));
        QVERIFY(QDir().mkpath(genaiPkg));
        QVERIFY(QDir().mkpath(tokPkg + QStringLiteral("/lib")));

        // openvino_genai keeps its module FLAT in the package dir.
        const QString genaiFlat = QDir(genaiPkg).filePath(genAiName());
        touchFile(genaiFlat);
        QCOMPARE(findGenAiModule(openvinoLibs, QStringLiteral("openvino_genai"), genAiName()), genaiFlat);
        // ...but a lib/ subdir variant is probed too (a toolkit-style package).
        QVERIFY(QDir().mkpath(genaiPkg + QStringLiteral("/lib")));
        const QString genaiLib = QDir(genaiPkg + QStringLiteral("/lib")).filePath(genAiName());
        touchFile(genaiLib);
        QCOMPARE(findGenAiModule(openvinoLibs, QStringLiteral("openvino_genai"), genAiName()), genaiLib);
        // openvino_tokenizers keeps its module under lib/.
        const QString tokLib = QDir(tokPkg + QStringLiteral("/lib")).filePath(tokenizersName());
        touchFile(tokLib);
        QCOMPARE(findGenAiModule(openvinoLibs, QStringLiteral("openvino_tokenizers"), tokenizersName()), tokLib);

        // The flat layout (toolkit installs: genai sits beside openvino.dll)
        // wins even when a pip sibling also exists.
        const QString flat = QDir(openvinoLibs).filePath(genAiName());
        touchFile(flat);
        QCOMPARE(findGenAiModule(openvinoLibs, QStringLiteral("openvino_genai"), genAiName()), flat);

        // A package that is not installed resolves to "" — never a guessed path.
        // (Probe with the tokenizers name: the flat runtime-dir check is
        // name-based, so probing with the genai name would find the flat genai
        // file created for the "flat wins" assertion above.)
        QCOMPARE(findGenAiModule(openvinoLibs, QStringLiteral("openvino_quantizer"), tokenizersName()), QString());
    }

    // The null engine's llm path is untouched by the GenAI flip (the LLM window
    // contract — units/seconds/ttft/valid — is the same for every engine).
    void nullEngineLlmWindowContract()
    {
        auto engine = nullEngine();
        QString err;
        QVERIFY(engine->init(modelFor(QStringLiteral("llm-1b-int4")), &err));
        Inference out;
        QVERIFY(engine->runWindow(QStringLiteral("llm"), modelFor(QStringLiteral("llm-1b-int4")), out, &err));
        QVERIFY(sane(out));
        QVERIFY(out.ttftSeconds > 0.0);   // the LLM window reports a first-token time
    }

    // §5.3 correctness sentinel through the workload layer (AP-5 10): a window
    // whose engine reports EOS-on-step-1 (units=1, valid=0) must be an Error —
    // a "tokens > 0 but garbage rate" is never scored.
    void llmEosOnStepOneIsError()
    {
        EosOnStepOneEngine engine;
        const sb::npu::workloads::WorkloadResult wr =
            sb::npu::workloads::runWindows(QStringLiteral("llm"), engine, modelFor(QStringLiteral("llm-1b-int4")), {});
        QCOMPARE(wr.status, sb::npu::workloads::WorkloadStatus::Error);
        QVERIFY(!wr.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestNpuEngine)
#include "tst_npu_engine.moc"

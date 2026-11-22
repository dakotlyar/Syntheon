// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenVINO NPU engine (Intel AI Boost), Phase 4c design §4.1. Loads the
// USER-INSTALLED OpenVINO runtime from the path detect resolved: the C++ core
// (openvino.dll / libopenvino.so), the C API (openvino_c.dll / libopenvino_c.so)
// and the Intel NPU plugin (openvino_intel_npu_plugin.dll /
// libopenvino_intel_npu_plugin.so). The `ov_*` C
// entry points are resolved by name from the C API library (not the C++ core),
// the model is compiled for device "NPU", and a sync-infer measurement window is
// run. No vendor header or .lib is compiled in (P-VENDOR); a missing/damaged
// runtime is a clean init() == false + a clear error, never a throw or a crash.
//
// The LLM workload (4c-5) runs through OpenVINO GenAI as a SECOND runtime
// (DD-1 §5.4 Option A): the compile-first smoke proved the raw-C KV loop cannot
// compile the stateful IR on the NPU, while LLMPipeline(dir, "NPU") does. The
// engine dlopens the user's openvino_genai + a small shim module (genai_shim.*,
// the P-VENDOR amendment — GenAI headers are a build input, the runtime stays
// user-installed, never bundled) and drives generation through the shim's C
// boundary. The GenAI load is lazy: only the llm workload touches it, so a box
// without GenAI still runs classify/detect and fails the LLM cleanly with an
// "LLM runtime missing" error. The vision (classify/detect) raw-C path is
// byte-for-byte unchanged from 4c-3.
//
// The C API below is the OpenVINO 2.x C ABI (openvino/c/ov_core.h, ov_tensor.h,
// ov_infer_request.h, ov_compiled_model.h, ov_common.h), pinned to the 2023.x+
// release lines and verified against those headers at fix-round 4c-3 — the
// signatures here are the real ones, not guesses. Every resolve is null-checked,
// so a mismatched symbol fails init() cleanly with the symbol name rather than
// crashing.

#include "tests/npu/engine/engine.h"
#include "tests/npu/engine/engine_dlopen.h"
#include "tests/npu/engine/genai_runtime.h"
#include "tests/npu/engine/genai_shim.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sb::npu {

namespace {

using namespace sb::npu::engine_internal;

// Opaque OpenVINO 2.x C types (the real definitions live in the runtime).
struct ov_core;
struct ov_model;
struct ov_compiled_model;
struct ov_infer_request;
struct ov_tensor;

// ov_version_t (ov_core.h): both string members are owned by the runtime; free
// the whole struct with ov_version_free.
struct ov_version {
    const char *buildNumber;
    const char *description;
};

// ov_available_devices_t (ov_core.h, 2026.x C ABI): the device list returned
// by ov_core_get_available_devices; freed with ov_available_devices_free.
struct ov_available_devices_t {
    char **devices;
    std::size_t size;
};

constexpr int kOk = 0;   // ov_status_e::OK
constexpr double kWindowSeconds = 5.0;

// The LLM generation bounds (Phase 4c §5.3): a hard cap on new tokens per
// window so a never-ending generation can't spin past the 5 s window (a 5 s
// window at a realistic ~55 tok/s ≈ 275 tokens, so 256 is the ceiling), and
// the §5.3 correctness floor — a window is `valid` only if generation reached
// ≥ 8 new tokens before EOS; a model that emits EOS on step 1 is an Error,
// never a scored rate.
constexpr std::int64_t kLlmMaxNewTokens = 256;
constexpr std::int64_t kLlmMinNewTokens = 8;

// The fixed prompt the scored LLM loop generates from — the DD's "Paris"
// check, and the exact call the Nivedellir smoke proved on the pack's IR
// (LLMPipeline(dir, "NPU"), then generate("The capital of France is",
// max_new_tokens=N)). A fixed prompt keeps windows deterministic (§8); GenAI
// tokenizes it internally (CPU) and applies the model dir's chat template, as
// the smoke did.
constexpr const char *kLlmPrompt = "The capital of France is";

// The resolved C entry points. All are null-checked at resolve time.
struct OpenVinoApi {
    int (*coreCreate)(ov_core **core) = nullptr;
    void (*coreFree)(ov_core *core) = nullptr;
    int (*readModel)(const ov_core *core, const char *model_path, const char *bin_path, ov_model **model) = nullptr;
    int (*readModelUnicode)(const ov_core *core, const wchar_t *model_path, const wchar_t *bin_path,
                            ov_model **model) = nullptr;   // Windows-only (wide path, L1)
    // C variadic: the trailing ... is part of the type and must not be dropped
    // (calling through a non-variadic fnptr is UB).
    int (*compileModel)(const ov_core *core, const ov_model *model, const char *device_name,
                        std::size_t property_args_size, ov_compiled_model **compiled_model, ...) = nullptr;
    void (*modelFree)(ov_model *model) = nullptr;
    void (*compiledModelFree)(ov_compiled_model *model) = nullptr;
    int (*createInferRequest)(const ov_compiled_model *model, ov_infer_request **request) = nullptr;
    int (*getInputTensor)(const ov_infer_request *request, std::size_t idx, ov_tensor **tensor) = nullptr;
    int (*infer)(ov_infer_request *request) = nullptr;
    int (*getOutputTensor)(const ov_infer_request *request, std::size_t idx, ov_tensor **tensor) = nullptr;
    int (*tensorData)(const ov_tensor *tensor, void **data) = nullptr;             // writes *data
    int (*tensorByteSize)(const ov_tensor *tensor, std::size_t *byte_size) = nullptr;
    void (*tensorFree)(ov_tensor *tensor) = nullptr;                               // frees a by_index wrapper
    void (*inferRequestFree)(ov_infer_request *request) = nullptr;
    int (*getVersion)(ov_version *version) = nullptr;      // optional: absent → "" version
    void (*versionFree)(ov_version *version) = nullptr;    // optional
    // The fail-closed CPU-fallback guard (EC-P9): the LLM path verifies the
    // OpenVINO core still reports an NPU device (GenAI's LLMPipeline does not
    // expose EXECUTION_DEVICES). Both 2026.x C ABI, null-checked at resolve time.
    int (*coreGetAvailableDevices)(const ov_core *core, ov_available_devices_t *devices) = nullptr;
    void (*availableDevicesFree)(ov_available_devices_t *devices) = nullptr;
};

} // namespace

namespace {

class OpenVinoEngine final : public INpuEngine
{
public:
    explicit OpenVinoEngine(QString runtimePath) : m_runtimePath(std::move(runtimePath)) {}
    ~OpenVinoEngine() override { unload(); }   // release ov handles before the Library dtors dlclose

    QString backendName() const override { return QStringLiteral("openvino-npu"); }
    // The resolved runtime version: the OpenVINO (core) description — and, once
    // the LLM workload has lazily loaded it, the GenAI runtime's own version
    // appended, so the Detail page's Runtime row shows both (§4.1/§9).
    QString runtimeVersion() const override
    {
        if (m_genAiVersion.isEmpty())
            return m_version;
        return m_version.isEmpty() ? m_genAiVersion
                                   : m_version + QStringLiteral(" · GenAI ") + m_genAiVersion;
    }

    bool init(const LoadedModel &model, QString *error) override
    {
        // Release any prior run's ov handles before (re)loading, so the library
        // handles handed to the move-assignments below are never dlclose'd while
        // NPU device handles/threads from the previous run are still live.
        unload();

        // The plugin (openvino_intel_npu_plugin.{dll,so}) must sit beside the
        // core (detect verified that); holding its handle keeps it loaded for the
        // core's device loader. The C API library (openvino_c) exports the ov_*
        // entry points; the C++ core stays loaded as its dependency.
        auto core = std::make_unique<Library>(resolvePath(coreName(), false), error);
        if (!core->isOpen())
            return false;
        auto capi = std::make_unique<Library>(resolvePath(cApiName(), false), error);
        if (!capi->isOpen())
            return false;
        auto plugin = std::make_unique<Library>(resolvePath(pluginName(), true), error);
        if (!plugin->isOpen())
            return false;

        m_core = std::move(core);
        m_cApi = std::move(capi);
        m_plugin = std::move(plugin);
        return load(model, error);
    }

    bool runWindow(const QString &workload, const LoadedModel &model, Inference &out, QString *error,
                   const std::function<bool()> &cancelled) override
    {
        out.seconds = 0.0;
        out.units = 0;
        out.valid = 0;
        out.ttftSeconds = 0.0;
        if (!m_ready)
            return failWith(QStringLiteral("the OpenVINO engine is not initialised"), error);
        // Per-workload reload (4c-5): each workload compiles its own model, so the
        // engine is not welded to the classify model init() loaded. Reload when
        // the requested model differs from the one currently compiled — a
        // classify→detect path change, or a switch to/from the LLM decoder (which
        // uses a different file inside model.dir and a different device property).
        // Never score a window on the wrong model (EC-P +2 silent-wrong).
        const bool llm = (workload == QLatin1StringView("llm"));
        const QString neededPath = llm ? llmDecoderPath(model) : model.path;
        if (llm != m_loadedIsLlm || neededPath != m_loadedPath) {
            if (!compileModel(model, llm, error))
                return false;
        }
        if (llm)
            return runLlmWindow(out, error, cancelled);

        // One deterministic synthetic input buffer for the whole run: the same
        // fixed byte pattern on every window so windows are comparable (design §8).
        // The workload layer's precise image/token construction lands in task 11;
        // here a fixed, non-zero buffer drives the raw inference loop.
        if (!prepareInput(error))
            return failWith(QStringLiteral("failed to build the synthetic inference input"), error);

        const auto start = std::chrono::steady_clock::now();
        const auto until = start + std::chrono::milliseconds(int(kWindowSeconds * 1000.0));
        while (std::chrono::steady_clock::now() < until) {
            // Poll the cancel predicate between inferences so a Stop breaks the
            // loop within one inference (P-STOP ≤ 250 ms) — a clean break, not a
            // failure.
            if (cancelled && cancelled())
                break;
            if (m_api.infer(m_request) != kOk)
                return failWith(QStringLiteral("an inference failed on the NPU"), error);
            ++out.units;
            if (outputSanity())
                ++out.valid;
        }
        // The real elapsed span, not kWindowSeconds: the final inference may land
        // just before or just after the 5 s edge, and stamping 5.0 would inflate
        // the rate by up to ~10 % for slower (detect/llm) workloads.
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return true;
    }

private:
    static QString coreName()
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("openvino.dll");
#else
        return QStringLiteral("libopenvino.so");
#endif
    }
    static QString cApiName()
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("openvino_c.dll");
#else
        return QStringLiteral("libopenvino_c.so");
#endif
    }
    static QString pluginName()
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("openvino_intel_npu_plugin.dll");
#else
        // OpenVINO's Linux NPU plugin is a shared MODULE named with the `lib`
        // prefix (CMAKE_SHARED_MODULE_PREFIX = "lib"; ov::util::make_plugin_library_name()
        // prepends it on non-Windows) → libopenvino_intel_npu_plugin.so.
        return QStringLiteral("libopenvino_intel_npu_plugin.so");
#endif
    }
    static QString libPath(const QString &dir, const QString &name) { return QDir(dir).filePath(name); }

    // The absolute dlopen path for `name`. Windows loads the exact DLL beside the
    // runtime path; Linux resolves the actual file detect_linux.cpp matched — the
    // unversioned name (tarball / -dev namelink) or a `name.*` SONAME (APT/DEB,
    // RPM, pip), and for the NPU plugin also `openvino-*/name` (OV_CPACK_PLUGINSDIR)
    // — so the probe and this loader agree on the exact path (M1).
    QString resolvePath(const QString &name, bool plugin)
    {
#if defined(Q_OS_WIN)
        Q_UNUSED(plugin);
        return libPath(m_runtimePath, name);
#else
        return runtimeFile(m_runtimePath, name, plugin);
#endif
    }

#if !defined(Q_OS_WIN)
    static QString runtimeFile(const QString &dir, const QString &name, bool plugin)
    {
        const QString flat = QDir(dir).filePath(name);
        if (QFileInfo::exists(flat))
            return flat;
        const QDir d(dir);
        // SONAME form name.<ver> (the runtime packages / pip wheel keep only the
        // versioned file; the unversioned .so is a -dev namelink).
        const QStringList sonames = d.entryList({name + QStringLiteral(".*")}, QDir::Files,
                                                QDir::Name | QDir::Reversed);
        if (!sonames.isEmpty())
            return QDir(dir).filePath(sonames.first());
        // The NPU plugin lives under ${LIBDIR}/openvino-<ver>/ on packages, not flat.
        if (plugin) {
            const QStringList subs = d.entryList({QStringLiteral("openvino-*")}, QDir::Dirs
                                                       | QDir::NoDotAndDotDot,
                                                 QDir::Name | QDir::Reversed);
            for (const QString &sub : subs) {
                const QString p = QDir(dir).filePath(sub + QStringLiteral("/") + name);
                if (QFileInfo::exists(p))
                    return p;
            }
        }
        return flat;   // not found: the Library ctor reports a clean load error
    }
#endif

    Library &cApi() { return *m_cApi; }

    // Resolves every entry point from the C API library, short-circuiting on the
    // FIRST missing symbol so *error always names the first failure (L2).
    bool resolveApi(Library &capi, QString *error)
    {
        m_api.coreCreate = resolve<decltype(m_api.coreCreate)>(capi, "ov_core_create", error);
        if (!m_api.coreCreate)
            return false;
        m_api.coreFree = resolve<decltype(m_api.coreFree)>(capi, "ov_core_free", error);
        if (!m_api.coreFree)
            return false;
#if defined(_WIN32)
        // Non-ASCII %LOCALAPPDATA%: the wide fixer used below needs the unicode
        // read-model entry point (L1); never route through CP_ACP on Windows.
        m_api.readModelUnicode = resolve<decltype(m_api.readModelUnicode)>(capi, "ov_core_read_model_unicode", error);
        if (!m_api.readModelUnicode)
            return false;
#else
        m_api.readModel = resolve<decltype(m_api.readModel)>(capi, "ov_core_read_model", error);
        if (!m_api.readModel)
            return false;
#endif
        m_api.compileModel = resolve<decltype(m_api.compileModel)>(capi, "ov_core_compile_model", error);
        if (!m_api.compileModel)
            return false;
        m_api.modelFree = resolve<decltype(m_api.modelFree)>(capi, "ov_model_free", error);
        if (!m_api.modelFree)
            return false;
        m_api.compiledModelFree = resolve<decltype(m_api.compiledModelFree)>(capi, "ov_compiled_model_free", error);
        if (!m_api.compiledModelFree)
            return false;
        m_api.createInferRequest = resolve<decltype(m_api.createInferRequest)>(capi,
                                                                               "ov_compiled_model_create_infer_request",
                                                                               error);
        if (!m_api.createInferRequest)
            return false;
        m_api.getInputTensor = resolve<decltype(m_api.getInputTensor)>(capi,
                                                                       "ov_infer_request_get_input_tensor_by_index",
                                                                       error);
        if (!m_api.getInputTensor)
            return false;
        m_api.infer = resolve<decltype(m_api.infer)>(capi, "ov_infer_request_infer", error);
        if (!m_api.infer)
            return false;
        m_api.getOutputTensor = resolve<decltype(m_api.getOutputTensor)>(capi,
                                                                         "ov_infer_request_get_output_tensor_by_index",
                                                                         error);
        if (!m_api.getOutputTensor)
            return false;
        m_api.tensorData = resolve<decltype(m_api.tensorData)>(capi, "ov_tensor_data", error);
        if (!m_api.tensorData)
            return false;
        m_api.tensorByteSize = resolve<decltype(m_api.tensorByteSize)>(capi, "ov_tensor_get_byte_size", error);
        if (!m_api.tensorByteSize)
            return false;
        m_api.tensorFree = resolve<decltype(m_api.tensorFree)>(capi, "ov_tensor_free", error);
        if (!m_api.tensorFree)
            return false;
        m_api.inferRequestFree = resolve<decltype(m_api.inferRequestFree)>(capi, "ov_infer_request_free", error);
        if (!m_api.inferRequestFree)
            return false;
        m_api.coreGetAvailableDevices =
            resolve<decltype(m_api.coreGetAvailableDevices)>(capi, "ov_core_get_available_devices", error);
        if (!m_api.coreGetAvailableDevices)
            return false;
        m_api.availableDevicesFree =
            resolve<decltype(m_api.availableDevicesFree)>(capi, "ov_available_devices_free", error);
        if (!m_api.availableDevicesFree)
            return false;

        // Version is optional: resolve into a sink so an absence isn't an error.
        QString versionErr;
        m_api.getVersion = resolve<decltype(m_api.getVersion)>(capi, "ov_get_openvino_version", &versionErr);
        m_api.versionFree = resolve<decltype(m_api.versionFree)>(capi, "ov_version_free", &versionErr);
        return true;
    }

    bool load(const LoadedModel &model, QString *error)
    {
        if (error)
            error->clear();
        if (!resolveApi(cApi(), error))
            return false;

        m_version = readVersion();

        if (m_api.coreCreate(&m_coreHandle) != kOk || !m_coreHandle)
            return failLoad(QStringLiteral("ov_core_create failed"), error);

        return compileModel(model, false, error);
    }

    // The stateful LLM decoder's primary IR, found BY NAME inside the model's
    // cache directory (§5.1) — never via files.first() ordering, which cannot
    // identify the decoder among the multi-file LLM pack.
    // SMOKE: the decoder file name inside model.dir ("openvino_model.xml") from
    // the pack inspection; confirm the actual IR file name on Nivedellir.
    static QString llmDecoderPath(const LoadedModel &model)
    {
        if (model.dir.isEmpty())
            return QString();
        return QDir(model.dir).filePath(QStringLiteral("openvino_model.xml"));
    }

    // Reads + compiles + requests `model`, releasing any previously compiled
    // model first (the core + libraries survive — this is the per-workload
    // reload path). `llm` requests the NPUW device property for the decoder and
    // records the decoder path as m_loadedPath. On failure it unwinds everything
    // (failLoad → unload) so the engine is cleanly uninitialised, never half-loaded.
    bool compileModel(const LoadedModel &model, bool llm, QString *error)
    {
        freeModel();   // the previous model's handles; the core stays live
        const QString xml = llm ? llmDecoderPath(model) : model.path;

        int readStatus = kOk;
#if defined(_WIN32)
        // Wide path so a non-ASCII profile/app-data dir doesn't fail in CP_ACP (L1).
        const std::wstring wpath = xml.toStdWString();
        readStatus = m_api.readModelUnicode(m_coreHandle, wpath.c_str(), nullptr, &m_model);
#else
        readStatus = m_api.readModel(m_coreHandle, xml.toUtf8().constData(), nullptr, &m_model);
#endif
        if (readStatus != kOk || !m_model)
            return failLoad(QStringLiteral("ov_core_read_model failed for %1").arg(xml), error);

        if (llm) {
            // The LLM workload runs through OpenVINO GenAI (DD-1 §5.4 Option A —
            // the smoke proved the raw-C KV loop cannot compile the stateful IR
            // on the NPU; GenAI staticizes the KV state internally). "Compiling"
            // the LLM here means lazily loading the GenAI second runtime + our
            // shim, creating LLMPipeline(model.dir, "NPU"), and running the
            // fail-closed CPU-fallback guard. The vision path above (classify/
            // detect) is byte-for-byte unchanged.
            if (!loadGenAi(error))
                return failLoad(QStringLiteral("the LLM runtime is unavailable: %1").arg(*error), error);
            if (!createLlmPipeline(model, error))
                return failLoad(QStringLiteral("failed to build the LLM pipeline: %1").arg(*error), error);
            // The GenAI pipeline owns its requests — there is no vision
            // infer_request to build for the llm workload (m_compiled is null).
        } else {
            // 0 property args + no variadic pairs → compiles the model with defaults.
            if (m_api.compileModel(m_coreHandle, m_model, "NPU", 0, &m_compiled) != kOk || !m_compiled)
                return failLoad(QStringLiteral("ov_core_compile_model failed on device 'NPU'"), error);
            if (m_api.createInferRequest(m_compiled, &m_request) != kOk || !m_request)
                return failLoad(QStringLiteral("ov_compiled_model_create_infer_request failed"), error);
        }

        m_loadedPath = xml;
        m_loadedIsLlm = llm;
        m_ready = true;
        return true;
    }

    // The module's version string, or "" when the (optional) symbol is absent or
    // the call fails. ov_get_openvino_version returns ov_status_e; only read/free
    // the struct when it succeeded (C1).
    QString readVersion()
    {
        if (!m_api.getVersion)
            return QString();
        ov_version v{};
        if (m_api.getVersion(&v) != kOk)
            return QString();
        QString out;
        if (v.description)
            out = QString::fromUtf8(v.description);
        if (out.isEmpty() && v.buildNumber)
            out = QString::fromUtf8(v.buildNumber);
        if (m_api.versionFree)
            m_api.versionFree(&v);
        return out;
    }

    // Fills the input tensor (index 0) with a fixed deterministic pattern.
    bool prepareInput(QString *error)
    {
        ov_tensor *input = nullptr;
        if (m_api.getInputTensor(m_request, 0, &input) != kOk || !input)
            return failWith(QStringLiteral("failed to get the input tensor"), error);

        void *data = nullptr;
        if (m_api.tensorData(input, &data) != kOk || !data) {
            m_api.tensorFree(input);
            return failWith(QStringLiteral("the input tensor has no data buffer"), error);
        }
        std::size_t bytes = 0;
        if (m_api.tensorByteSize(input, &bytes) != kOk) {
            m_api.tensorFree(input);
            return failWith(QStringLiteral("failed to read the input tensor size"), error);
        }
        // A deterministic (byte i → i & 0xFF) non-zero fill, not all-zero, so a
        // silent "always-zero" output is still distinguishable from real work.
        std::uint8_t *p = static_cast<std::uint8_t *>(data);
        for (std::size_t i = 0; i < bytes; ++i)
            p[i] = std::uint8_t(i);
        m_api.tensorFree(input);   // the wrapper, not the request's buffer
        return true;
    }

    // The §2 correctness sentinel, reduced to an output-presence check here: the
    // precise argmax-range / box-count / token-count checks live in the workload
    // layer (task 11), which owns model shape knowledge. The engine counts an
    // inference valid when its output tensor is non-empty. The by_index getter
    // heap-allocates a fresh wrapper each call → free it every time (C1 leak).
    bool outputSanity()
    {
        ov_tensor *output = nullptr;
        if (m_api.getOutputTensor(m_request, 0, &output) != kOk || !output)
            return false;
        std::size_t bytes = 0;
        const int status = m_api.tensorByteSize(output, &bytes);
        m_api.tensorFree(output);
        return status == kOk && bytes > 0;
    }

    // ---- LLM via OpenVINO GenAI (DD-1 §5.4 Option A) ----

    // The second runtime the Intel engine loads lazily for the LLM workload
    // (P-VENDOR as amended): openvino_genai from the USER's install (probed
    // beside the OpenVINO runtime dir), plus OUR shim module that wraps
    // ov::genai::LLMPipeline behind the stable C boundary in genai_shim.h. The
    // shim is built only when a GenAI SDK dir is configured; a missing runtime
    // OR shim is a clean "LLM runtime missing" error on the llm path only —
    // classify/detect never touch these, so a box without GenAI still runs the
    // vision workloads (never a launch failure).
    static QString genAiModuleName()
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("openvino_genai.dll");
#else
        return QStringLiteral("libopenvino_genai.so");
#endif
    }
    static QString genAiTokenizersName()
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("openvino_tokenizers.dll");
#else
        return QStringLiteral("libopenvino_tokenizers.so");
#endif
    }
    // The shim is OUR module (GPL-3.0 code built with the GenAI headers when
    // SYNTHEON_OPENVINO_GENAI_SDK_DIR is configured); it rides next to the app
    // module like any other Syntheon library.
    static QString shimModulePath()
    {
        const QString name =
#if defined(Q_OS_WIN)
            QStringLiteral("syn_openvino_genai_shim.dll");
#else
            QStringLiteral("libsyn_openvino_genai_shim.so");
#endif
        return QDir(QCoreApplication::applicationDirPath()).filePath(name);
    }

    // The engine ↔ shim C ABI, resolved by name from the loaded shim module
    // (genai_shim.h). Each symbol is null-checked at resolve time like
    // resolveApi(), so a damaged shim is a clean error naming the symbol.
    struct GenAiApi {
        genai_shim::SynGenAiCreateFn create = nullptr;
        genai_shim::SynGenAiGenerateFn generate = nullptr;
        genai_shim::SynGenAiDeleteFn destroy = nullptr;
    };

    // Lazy load of the GenAI runtime + shim, once per engine (m_genAi set on
    // success). Load order is load-bearing: openvino_genai FIRST (Linux:
    // RTLD_GLOBAL so the shim's undefined ov::genai::* symbols resolve against
    // it; Windows: already-loaded openvino.dll/ov dependencies bind by name),
    // then the shim, whose own dependency on openvino_genai binds to the
    // already-loaded module. On any failure nothing is half-installed.
    // SMOKE: the openvino_tokenizers extension is found via the probe and
    // handed to the shim (it sets OPENVINO_TOKENIZERS_PATH_GENAI so GenAI's
    // Tokenizer loads it from the pip sibling dir); confirm on Nivedellir that
    // the pipeline builds with the pip-installed wheel layout.
    bool loadGenAi(QString *error)
    {
        if (m_genAi)
            return true;   // already loaded (a previous llm compile succeeded)
        const QString genaiPath = findGenAiModule(m_runtimePath, QStringLiteral("openvino_genai"), genAiModuleName());
        if (genaiPath.isEmpty()) {
            // SMOKE: "not found" = neither flat beside the OpenVINO runtime dir
            // nor a <site-packages>/openvino_genai/lib/ sibling (pip layout).
            return failWith(
                QStringLiteral(
                    "LLM runtime missing: OpenVINO GenAI (openvino_genai) was not found beside the OpenVINO runtime "
                    "— install the OpenVINO GenAI runtime 2026.4 (pip: openvino-genai==2026.4.1.0) for the LLM workload"),
                error);
        }
        const QString tokenizersPath =
            findGenAiModule(m_runtimePath, QStringLiteral("openvino_tokenizers"), genAiTokenizersName());

        QString loadError;
        auto genai = std::make_unique<Library>(genaiPath, &loadError, /*globalScope*/ true);
        if (!genai->isOpen())
            return failWith(QStringLiteral("LLM runtime missing: failed to load %1: %2").arg(genaiPath, loadError), error);

        const QString shimPath = shimModulePath();
        auto shim = std::make_unique<Library>(shimPath, &loadError);
        if (!shim->isOpen())
            return failWith(QStringLiteral("LLM runtime missing: the GenAI shim %1 could not be loaded — this build "
                                           "has no OpenVINO GenAI support (rebuild with SYNTHEON_OPENVINO_GENAI_SDK_DIR): %2")
                                .arg(shimPath, loadError),
                            error);

        GenAiApi api;
        api.create = resolve<genai_shim::SynGenAiCreateFn>(*shim, "syn_genai_pipeline_create", &loadError);
        if (!api.create)
            return failWith(QStringLiteral("LLM runtime missing: the GenAI shim is damaged — failed to resolve "
                                           "syn_genai_pipeline_create in %1: %2")
                                .arg(shimPath, loadError),
                            error);
        api.generate = resolve<genai_shim::SynGenAiGenerateFn>(*shim, "syn_genai_generate", &loadError);
        if (!api.generate)
            return failWith(QStringLiteral("LLM runtime missing: the GenAI shim is damaged — failed to resolve "
                                           "syn_genai_generate in %1: %2")
                                .arg(shimPath, loadError),
                            error);
        api.destroy = resolve<genai_shim::SynGenAiDeleteFn>(*shim, "syn_genai_pipeline_delete", &loadError);
        if (!api.destroy)
            return failWith(QStringLiteral("LLM runtime missing: the GenAI shim is damaged — failed to resolve "
                                           "syn_genai_pipeline_delete in %1: %2")
                                .arg(shimPath, loadError),
                            error);

        m_genAi = std::move(genai);
        m_shim = std::move(shim);
        m_genAiApi = api;
        m_tokenizersPath = tokenizersPath;
        m_genAiVersion = genAiVersion(genaiPath);
        return true;
    }

    // Creates the GenAI pipeline for the LLM model dir (the llm workload's
    // "compile"): LLMPipeline(model.dir, "NPU") — GenAI reads the model dir's
    // generation_config.json + chat template, tokenizes internally (CPU) and
    // staticizes the KV state for the NPU compiler (the smoke-proven path).
    // Runs the fail-closed CPU-fallback guard after creation.
    bool createLlmPipeline(const LoadedModel &model, QString *error)
    {
        if (model.dir.isEmpty())
            return failWith(QStringLiteral("the LLM model has no cache directory"), error);
        char errbuf[genai_shim::kSynGenAiErrorLen] = {};
        genai_shim::SynGenAiContext *ctx = m_genAiApi.create(model.dir.toUtf8().constData(), "NPU",
                                                             m_tokenizersPath.toUtf8().constData(), llmEosId(model),
                                                             errbuf, sizeof(errbuf));
        if (!ctx)
            return failWith(QString::fromUtf8(errbuf), error);
        if (m_llm)
            m_genAiApi.destroy(m_llm);
        m_llm = ctx;
        return ensureNpuDevice(error);
    }

    // The fail-closed CPU-fallback guard (EC-P9, must handle): an LLM run that
    // did not actually use the NPU must be Error, never scored. GenAI's
    // LLMPipeline does not expose EXECUTION_DEVICES (verified against the
    // 2026.4.1 headers: the compiled model is internal, and the stateful NPU
    // path compiles on device "NPU" internally — SMOKE: confirm on Nivedellir;
    // if a future GenAI exposes a device property, prefer it). So the guard
    // checks exactly what CAN be verified: the pipeline was just created with
    // device "NPU" (createLlmPipeline) AND the OpenVINO core reports an NPU
    // device available AND GenAI loaded (this runs only after loadGenAi
    // succeeded). The failure message names every fact checked.
    bool ensureNpuDevice(QString *error)
    {
        ov_available_devices_t devices{};
        if (m_api.coreGetAvailableDevices(m_coreHandle, &devices) != kOk) {
            return failWith(QStringLiteral("the LLM CPU-fallback guard failed: ov_core_get_available_devices "
                                           "returned an error — the device check was not possible, refusing to score"),
                            error);
        }
        QString deviceList;
        bool npu = false;
        for (std::size_t i = 0; i < devices.size && devices.devices; ++i) {
            if (!devices.devices[i])
                continue;
            if (!deviceList.isEmpty())
                deviceList += QLatin1StringView(", ");
            deviceList += QString::fromUtf8(devices.devices[i]);
            if (QString::fromUtf8(devices.devices[i]).startsWith(QLatin1StringView("NPU"), Qt::CaseInsensitive))
                npu = true;
        }
        if (m_api.availableDevicesFree)
            m_api.availableDevicesFree(&devices);
        if (!npu) {
            return failWith(
                QStringLiteral("the LLM pipeline was requested on device 'NPU' and OpenVINO GenAI loaded, but the "
                               "OpenVINO core reports no NPU device (available: %1) — refusing to score a "
                               "CPU-fallback run")
                    .arg(deviceList),
                error);
        }
        return true;
    }

    // The EOS token id from the model dir's generation_config.json (the
    // smoke's read_eos_id); 2 (the TinyLlama EOS id) when the file is
    // absent/unparsable. Used only for the streamer's informational eosSeen
    // flag — the §5.3 sentinel never depends on it (a wrong EOS only means
    // generation runs to the 256-token bound, still a valid score).
    static std::int64_t llmEosId(const LoadedModel &model)
    {
        QFile f(QDir(model.dir).filePath(QStringLiteral("generation_config.json")));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            const QJsonValue eos = doc.object().value(QStringLiteral("eos_token_id"));
            if (eos.isDouble())
                return std::int64_t(eos.toDouble());
            if (eos.isArray()) {
                const QJsonArray arr = eos.toArray();
                if (!arr.isEmpty() && arr.first().isDouble())
                    return std::int64_t(arr.first().toDouble());
            }
        }
        return 2;
    }

    // The GenAI runtime's own version string: the DLL's VERSIONINFO resource on
    // Windows (e.g. "2026.4.1"), the .so filename build suffix on Linux (pip
    // wheels ship libopenvino_genai.so.<build>, e.g. .2641 → "2026.4.1"), else
    // "". SMOKE: confirm the string reads sanely on Nivedellir.
    static QString genAiVersion(const QString &modulePath)
    {
#if defined(Q_OS_WIN)
        const QString native = QDir::toNativeSeparators(QFileInfo(modulePath).absoluteFilePath());
        const std::wstring wide = native.toStdWString();
        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(wide.c_str(), &handle);
        if (size == 0)
            return {};
        // The extra parens defeat the most-vexing-parse (a bare `block(size)`
        // would declare a function on Windows).
        std::vector<std::uint8_t> block((std::size_t(size)));
        if (!GetFileVersionInfoW(wide.c_str(), handle, size, block.data()))
            return {};
        void *value = nullptr;
        UINT valueLen = 0;
        if (!VerQueryValueW(block.data(), L"\\", &value, &valueLen))
            return {};
        const auto *fixed = static_cast<const VS_FIXEDFILEINFO *>(value);
        if (!fixed || fixed->dwSignature != 0xFEED04BD)
            return {};
        return QStringLiteral("%1.%2.%3")
            .arg(fixed->dwFileVersionMS >> 16)
            .arg(fixed->dwFileVersionMS & 0xFFFF)
            .arg(fixed->dwFileVersionLS >> 16);
#else
        // libopenvino_genai.so.2641 → "2026.4.1" (26.4.1 without the century).
        const QString fileName = QFileInfo(modulePath).fileName();
        const QString suffix = fileName.section(QLatin1Char('.'), -1);
        bool ok = false;
        const int build = suffix.toInt(&ok);
        if (!ok || build <= 0 || build < 1000 || build > 9999)
            return {};
        return QStringLiteral("%1.%2.%3").arg(build / 100 + 2000).arg((build / 10) % 10).arg(build % 10);
#endif
    }

    // The C thunk the shim's streamer calls per token to poll Stop (P-STOP ≤
    // 250 ms: a token on the NPU lands every ~30 ms, far inside the budget).
    // m_pendingCancelled is set for the duration of one generate() call —
    // single-threaded, no locking needed.
    static int cancelThunk(void *user)
    {
        auto *engine = static_cast<OpenVinoEngine *>(user);
        return engine->m_pendingCancelled && engine->m_pendingCancelled() ? 1 : 0;
    }

    // One 5 s window of generation: a single synchronous generate() call
    // bounded by the shim's streamer — it counts the generated tokens, records
    // the first-token wall time, and returns STOP/CANCEL when the 5 s window
    // elapses or Stop fires (GenAI stops at EOS or the 256-token bound itself).
    // `units` = generated tokens, `ttftSeconds` = wall time to the first
    // generated token, `valid` = 1 only when generation reached the §5.3 floor
    // (≥ 8 tokens) — a model that emits EOS on step 1 yields tokens < 8 and is
    // an Error via the workload layer's correctness sentinel, never a scored
    // rate. The median-of-3 window structure in workloads.cpp is unchanged.
    bool runLlmWindow(Inference &out, QString *error, const std::function<bool()> &cancelled)
    {
        const auto windowStart = std::chrono::steady_clock::now();
        const auto windowStartNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(windowStart.time_since_epoch()).count();
        genai_shim::SynGenAiControl control;
        control.deadlineNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 (windowStart + std::chrono::milliseconds(int(kWindowSeconds * 1000.0)))
                                     .time_since_epoch())
                                 .count();
        m_pendingCancelled = cancelled;
        control.pollCancelled = &OpenVinoEngine::cancelThunk;
        control.cancelledUser = this;

        char errbuf[genai_shim::kSynGenAiErrorLen] = {};
        const int status =
            m_genAiApi.generate(m_llm, kLlmPrompt, kLlmMaxNewTokens, &control, errbuf, sizeof(errbuf));
        m_pendingCancelled = {};
        if (status != 0)
            return failWith(QString::fromUtf8(errbuf), error);

        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart).count();
        out.units = control.tokens;
        out.seconds = elapsed;
        out.ttftSeconds = control.firstTokenNs > 0 ? double(control.firstTokenNs - windowStartNs) / 1e9 : 0.0;
        // §5.3 correctness floor: valid only if generation reached ≥ 8 new
        // tokens. The workload layer turns `valid <= 0` into an Error, so an
        // EOS-on-step-1 model is never scored.
        out.valid = (out.units >= kLlmMinNewTokens) ? 1 : 0;
        return true;
    }

    // failWith() stamps the error and returns false; failLoad() first unwinds the
    // runtime handles (the init path). Nothing here throws.
    bool failLoad(const QString &message, QString *error)
    {
        unload();
        return failWith(message, error);
    }
    static bool failWith(const QString &message, QString *error)
    {
        if (error)
            *error = message;
        return false;
    }

    // Releases the per-model ov_* handles (request → compiled → model) in
    // dependency order. The core + libraries survive, so this is the per-workload
    // reload path (compileModel calls it before recompiling).
    void freeModel()
    {
        if (m_api.inferRequestFree && m_request) {
            m_api.inferRequestFree(m_request);
            m_request = nullptr;
        }
        if (m_api.compiledModelFree && m_compiled) {
            m_api.compiledModelFree(m_compiled);
            m_compiled = nullptr;
        }
        if (m_api.modelFree && m_model) {
            m_api.modelFree(m_model);
            m_model = nullptr;
        }
    }

    // Releases every ov_* handle including the core and clears `m_ready`. Call
    // BEFORE the Library owners are destroyed or reassigned so the libraries are
    // never dlclose'd with live device handles.
    void unload()
    {
        freeModel();
        // The GenAI pipeline object lives in the shim module: destroy it BEFORE
        // the shim (and genai) libraries are released, or its destructor would
        // run inside a dlclose'd module.
        if (m_genAiApi.destroy && m_llm) {
            m_genAiApi.destroy(m_llm);
            m_llm = nullptr;
        }
        if (m_api.coreFree && m_coreHandle) {
            m_api.coreFree(m_coreHandle);
            m_coreHandle = nullptr;
        }
        m_loadedPath.clear();
        m_loadedIsLlm = false;
        m_ready = false;
        m_shim.reset();
        m_genAi.reset();
        m_genAiApi = GenAiApi{};
        m_genAiVersion.clear();
        m_tokenizersPath.clear();
        m_pendingCancelled = {};
    }

    QString m_runtimePath;
    QString m_version;
    QString m_loadedPath;    // the model file compiled by the last successful compileModel()
    bool m_loadedIsLlm = false;   // whether the loaded model is the stateful LLM decoder
    OpenVinoApi m_api;
    std::unique_ptr<Library> m_core;    // the C++ runtime (kept loaded as a dependency)
    std::unique_ptr<Library> m_cApi;    // the C API (ov_* symbols resolved from here)
    std::unique_ptr<Library> m_plugin;  // the Intel NPU plugin (kept loaded for the core)
    // The GenAI "second runtime" (DD-1 §5.4, loaded lazily for the LLM workload):
    std::unique_ptr<Library> m_genAi;   // the user's openvino_genai runtime
    std::unique_ptr<Library> m_shim;    // our syn_openvino_genai_shim module
    GenAiApi m_genAiApi;
    genai_shim::SynGenAiContext *m_llm = nullptr;   // the live LLMPipeline (per llm compile)
    QString m_genAiVersion;             // the GenAI runtime's version ("" until the llm path loads it)
    QString m_tokenizersPath;           // the user's openvino_tokenizers lib ("" when not found)
    std::function<bool()> m_pendingCancelled;   // set for the duration of one generate() call

    ov_core *m_coreHandle = nullptr;
    ov_model *m_model = nullptr;
    ov_compiled_model *m_compiled = nullptr;
    ov_infer_request *m_request = nullptr;
    bool m_ready = false;
};

} // namespace

std::unique_ptr<INpuEngine> makeOpenVinoEngine(const QString &runtimePath)
{
    return std::make_unique<OpenVinoEngine>(runtimePath);
}

} // namespace sb::npu

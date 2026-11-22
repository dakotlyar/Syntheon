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
// The C API below is the OpenVINO 2.x C ABI (openvino/c/ov_core.h, ov_tensor.h,
// ov_infer_request.h, ov_compiled_model.h, ov_common.h), pinned to the 2023.x+
// release lines and verified against those headers at fix-round 4c-3 — the
// signatures here are the real ones, not guesses. Every resolve is null-checked,
// so a mismatched symbol fails init() cleanly with the symbol name rather than
// crashing.

#include "tests/npu/engine/engine.h"
#include "tests/npu/engine/engine_dlopen.h"

#include <QDir>
#include <QFileInfo>

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

// ov_element_type_e (ov_element_type.h): the element type of a tensor, read at
// run time so the code never hardcodes a numeric enum value.
typedef int ov_element_type_e;

// ov_shape_t (ov_shape.h): rank + up to OV_MAX_DIMS (8) dims. Freed with
// ov_shape_free once allocated via ov_shape_create.
struct ov_shape_t {
    std::int64_t rank;
    std::int64_t dims[8];
};

constexpr int kOk = 0;   // ov_status_e::OK
constexpr double kWindowSeconds = 5.0;

// The LLM decode-loop bounds (Phase 4c §5.3): a hard cap on new tokens per
// window so a never-ending / runaway generation can't spin past the 5 s window
// (a 5 s window at a realistic ~55 tok/s ≈ 275 tokens, so 256 is the ceiling),
// and the §5.3 correctness floor — a window is `valid` only if generation
// reached ≥ 8 new tokens before EOS; a model that emits EOS on step 1 is an
// Error, never a scored rate.
constexpr std::int64_t kLlmMaxNewTokens = 256;
constexpr std::int64_t kLlmMinNewTokens = 8;

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
    // The LLM path (4c-5): read the compiled model's properties (fail-closed
    // EXECUTION_DEVICES guard), shape/reshape tensors, and swap per-step input
    // tensors for the autoregressive loop. All OpenVINO 2.x C ABI; each
    // null-checked at resolve time.
    int (*compiledModelGetProperty)(const ov_compiled_model *model, const char *property_key,
                                    char **property_value) = nullptr;
    void (*free)(void *mem) = nullptr;                          // ov_free: frees get_property's return
    int (*tensorGetShape)(const ov_tensor *tensor, ov_shape_t *shape) = nullptr;
    int (*shapeCreate)(std::int64_t rank, const std::int64_t *dims, ov_shape_t *shape) = nullptr;
    void (*shapeFree)(ov_shape_t *shape) = nullptr;
    int (*tensorGetElementType)(const ov_tensor *tensor, ov_element_type_e *type) = nullptr;
    int (*tensorCreate)(ov_element_type_e type, const ov_shape_t shape, void *host_ptr, ov_tensor **tensor) = nullptr;
    int (*setInputTensor)(ov_infer_request *request, std::size_t idx, const ov_tensor *tensor) = nullptr;
};

} // namespace

namespace {

class OpenVinoEngine final : public INpuEngine
{
public:
    explicit OpenVinoEngine(QString runtimePath) : m_runtimePath(std::move(runtimePath)) {}
    ~OpenVinoEngine() override { unload(); }   // release ov handles before the Library dtors dlclose

    QString backendName() const override { return QStringLiteral("openvino-npu"); }
    QString runtimeVersion() const override { return m_version; }

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
        m_api.compiledModelGetProperty =
            resolve<decltype(m_api.compiledModelGetProperty)>(capi, "ov_compiled_model_get_property", error);
        if (!m_api.compiledModelGetProperty)
            return false;
        m_api.free = resolve<decltype(m_api.free)>(capi, "ov_free", error);
        if (!m_api.free)
            return false;
        m_api.tensorGetShape = resolve<decltype(m_api.tensorGetShape)>(capi, "ov_tensor_get_shape", error);
        if (!m_api.tensorGetShape)
            return false;
        m_api.shapeCreate = resolve<decltype(m_api.shapeCreate)>(capi, "ov_shape_create", error);
        if (!m_api.shapeCreate)
            return false;
        m_api.shapeFree = resolve<decltype(m_api.shapeFree)>(capi, "ov_shape_free", error);
        if (!m_api.shapeFree)
            return false;
        m_api.tensorGetElementType = resolve<decltype(m_api.tensorGetElementType)>(capi, "ov_tensor_get_element_type", error);
        if (!m_api.tensorGetElementType)
            return false;
        m_api.tensorCreate = resolve<decltype(m_api.tensorCreate)>(capi, "ov_tensor_create", error);
        if (!m_api.tensorCreate)
            return false;
        m_api.setInputTensor =
            resolve<decltype(m_api.setInputTensor)>(capi, "ov_infer_request_set_input_tensor_by_index", error);
        if (!m_api.setInputTensor)
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
            // The LLM decoder is compiled requesting the NPUW device property so
            // generation can land on the NPU's stateful device (GenAI sets this
            // internally; Option B replicates it). One property pair + a variadic
            // (key, value) tuple.
            // SMOKE: the exact NPUW device-property KEY string and VALUE string
            // for ov_core_compile_model ("NPUW_DEVICES" / "NPU,CPU" are guesses);
            // read the OpenVINO NPU plugin docs on Nivedellir, do not trust them.
            const char *kDeviceKey = "NPUW_DEVICES";
            const char *kDeviceValue = "NPU,CPU";
            if (m_api.compileModel(m_coreHandle, m_model, "NPU", 1, &m_compiled, kDeviceKey, kDeviceValue) != kOk
                || !m_compiled)
                return failLoad(QStringLiteral("ov_core_compile_model failed for the LLM decoder on device 'NPU'"), error);
        } else {
            // 0 property args + no variadic pairs → compiles the model with defaults.
            if (m_api.compileModel(m_coreHandle, m_model, "NPU", 0, &m_compiled) != kOk || !m_compiled)
                return failLoad(QStringLiteral("ov_core_compile_model failed on device 'NPU'"), error);
        }

        if (m_api.createInferRequest(m_compiled, &m_request) != kOk || !m_request)
            return failLoad(QStringLiteral("ov_compiled_model_create_infer_request failed"), error);

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

    // ---- LLM autoregressive decode (Phase 4c §5.2 Option B) ----

    // The OpenVINO 2.x element-type enum values (ov_element_type.h), read back
    // from each tensor at run time so the code never assumes a fixed type.
    static constexpr ov_element_type_e kElementUndefined = 0;
    static constexpr ov_element_type_e kElementI32 = 10;
    static constexpr ov_element_type_e kElementI64 = 11;
    static constexpr ov_element_type_e kElementF32 = 5;
    static constexpr std::size_t kMaxLlmInputs = 8;

    // A fixed pre-shipped prompt's input_ids for the scored loop (§5.2): the
    // real tokenization stays CPU/offline for the correctness spot-check only.
    // SMOKE: replace with the real pre-shipped prompt's input_ids from the pack;
    // the EOS id below is unknown and must be confirmed on Nivedellir (a wrong
    // EOS only means generation runs to the 5 s / 256-token bound instead of
    // stopping early — still a valid score).
    static constexpr std::int64_t kLlmEosId = 2;
    static constexpr std::int64_t kLlmPrompt[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static constexpr std::int64_t kLlmPromptLen = std::int64_t(sizeof(kLlmPrompt) / sizeof(kLlmPrompt[0]));
    // A single generated token's attention mask (all ones) and the fixed beam_idx
    // = 0 fed at every decode step (KV persists as internal model state; the
    // beam_idx input is pinned to 0 — SMOKE, confirm on Nivedellir).
    static constexpr std::int64_t kLlmMaskOnes[] = {1};
    static constexpr std::int64_t kLlmBeamZero[] = {0};

    // One 5 s window of autoregressive generation. Runs the stateful decoder:
    // prefill once on the fixed prompt (populating the KV cache held as internal
    // model state), then greedy/argmax one token at a time until EOS, the 256-token
    // cap, or the 5 s window ends. `units` = generated tokens, `ttftSeconds` =
    // first-token wall time, `valid` = 1 only when generation reached the §5.3
    // floor (≥ 8 tokens) — so a model that emits EOS on step 1 is an Error via
    // the workload layer's existing correctness sentinel, never a scored rate.
    bool runLlmWindow(Inference &out, QString *error, const std::function<bool()> &cancelled)
    {
        // Fail-closed CPU-fallback guard (EC-P9, must handle): the decoder was
        // compiled requesting the NPUW device, which may fall back to CPU — never
        // silently score a CPU run. Read the compiled model's EXECUTION_DEVICES
        // and require NPU.
        // SMOKE: EXECUTION_DEVICES is returned by ov_compiled_model_get_property
        // as a comma-joined device string; confirm the exact value on Nivedellir.
        char *devices = nullptr;
        if (m_api.compiledModelGetProperty(m_compiled, "EXECUTION_DEVICES", &devices) != kOk || !devices)
            return failWith(QStringLiteral("failed to read EXECUTION_DEVICES from the LLM decoder"), error);
        const QString deviceList = QString::fromUtf8(devices);
        m_api.free(devices);
        if (!deviceList.contains(QLatin1StringView("NPU"), Qt::CaseInsensitive))
            return failWith(QStringLiteral("the LLM decoder compiled on '%1', not the NPU — refusing to score a CPU-fallback run")
                                .arg(deviceList),
                            error);

        // Identify the stateful decoder's inputs by shape and read their element
        // types. Standard OpenVINO stateful-decoder ordering (SMOKE): input_ids
        // is the first rank-2 input, attention_mask the second rank-2 input, and
        // beam_idx the single rank-1 input.
        int inputIdsIdx = -1, maskIdx = -1, beamIdx = -1;
        ov_element_type_e idsType = kElementUndefined, maskType = kElementUndefined, beamType = kElementUndefined;
        int rank2Seen = 0;
        for (std::size_t idx = 0; idx < kMaxLlmInputs; ++idx) {
            ov_tensor *t = nullptr;
            if (m_api.getInputTensor(m_request, idx, &t) != kOk || !t)
                break;   // index past the last input → done
            ov_shape_t shape{};
            const bool haveShape = m_api.tensorGetShape(t, &shape) == kOk;
            ov_element_type_e type = kElementUndefined;
            m_api.tensorGetElementType(t, &type);
            if (haveShape && shape.rank == 1 && beamIdx < 0) {
                beamIdx = int(idx);
                beamType = type;
            } else if (haveShape && shape.rank == 2 && rank2Seen == 0) {
                inputIdsIdx = int(idx);
                idsType = type;
                ++rank2Seen;
            } else if (haveShape && shape.rank == 2 && rank2Seen == 1) {
                maskIdx = int(idx);
                maskType = type;
                ++rank2Seen;
            }
            m_api.tensorFree(t);
        }
        if (inputIdsIdx < 0 || maskIdx < 0 || beamIdx < 0) {
            return failWith(
                QStringLiteral("the LLM decoder has an unexpected input layout (need input_ids, attention_mask, beam_idx)"),
                error);
        }

        const auto windowStart = std::chrono::steady_clock::now();

        // Prefill once on the full prompt (§5.2 step 3): a single stateful decoder
        // does prefill from the full prompt on the first infer, KV persisting as
        // internal model state across the subsequent decode infers.
        // SMOKE: the single stateful-decoder prefill assumption (vs a separate
        // embed model) — confirm from the emitted IR on Nivedellir.
        // The prefill attention mask is all-ones across the full prompt (no
        // padding in greedy decoding).
        std::vector<std::int64_t> promptMask(std::size_t(kLlmPromptLen), 1);
        if (!setInput(inputIdsIdx, idsType, kLlmPrompt, kLlmPromptLen, true, error)
            || !setInput(maskIdx, maskType, promptMask.data(), kLlmPromptLen, true, error)
            || !setInput(beamIdx, beamType, kLlmBeamZero, 1, false, error))
            return false;
        if (m_api.infer(m_request) != kOk)
            return failWith(QStringLiteral("the LLM decoder failed during prefill"), error);
        const auto firstTokenAt = std::chrono::steady_clock::now();
        std::int64_t firstToken = 0;
        if (!argmaxLogits(kLlmPromptLen - 1, &firstToken, error))
            return false;
        const double ttft = std::chrono::duration<double>(firstTokenAt - windowStart).count();

        // Decode loop (§5.2 step 4): feed the last token, greedy/argmax the next.
        // KV is internal model state (persists across ov_infer_request_infer with
        // beam_idx = 0 at every step — SMOKE, confirm on Nivedellir).
        std::int64_t tokens = 1;   // the prefill's first generated token
        std::int64_t prev = firstToken;
        const auto until = windowStart + std::chrono::milliseconds(int(kWindowSeconds * 1000.0));
        while (prev != kLlmEosId && tokens < kLlmMaxNewTokens && std::chrono::steady_clock::now() < until) {
            if (cancelled && cancelled())
                break;
            if (!setInput(inputIdsIdx, idsType, &prev, 1, true, error)
                || !setInput(maskIdx, maskType, kLlmMaskOnes, 1, true, error)
                || !setInput(beamIdx, beamType, kLlmBeamZero, 1, false, error))
                return false;
            if (m_api.infer(m_request) != kOk)
                return failWith(QStringLiteral("the LLM decoder failed during token generation"), error);
            std::int64_t next = 0;
            if (!argmaxLogits(0, &next, error))
                return false;
            prev = next;
            ++tokens;
        }

        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart).count();
        out.units = tokens;
        out.seconds = elapsed;
        out.ttftSeconds = ttft;
        // §5.3 correctness floor: valid only if generation reached ≥ 8 new tokens
        // before EOS. The workload layer turns `valid <= 0` into an Error, so an
        // EOS-on-step-1 model is never scored.
        out.valid = (tokens >= kLlmMinNewTokens) ? 1 : 0;
        return true;
    }

    // Builds and attaches one integer input tensor to the request: `values`
    // (count `count`) typed `type`, shaped [1, count] (rank2) or [count] (rank1).
    // The host buffer is copied by ov_tensor_create, so it need not outlive the call.
    bool setInput(int idx, ov_element_type_e type, const std::int64_t *values, std::int64_t count, bool rank2, QString *error)
    {
        const std::int64_t dims2[2] = {1, count};
        const std::int64_t dims1[1] = {count};
        ov_shape_t shape{};
        const std::int64_t *dims = rank2 ? dims2 : dims1;
        if (m_api.shapeCreate(rank2 ? 2 : 1, dims, &shape) != kOk)
            return failWith(QStringLiteral("failed to build an input tensor shape"), error);

        std::vector<std::uint8_t> buffer;
        if (type == kElementI64) {
            buffer.resize(std::size_t(sizeof(std::int64_t) * count));
            std::memcpy(buffer.data(), values, sizeof(std::int64_t) * std::size_t(count));
        } else if (type == kElementI32) {
            buffer.resize(std::size_t(sizeof(std::int32_t) * count));
            std::int32_t *dst = reinterpret_cast<std::int32_t *>(buffer.data());
            for (std::int64_t i = 0; i < count; ++i)
                dst[i] = std::int32_t(values[i]);
        } else {
            m_api.shapeFree(&shape);
            // SMOKE: the decoder's integer inputs assumed I32/I64.
            return failWith(QStringLiteral("the LLM decoder uses an unsupported integer input element type"), error);
        }

        ov_tensor *tensor = nullptr;
        const int status = m_api.tensorCreate(type, shape, buffer.data(), &tensor);
        m_api.shapeFree(&shape);
        if (status != kOk || !tensor)
            return failWith(QStringLiteral("failed to build an input tensor"), error);
        const int setStatus = m_api.setInputTensor(m_request, std::size_t(idx), tensor);
        m_api.tensorFree(tensor);   // the request now owns its own copy
        if (setStatus != kOk)
            return failWith(QStringLiteral("failed to attach an input tensor to the decoder"), error);
        return true;
    }

    // Greedy/argmax the token at `row` of the logits output (row = prompt_len-1
    // for prefill, 0 for single-token decode). Logits are assumed FP32 (SMOKE).
    bool argmaxLogits(std::int64_t row, std::int64_t *token, QString *error)
    {
        ov_tensor *logits = nullptr;
        if (m_api.getOutputTensor(m_request, 0, &logits) != kOk || !logits)
            return failWith(QStringLiteral("failed to read the decoder's logits"), error);
        ov_element_type_e type = kElementUndefined;
        if (m_api.tensorGetElementType(logits, &type) != kOk || type != kElementF32) {
            m_api.tensorFree(logits);
            return failWith(QStringLiteral("the decoder's logits are not FP32"), error);
        }
        ov_shape_t shape{};
        if (m_api.tensorGetShape(logits, &shape) != kOk || shape.rank < 3) {
            m_api.tensorFree(logits);
            return failWith(QStringLiteral("the decoder's logits have an unexpected shape"), error);
        }
        void *data = nullptr;
        if (m_api.tensorData(logits, &data) != kOk || !data) {
            m_api.tensorFree(logits);
            return failWith(QStringLiteral("the logits tensor has no data buffer"), error);
        }
        const float *f = static_cast<const float *>(data);
        const std::int64_t vocab = shape.dims[2];
        if (vocab <= 0 || row >= shape.dims[1]) {
            m_api.tensorFree(logits);
            return failWith(QStringLiteral("the decoder's logits shape is out of range"), error);
        }
        const float *rowData = f + row * vocab;
        std::int64_t best = 0;
        float bestVal = -std::numeric_limits<float>::infinity();
        for (std::int64_t i = 0; i < vocab; ++i) {
            if (rowData[i] > bestVal) {
                bestVal = rowData[i];
                best = i;
            }
        }
        m_api.tensorFree(logits);
        *token = best;
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
        if (m_api.coreFree && m_coreHandle) {
            m_api.coreFree(m_coreHandle);
            m_coreHandle = nullptr;
        }
        m_loadedPath.clear();
        m_loadedIsLlm = false;
        m_ready = false;
    }

    QString m_runtimePath;
    QString m_version;
    QString m_loadedPath;    // the model file compiled by the last successful compileModel()
    bool m_loadedIsLlm = false;   // whether the loaded model is the stateful LLM decoder
    OpenVinoApi m_api;
    std::unique_ptr<Library> m_core;    // the C++ runtime (kept loaded as a dependency)
    std::unique_ptr<Library> m_cApi;    // the C API (ov_* symbols resolved from here)
    std::unique_ptr<Library> m_plugin;  // the Intel NPU plugin (kept loaded for the core)

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

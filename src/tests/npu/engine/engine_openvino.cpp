// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenVINO NPU engine (Intel AI Boost), Phase 4c design §4.1. Loads the
// USER-INSTALLED OpenVINO runtime from the path detect resolved: the C++ core
// (openvino.dll / libopenvino.so), the C API (openvino_c.dll / libopenvino_c.so)
// and the Intel NPU plugin (openvino_intel_npu_plugin.{dll,so}). The `ov_*` C
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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

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

constexpr int kOk = 0;   // ov_status_e::OK
constexpr double kWindowSeconds = 5.0;

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
        auto core = std::make_unique<Library>(libPath(m_runtimePath, coreName()), error);
        if (!core->isOpen())
            return false;
        auto capi = std::make_unique<Library>(libPath(m_runtimePath, cApiName()), error);
        if (!capi->isOpen())
            return false;
        auto plugin = std::make_unique<Library>(libPath(m_runtimePath, pluginName()), error);
        if (!plugin->isOpen())
            return false;

        m_core = std::move(core);
        m_cApi = std::move(capi);
        m_plugin = std::move(plugin);
        return load(model, error);
    }

    bool runWindow(const QString &, const LoadedModel &, Inference &out, QString *error,
                   const std::function<bool()> &cancelled) override
    {
        out.seconds = 0.0;
        out.units = 0;
        out.valid = 0;
        if (!m_ready)
            return failWith(QStringLiteral("the OpenVINO engine is not initialised"), error);

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
        // OpenVINO's NPU plugin ships without the `lib` prefix (it is dlopen'd by
        // name, unlike the linked core/C-API libs) — match detect_linux.cpp.
        return QStringLiteral("openvino_intel_npu_plugin.so");
#endif
    }
    static QString libPath(const QString &dir, const QString &name) { return QDir(dir).filePath(name); }

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

        int readStatus = kOk;
#if defined(_WIN32)
        // Wide path so a non-ASCII profile/app-data dir doesn't fail in CP_ACP (L1).
        const std::wstring wpath = model.path.toStdWString();
        readStatus = m_api.readModelUnicode(m_coreHandle, wpath.c_str(), nullptr, &m_model);
#else
        readStatus = m_api.readModel(m_coreHandle, model.path.toUtf8().constData(), nullptr, &m_model);
#endif
        if (readStatus != kOk || !m_model)
            return failLoad(QStringLiteral("ov_core_read_model failed for %1").arg(model.path), error);
        // 0 property args + no variadic pairs → compiles the model with defaults.
        if (m_api.compileModel(m_coreHandle, m_model, "NPU", 0, &m_compiled) != kOk || !m_compiled)
            return failLoad(QStringLiteral("ov_core_compile_model failed on device 'NPU'"), error);
        if (m_api.createInferRequest(m_compiled, &m_request) != kOk || !m_request)
            return failLoad(QStringLiteral("ov_compiled_model_create_infer_request failed"), error);

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

    // Releases the ov_* handles in dependency order (request → compiled → model →
    // core) and clears `m_ready`. Call BEFORE the Library owners are destroyed or
    // reassigned so the libraries are never dlclose'd with live device handles.
    void unload()
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
        if (m_api.coreFree && m_coreHandle) {
            m_api.coreFree(m_coreHandle);
            m_coreHandle = nullptr;
        }
        m_ready = false;
    }

    QString m_runtimePath;
    QString m_version;
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

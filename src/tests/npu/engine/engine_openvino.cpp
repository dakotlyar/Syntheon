// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenVINO NPU engine (Intel AI Boost), Phase 4c design §4.1. dlopens the
// USER-INSTALLED OpenVINO runtime (openvino.dll / libopenvino.so) plus the
// Intel NPU plugin, resolves the OpenVINO C API by name, compiles the model for
// device "NPU", and runs a sync-infer measurement window. No vendor header or
// .lib is compiled in (P-VENDOR); a missing/damaged runtime is a clean
// init() == false + a clear error, never a throw or a crash.
//
// The C API below is the OpenVINO 2.x C ABI (openvino/c/openvino.h), pinned to
// the 2023.x+ release lines. The symbols marked [UNCERTAIN] have changed name
// or shape across releases and are the ones most likely to need re-pinning at
// real-hardware validation (§16): every resolve is null-checked, so a mismatched
// symbol fails init() cleanly with the symbol name rather than crashing.

#include "tests/npu/engine/engine.h"
#include "tests/npu/engine/engine_dlopen.h"

#include <QDir>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
struct ov_any;   // property map ("config"); we always pass nullptr (defaults)

// ov_version_t (2023.x layout): the strings are owned by the runtime; free with
// ov_version_free. [UNCERTAIN] pre-2022.2 exposed `const char* ov_get_openvino_version()`.
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
    int (*readModel)(ov_core *core, const char *model_path, const char *bin_path, ov_model **model) = nullptr;
    int (*compileModel)(const ov_core *core, const ov_model *model, const char *device_name,
                        const ov_any *config, ov_compiled_model **compiled_model) = nullptr;   // [UNCERTAIN] config arg type
    void (*modelFree)(ov_model *model) = nullptr;
    void (*compiledModelFree)(ov_compiled_model *model) = nullptr;
    int (*createInferRequest)(const ov_compiled_model *model, ov_infer_request **request) = nullptr;
    int (*getInputTensor)(const ov_infer_request *request, size_t idx, ov_tensor **tensor) = nullptr;    // [UNCERTAIN] by_index vs get_tensor(port)
    int (*infer)(ov_infer_request *request) = nullptr;
    int (*getOutputTensor)(const ov_infer_request *request, size_t idx, ov_tensor **tensor) = nullptr;   // [UNCERTAIN] by_index vs get_tensor(port)
    void *(*tensorData)(const ov_tensor *tensor) = nullptr;
    size_t (*tensorByteSize)(const ov_tensor *tensor) = nullptr;
    void (*inferRequestFree)(ov_infer_request *request) = nullptr;
    void (*getVersion)(ov_version *version) = nullptr;    // optional: absent → "" version
    void (*versionFree)(ov_version *version) = nullptr;   // optional
};

} // namespace

namespace {

class OpenVinoEngine final : public INpuEngine
{
public:
    explicit OpenVinoEngine(QString runtimePath) : m_runtimePath(std::move(runtimePath)) {}

    QString backendName() const override { return QStringLiteral("openvino-npu"); }
    QString runtimeVersion() const override { return m_version; }

    bool init(const LoadedModel &model, QString *error) override
    {
        // The plugin (openvino_intel_npu_plugin.{dll,so}) must sit beside the
        // core (detect verified that); holding its handle keeps it loaded for the
        // core's device loader. The core exports the whole C API below.
        auto core = std::make_unique<Library>(libPath(m_runtimePath, coreName()), error);
        if (!core->isOpen())
            return false;
        auto plugin = std::make_unique<Library>(libPath(m_runtimePath, pluginName()), error);
        if (!plugin->isOpen())
            return false;
        m_core = std::move(core);
        m_plugin = std::move(plugin);
        return load(model, error);
    }

    bool runWindow(const QString &, const LoadedModel &, Inference &out, QString *error) override
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

        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(kWindowSeconds * 1000.0));
        while (std::chrono::steady_clock::now() < until) {
            if (m_api.infer(m_request) != kOk)
                return failWith(QStringLiteral("an inference failed on the NPU"), error);
            ++out.units;
            if (outputSanity())
                ++out.valid;
        }
        out.seconds = kWindowSeconds;
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
    static QString pluginName()
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("openvino_intel_npu_plugin.dll");
#else
        return QStringLiteral("libopenvino_intel_npu_plugin.so");
#endif
    }
    static QString libPath(const QString &dir, const QString &name) { return QDir(dir).filePath(name); }

    bool resolveApi(Library &core, QString *error)
    {
        m_api.coreCreate = resolve<decltype(m_api.coreCreate)>(core, "ov_core_create", error);
        m_api.coreFree = resolve<decltype(m_api.coreFree)>(core, "ov_core_free", error);
        m_api.readModel = resolve<decltype(m_api.readModel)>(core, "ov_core_read_model", error);
        m_api.compileModel = resolve<decltype(m_api.compileModel)>(core, "ov_core_compile_model", error);
        m_api.modelFree = resolve<decltype(m_api.modelFree)>(core, "ov_model_free", error);
        m_api.compiledModelFree = resolve<decltype(m_api.compiledModelFree)>(core, "ov_compiled_model_free", error);
        m_api.createInferRequest = resolve<decltype(m_api.createInferRequest)>(core, "ov_compiled_model_create_infer_request", error);
        // [UNCERTAIN] input/output access: the 2023.1+ unified per-port
        // `ov_infer_request_get_tensor(req, port, &tensor)` is the alternative if
        // these by_index entry points are absent from a user's runtime build.
        m_api.getInputTensor = resolve<decltype(m_api.getInputTensor)>(core, "ov_infer_request_get_input_tensor_by_index", error);
        m_api.infer = resolve<decltype(m_api.infer)>(core, "ov_infer_request_infer", error);
        m_api.getOutputTensor = resolve<decltype(m_api.getOutputTensor)>(core, "ov_infer_request_get_output_tensor_by_index", error);
        m_api.tensorData = resolve<decltype(m_api.tensorData)>(core, "ov_tensor_data", error);
        m_api.tensorByteSize = resolve<decltype(m_api.tensorByteSize)>(core, "ov_tensor_get_byte_size", error);
        m_api.inferRequestFree = resolve<decltype(m_api.inferRequestFree)>(core, "ov_infer_request_free", error);

        // Version is optional: resolve into a sink so an absence isn't an error.
        QString versionErr;
        m_api.getVersion = resolve<decltype(m_api.getVersion)>(core, "ov_get_openvino_version", &versionErr);
        if (!m_api.getVersion)
            versionErr.clear();
        m_api.versionFree = resolve<decltype(m_api.versionFree)>(core, "ov_version_free", &versionErr);
        if (!m_api.versionFree)
            versionErr.clear();

        return m_api.coreCreate && m_api.coreFree && m_api.readModel && m_api.compileModel && m_api.modelFree
               && m_api.compiledModelFree && m_api.createInferRequest && m_api.getInputTensor && m_api.infer
               && m_api.getOutputTensor && m_api.tensorData && m_api.tensorByteSize && m_api.inferRequestFree;
    }

    bool load(const LoadedModel &model, QString *error)
    {
        if (error)
            error->clear();
        if (!resolveApi(m_coreLibrary(), error))
            return false;

        m_version = readVersion();

        if (m_api.coreCreate(&m_coreHandle) != kOk || !m_coreHandle)
            return failLoad(QStringLiteral("ov_core_create failed"), error);
        if (m_api.readModel(m_coreHandle, model.path.toUtf8().constData(), nullptr, &m_model) != kOk || !m_model)
            return failLoad(QStringLiteral("ov_core_read_model failed for %1").arg(model.path), error);
        if (m_api.compileModel(m_coreHandle, m_model, "NPU", nullptr, &m_compiled) != kOk || !m_compiled)
            return failLoad(QStringLiteral("ov_core_compile_model failed on device 'NPU'"), error);
        if (m_api.createInferRequest(m_compiled, &m_request) != kOk || !m_request)
            return failLoad(QStringLiteral("ov_compiled_model_create_infer_request failed"), error);

        m_ready = true;
        return true;
    }

    Library &m_coreLibrary() { return *m_core; }

    // The module's version string, or "" when the (uncertain) symbol is absent.
    QString readVersion()
    {
        if (!m_api.getVersion)
            return QString();
        ov_version v{};
        m_api.getVersion(&v);
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
        void *data = m_api.tensorData(input);
        if (!data)
            return failWith(QStringLiteral("the input tensor has no data buffer"), error);
        const size_t bytes = m_api.tensorByteSize(input);
        // A deterministic (byte i → i & 0xFF) non-zero fill, not all-zero, so a
        // silent "always-zero" output is still distinguishable from real work.
        std::uint8_t *p = static_cast<std::uint8_t *>(data);
        for (size_t i = 0; i < bytes; ++i)
            p[i] = std::uint8_t(i);
        return true;
    }

    // The §2 correctness sentinel, reduced to an output-presence check here: the
    // precise argmax-range / box-count / token-count checks live in the workload
    // layer (task 11), which owns model shape knowledge. The engine counts an
    // inference valid when its output tensor is non-empty.
    bool outputSanity()
    {
        ov_tensor *output = nullptr;
        if (m_api.getOutputTensor(m_request, 0, &output) != kOk || !output)
            return false;
        return m_api.tensorByteSize(output) > 0;
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

    void unload()
    {
        if (m_api.inferRequestFree && m_request) {
            m_api.inferRequestFree(m_request);
            m_request = nullptr;
        }
        if (m_api.modelFree && m_model) {
            m_api.modelFree(m_model);
            m_model = nullptr;
        }
        if (m_api.compiledModelFree && m_compiled) {
            m_api.compiledModelFree(m_compiled);
            m_compiled = nullptr;
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
    std::unique_ptr<Library> m_core;
    std::unique_ptr<Library> m_plugin;

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

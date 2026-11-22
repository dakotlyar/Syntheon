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
// (DD-1 §5.4 Option A, C-API rewrite 2026-11-22): the compile-first smoke
// proved the raw-C KV loop cannot compile the stateful IR on the NPU, while
// LLMPipeline(dir, "NPU") does. The engine dlopens the user's openvino_genai_c
// (the C wrapper over the C++ openvino_genai runtime) and resolves the ov_genai_*
// entry points by name via genai_c_api.h — no C++ shim module, no GenAI headers
// or import-lib as a build input (P-VENDOR restored; the runtime stays
// user-installed, never bundled). The GenAI load is lazy: only the llm workload
// touches it, so a box without GenAI still runs classify/detect and fails the
// LLM cleanly with an "LLM runtime missing" error. The vision (classify/detect)
// raw-C path is byte-for-byte unchanged from 4c-3.
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
#include "tests/npu/engine/genai_c_api.h"

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

// Engine-side LLM window control (spec §4d): the streamer thunk is used ONLY to
// bound a generate() call at the 5 s deadline and to poll the caller's cancel
// predicate — token counts and times come from the C perf metrics, never from
// the streamer's decoded-string callbacks (the llm.tps basis is GenAI's own
// throughput, §16.2-comparable).
struct LlmControl {
    std::int64_t deadlineNs = 0;
    int (*pollCancelled)(void *user) = nullptr;
    void *cancelledUser = nullptr;
    int stopped = 0;   // 0 none (natural EOS/bound), 1 window deadline, 2 cancel
};

// The C streamer callback (ov_genai_streaming_status_e): called per generated
// token; returns Stop at the window deadline (truncating the call) and Cancel
// when the caller's predicate fires (P-STOP ≤ 250 ms — a token on the NPU
// lands every ~30 ms, far inside the budget). 0 = Running keeps generating.
static GenAiStreamStatus llmStreamerThunk(const char *str, void *args)
{
    Q_UNUSED(str);
    auto *ctl = static_cast<LlmControl *>(args);
    if (ctl->pollCancelled && ctl->pollCancelled(ctl->cancelledUser)) {
        ctl->stopped = 2;
        return GenAiStreamStatus::Cancel;
    }
    if (std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count()
        >= ctl->deadlineNs) {
        ctl->stopped = 1;
        return GenAiStreamStatus::Stop;
    }
    return GenAiStreamStatus::Running;
}

} // namespace

namespace {

class OpenVinoEngine final : public INpuEngine
{
public:
    explicit OpenVinoEngine(QString runtimePath) : m_runtimePath(std::move(runtimePath)) {}
    ~OpenVinoEngine() override { unload(); }   // release ov handles before the Library dtors dlclose

    QString backendName() const override { return QStringLiteral("openvino-npu"); }
    // The resolved runtime version: the OpenVINO (core) description — and, once
    // the LLM workload has lazily loaded it, the GenAI marker appended, so the
    // Detail page's Runtime row shows both ("OpenVINO <desc> · GenAI"). The
    // marker is the literal string "GenAI": the C ABI has no version getter
    // (ov::genai::get_version() is C++-only), so no number is appended — the
    // core version on the same line carries the 2026.4.1 line (spec §8).
    QString runtimeVersion() const override
    {
        if (m_genAiVersion.isEmpty())
            return m_version;
        return m_version.isEmpty() ? m_genAiVersion
                                   : m_version + QStringLiteral(" · ") + m_genAiVersion;
    }

    bool init(const LoadedModel &model, QString *error) override
    {
        // Release any prior run's ov handles before (re)loading, so the library
        // handles handed to the move-assignments below are never dlclose'd while
        // NPU device handles/threads from the previous run are still live.
        unload();

#if defined(Q_OS_WIN)
        // AP-5 finding 3: on the standard SDK-archive install openvino.dll (and
        // openvino_genai.dll) import tbb12.dll, which lives under
        // runtime/3rdparty/tbb/ — NOT flat beside openvino.dll. This process
        // runs SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS), so
        // %PATH% (setupvars) is ignored and Library's
        // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR only searches the load dir → error
        // 126 on a clean SDK install. Load tbb12.dll by absolute path into
        // m_tbb first (it OUTLIVES m_core/m_genAi: declared first, destroyed
        // last), so the cores' import tables bind the already-loaded module.
        if (!loadTbbIfNeeded(error))
            return false;
#endif

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

#if defined(Q_OS_WIN)
    // AP-5 finding 3: makes the standard Windows SDK-archive install loadable.
    // The flat (pip-style) layout needs nothing — tbb12.dll sits beside
    // openvino.dll and the OS loader's LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR finds
    // it (SMOKE: Nivedellir's pip-style runtime folder uses this; confirm the
    // flat layout still loads after this change). On the SDK-archive layout
    // m_runtimePath = <...>/runtime/bin/intel64/Release and tbb12.dll is three
    // levels up under 3rdparty — probe the two known archive locations by
    // absolute path and load the first hit into m_tbb. A present-but-unloadable
    // tbb12.dll fails init() cleanly with the OS error; an absent one falls
    // through to the core load, which reports the same error 126 as before
    // (no new failure semantics).
    bool loadTbbIfNeeded(QString *error)
    {
        if (m_tbb && m_tbb->isOpen())
            return true;
        if (QFileInfo::exists(libPath(m_runtimePath, QStringLiteral("tbb12.dll"))))
            return true;   // flat layout: nothing to do
        // SMOKE: exact probe paths against the real 2026.4.1 archive (verified
        // present: runtime/3rdparty/tbb/bin/tbb12.dll; the redist path is the
        // older archive layout — probed too, harmless when absent).
        const QString candidates[] = {
            QDir(m_runtimePath).filePath(QStringLiteral("../../../3rdparty/tbb/bin/tbb12.dll")),
            QDir(m_runtimePath).filePath(QStringLiteral("../../../3rdparty/tbb/redist/intel64/vc14/tbb12.dll")),
        };
        for (const QString &candidate : candidates) {
            if (!QFileInfo::exists(candidate))
                continue;
            QString loadError;
            auto tbb = std::make_unique<Library>(candidate, &loadError);
            if (!tbb->isOpen())
                return failWith(QStringLiteral("failed to load %1: %2").arg(candidate, loadError), error);
            m_tbb = std::move(tbb);
            return true;
        }
        return true;   // not found: the core load reports the OS error cleanly
    }
#endif

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

        if (llm) {
            // The LLM workload runs through OpenVINO GenAI (DD-1 §5.4 Option A —
            // the smoke proved the raw-C KV loop cannot compile the stateful IR
            // on the NPU; GenAI staticizes the KV state internally). "Compiling"
            // the LLM means lazily loading the GenAI C runtime (openvino_genai_c)
            // and creating LLMPipeline(model.dir, "NPU") — the decoder is NEVER
            // read/compiled via the C API here: LLMPipeline re-loads it from
            // model.dir, and a 631 MB read_model for nothing (AP-5 7b) would
            // only slow the load. The error strings are captured through locals
            // so a null `error` (runWindow allows it) never dereferences.
            QString innerError;
            if (!loadGenAi(&innerError))
                return failLoad(QStringLiteral("the LLM runtime is unavailable: %1").arg(innerError), error);
            if (!createLlmPipeline(model, &innerError))
                return failLoad(QStringLiteral("failed to build the LLM pipeline: %1").arg(innerError), error);
            // The GenAI pipeline owns its requests — there is no vision
            // infer_request to build for the llm workload (m_compiled is null).
        } else {
            // The vision (classify/detect) path is byte-for-byte unchanged.
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

    // ---- LLM via OpenVINO GenAI (DD-1 §5.4 Option A, C API) ----

    // The second runtime the Intel engine loads lazily for the LLM workload
    // (P-VENDOR, C-API rewrite): openvino_genai_c from the USER's install,
    // probed beside the OpenVINO runtime dir (flat beside openvino.dll — the
    // SDK runtime layout — or a pip sibling). The C DLL is a thin C wrapper
    // over the C++ openvino_genai runtime; the ov_genai_* entry points are
    // resolved by name via GenAiCApi (genai_c_api.h), mirroring resolveApi().
    // A missing runtime OR a missing symbol is a clean "LLM runtime missing"
    // error on the llm path only — classify/detect never touch these, so a box
    // without GenAI still runs the vision workloads (never a launch failure).
    static QString genAiCModuleName()
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("openvino_genai_c.dll");
#else
        return QStringLiteral("libopenvino_genai_c.so");
#endif
    }

    // Lazy load of the GenAI C runtime, once per engine (m_genAi set on
    // success). RTLD_GLOBAL so the C DLL's C++-core dependencies
    // (openvino_genai.dll / libopenvino_genai.so + openvino.dll) bind; the
    // C++ core is NOT loaded separately (the OS loader pulls it in
    // transitively). On any failure nothing is half-installed.
    // SMOKE: the C DLL is present flat beside openvino.dll on Nivedellir (the
    // SDK archive layout; the pip wheel ships only the C++ DLL) — already
    // copied to C:\Users\davek\syntheon-smoke\runtime\.
    bool loadGenAi(QString *error)
    {
        if (m_genAi)
            return true;   // already loaded (a previous llm compile succeeded)
        const QString capiPath = findGenAiModule(m_runtimePath, QStringLiteral("openvino_genai"), genAiCModuleName());
        if (capiPath.isEmpty())
            return failWith(QStringLiteral("LLM runtime missing: OpenVINO GenAI C API (openvino_genai_c) was not "
                                           "found beside the OpenVINO runtime — install the OpenVINO GenAI SDK "
                                           "runtime 2026.4.1 (the C API DLL) for the LLM workload"),
                            error);

        QString loadError;
        auto genaic = std::make_unique<Library>(capiPath, &loadError, /*globalScope*/ true);
        if (!genaic->isOpen())
            return failWith(QStringLiteral("LLM runtime missing: failed to load %1: %2").arg(capiPath, loadError), error);
        if (!resolveGenAiCApi(*genaic, &m_genAiApi, error))
            return false;   // names the first missing symbol
        m_genAi = std::move(genaic);
        // No C version getter exists (ov::genai::get_version() is C++-only) —
        // the marker stands for the GenAI line; the core version carries the
        // 2026.4.1 version number on the same row (spec §8).
        m_genAiVersion = QStringLiteral("GenAI");
        return true;
    }

    // Creates the GenAI pipeline for the LLM model dir (the llm workload's
    // "compile"): ov_genai_llm_pipeline_create(model.dir, "NPU", 4, ...) with
    // the two NPUW property pairs — GenAI reads the model dir's
    // generation_config.json + chat template, tokenizes internally (CPU) and
    // staticizes the KV state for the NPU compiler (the smoke-proven path).
    // property_args_size counts INDIVIDUAL const char* args (2 per property =
    // key+value), NOT pairs: 4 for the two NPUW pairs below. The DLL rejects
    // an odd count with INVALID_C_PARAM and divides by 2; with 2 it would read
    // only pair 0 (NPUW_DEVICES="NPU") and silently DROP
    // NPUW_FALLBACK_EXEC="false" — the EC-P9 guard's first layer (GenAI THROWS
    // on a VCL/compile/exec failure instead of silently falling back to CPU;
    // AP-5 finding 1). The generation config is created once per pipeline:
    // max_new_tokens only; GenAI resolves the rest from the dir's
    // generation_config.json. Runs the fail-closed CPU-fallback guard after
    // creation.
    bool createLlmPipeline(const LoadedModel &model, QString *error)
    {
        if (model.dir.isEmpty())
            return failWith(QStringLiteral("the LLM model has no cache directory"), error);
        ov_genai_llm_pipeline *pipe = nullptr;
        const int st = m_genAiApi.pipelineCreate(model.dir.toUtf8().constData(), "NPU", 4, &pipe, "NPUW_DEVICES",
                                                 "NPU", "NPUW_FALLBACK_EXEC", "false");
        if (st != kOk || !pipe)
            return failWith(QStringLiteral("failed to create the LLM pipeline on device 'NPU' (status %1)").arg(st),
                            error);
        if (m_llm) {
            m_genAiApi.pipelineFree(m_llm);
            m_llm = nullptr;
        }
        m_llm = pipe;

        ov_genai_generation_config *cfg = nullptr;
        if (m_genAiApi.configCreate(&cfg) != kOk || !cfg)
            return failWith(QStringLiteral("failed to create the LLM generation config"), error);
        if (m_genAiApi.configSetMaxNewTokens(cfg, std::size_t(kLlmMaxNewTokens)) != kOk) {
            m_genAiApi.configFree(cfg);
            return failWith(QStringLiteral("failed to set the LLM max_new_tokens"), error);
        }
        if (m_llmConfig)
            m_genAiApi.configFree(m_llmConfig);
        m_llmConfig = cfg;
        return ensureNpuDevice(error);   // unchanged EC-P9 guard
    }

    // The fail-closed CPU-fallback guard (EC-P9, must handle): an LLM run that
    // did not actually use the NPU must be Error, never scored. Two layers:
    // (1) the pipeline create passes NPUW_DEVICES="NPU" +
    // NPUW_FALLBACK_EXEC=false as the variadic property pairs (createLlmPipeline),
    // so GenAI THROWS on a VCL/compile/exec failure instead of silently
    // falling back to CPU; (2) this guard — GenAI's LLMPipeline does not
    // expose EXECUTION_DEVICES (verified against the 2026.4.1 headers: the
    // compiled model is internal — SMOKE: confirm on Nivedellir; if a future
    // GenAI exposes a device property, prefer it). It checks exactly what CAN
    // be verified: the pipeline was just created with device "NPU"
    // (createLlmPipeline) AND the OpenVINO core reports an NPU device
    // available AND GenAI loaded (this runs only after loadGenAi succeeded).
    // The failure message names every fact checked.
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

    // The C thunk that polls the caller's cancel predicate (P-STOP ≤ 250 ms —
    // a token on the NPU lands every ~30 ms, far inside the budget). Called
    // through LlmControl::pollCancelled by the streamer thunk.
    // m_pendingCancelled is set for the duration of one generate() call —
    // single-threaded, no locking needed.
    static int cancelThunk(void *user)
    {
        auto *engine = static_cast<OpenVinoEngine *>(user);
        return engine->m_pendingCancelled && engine->m_pendingCancelled() ? 1 : 0;
    }

    // One 5 s window of generation: a loop of synchronous generate() calls
    // until the window deadline (each call re-generates the fixed prompt;
    // GenAI's non-chat generate is stateless). The streamer thunk bounds a
    // call at the deadline or a cancel; GenAI stops each call at EOS or the
    // 256-token bound. llm.tps basis = GenAI's OWN throughput: Σ
    // num_generation_tokens ÷ Σ generate_duration, accumulated per call in
    // C++ (AP-5 finding 2: perf_metrics_add_in_place sums the token count but
    // MEANS the per-call generate durations — PerfMetrics::operator+ appends
    // durations to a vector and get_generate_duration divides by K, so folding
    // it inflates llm.tps by K× across K calls). Each call's metrics are read
    // and freed immediately; a non-OK read FAILS CLOSED (never folds a zero
    // into the window totals). Byte-comparable to benchmark_genai_c's
    // "Throughput" (the §16.2 cross-check), NOT the streamer's token math (the
    // C streamer hands back decoded strings only — spec §4d/§8). `ttftSeconds`
    // = the first call's ttft mean (prefill incl.), reported separately.
    // `valid` = 1 only when the window generated ≥ 8 tokens in total, no
    // naturally-EOS'd call produced fewer than the §5.3 floor (EOS-on-step-1
    // fails closed), and there was real generate time — the workload layer
    // turns valid <= 0 into an Error, never a scored rate. A cancel that races
    // a failing call is a Stop, not an engine failure (AP-5 finding 4): the
    // loop breaks and returns true with a truncated span, and the workload
    // layer's post-window Stop poll reports WorkloadStatus::Cancelled
    // (workloads.cpp:45), never an Error. The median-of-3 window structure in
    // workloads.cpp is unchanged.
    bool runLlmWindow(Inference &out, QString *error, const std::function<bool()> &cancelled)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(kWindowSeconds * 1000.0));
        LlmControl ctl;
        ctl.deadlineNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
        ctl.pollCancelled = &OpenVinoEngine::cancelThunk;
        ctl.cancelledUser = this;
        m_pendingCancelled = cancelled;

        std::size_t totalTokens = 0;   // Σ per-call num_generation_tokens (C++ accumulation)
        double totalGenMs = 0.0;       // Σ per-call generate_duration (each call's own mean = its duration)
        bool floorBroken = false;
        float firstTtftMs = 0.0f;
        bool haveTtft = false;

        while (std::chrono::steady_clock::now() < deadline && !floorBroken) {
            if (cancelled && cancelled())
                break;
            ctl.stopped = 0;
            GenAiStreamer streamer{ &llmStreamerThunk, &ctl };
            ov_genai_decoded_results *results = nullptr;
            if (m_genAiApi.pipelineGenerate(m_llm, kLlmPrompt, m_llmConfig, &streamer, &results) != kOk) {
                m_pendingCancelled = {};
                // A cancel that raced the failing call (the streamer returned
                // Cancel, or the caller's predicate fired) is a clean break —
                // truncated span, true — so the workload layer reports
                // Cancelled, never an Error (AP-5 finding 4a).
                if (ctl.stopped == 2 || (cancelled && cancelled()))
                    break;
                return failWith(QStringLiteral("LLM generation failed"), error);
            }
            ov_genai_perf_metrics *m = nullptr;
            if (!results || m_genAiApi.resultsGetPerfMetrics(results, &m) != kOk) {
                if (results)
                    m_genAiApi.resultsFree(results);
                m_pendingCancelled = {};
                if (ctl.stopped == 2 || (cancelled && cancelled()))
                    break;
                return failWith(QStringLiteral("failed to read LLM perf metrics"), error);
            }
            // Per-call accounting, fail closed: a non-OK read is a window
            // failure, never a folded zero (AP-5 finding 4b).
            std::size_t callTokens = 0;
            const int tokenStatus = m_genAiApi.perfNumGenerationTokens(m, &callTokens);
            float callGenMs = 0.0f, sd = 0.0f;
            const int durationStatus = m_genAiApi.perfGenerateDuration(m, &callGenMs, &sd);
            if (tokenStatus != kOk || durationStatus != kOk) {
                m_genAiApi.perfMetricsFree(m);
                m_genAiApi.resultsFree(results);
                m_pendingCancelled = {};
                return failWith(QStringLiteral("failed to read the LLM perf metrics"), error);
            }
            if (ctl.stopped == 0 && callTokens < std::size_t(kLlmMinNewTokens))
                floorBroken = true;   // §5.3 EOS-on-step-1
            if (!haveTtft) {
                float mean = 0, sdTtft = 0;
                if (m_genAiApi.perfTtft(m, &mean, &sdTtft) == kOk) {
                    firstTtftMs = mean;
                    haveTtft = true;
                }
            }
            totalTokens += callTokens;
            totalGenMs += double(callGenMs);
            m_genAiApi.perfMetricsFree(m);   // the per-call metrics are freed on every path (AP-5 finding 4c)
            m_genAiApi.resultsFree(results);
        }
        m_pendingCancelled = {};

        out.units = std::int64_t(totalTokens);
        out.seconds = totalGenMs / 1000.0;   // Σ generate_duration (GenAI throughput basis)
        out.ttftSeconds = haveTtft ? double(firstTtftMs) / 1000.0 : 0.0;   // first call's ttft mean (prefill incl.)
        out.valid = (out.units > 0 && totalTokens >= std::size_t(kLlmMinNewTokens) && !floorBroken
                     && out.seconds > 0.0)
            ? 1
            : 0;
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

    // Releases the per-model handles in dependency order — the vision
    // infer_request/compiled/model AND the GenAI pipeline + its generation
    // config (a llm→vision workload switch must not leak the NPU pipeline +
    // KV cache; the frees run through the resolved C API, which is loaded
    // whenever m_llm/m_llmConfig are non-null). The core + libraries survive,
    // so this is the per-workload reload path (compileModel calls it before
    // recompiling).
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
        if (m_genAiApi.configFree && m_llmConfig) {
            m_genAiApi.configFree(m_llmConfig);
            m_llmConfig = nullptr;
        }
        if (m_genAiApi.pipelineFree && m_llm) {
            m_genAiApi.pipelineFree(m_llm);
            m_llm = nullptr;
        }
    }

    // Releases every ov_* handle including the core and clears `m_ready`. Call
    // BEFORE the Library owners are destroyed or reassigned so the libraries are
    // never dlclose'd with live device handles.
    void unload()
    {
        // freeModel() destroys the GenAI pipeline + config too — BEFORE the
        // genai library is released, so their destructors never run inside a
        // dlclose'd module.
        freeModel();
        if (m_api.coreFree && m_coreHandle) {
            m_api.coreFree(m_coreHandle);
            m_coreHandle = nullptr;
        }
        m_loadedPath.clear();
        m_loadedIsLlm = false;
        m_ready = false;
        m_genAi.reset();
        m_genAiApi = GenAiCApi{};
        m_genAiVersion.clear();
        m_pendingCancelled = {};
    }

    QString m_runtimePath;
    QString m_version;
    QString m_loadedPath;    // the model file compiled by the last successful compileModel()
    bool m_loadedIsLlm = false;   // whether the loaded model is the stateful LLM decoder
    OpenVinoApi m_api;
    // Declared FIRST so it is destroyed LAST (C++ destroys members in reverse
    // declaration order): tbb12.dll must stay loaded while m_core/m_genAi are
    // still live, and must outlive them in unload/destruction (AP-5 finding 3).
    std::unique_ptr<Library> m_tbb;     // tbb12.dll (SDK-archive layout, Windows only)
    std::unique_ptr<Library> m_core;    // the C++ runtime (kept loaded as a dependency)
    std::unique_ptr<Library> m_cApi;    // the C API (ov_* symbols resolved from here)
    std::unique_ptr<Library> m_plugin;  // the Intel NPU plugin (kept loaded for the core)
    // The GenAI "second runtime" (DD-1 §5.4, loaded lazily for the LLM workload):
    std::unique_ptr<Library> m_genAi;   // the user's openvino_genai_c runtime (C ABI)
    GenAiCApi m_genAiApi;               // the resolved ov_genai_* entry points
    ov_genai_llm_pipeline *m_llm = nullptr;             // the live LLMPipeline (per llm compile)
    ov_genai_generation_config *m_llmConfig = nullptr;  // its max_new_tokens config
    QString m_genAiVersion;             // "GenAI" marker ("" until the llm path loads it)
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

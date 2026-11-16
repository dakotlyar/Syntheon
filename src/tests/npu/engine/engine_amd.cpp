// SPDX-License-Identifier: GPL-3.0-or-later
// The AMD NPU engine (Ryzen AI / Vitis AI EP), Phase 4c design §4.1. dlopens the
// USER-INSTALLED ONNX Runtime (onnxruntime.dll / libonnxruntime.so) plus the
// Vitis AI EP library from RYZEN_AI_INSTALLATION_PATH / the default RyzenAI
// deployment dir (the runtime path resolve() handed us — detect reused it), and
// resolves ONNX Runtime's C entry point OrtGetApiBase with null-checks. No vendor
// header or .lib is compiled in (P-VENDOR); any load/resolve failure is a clean
// init() == false + a clear error, never a throw.
//
// HONEST SCOPE NOTE (Antilles / AP-5 / §16): the stable, hand-pinnable part of
// ONNX Runtime's C ABI is OrtGetApiBase() + the 2-member OrtApiBase table
// (GetApi → GetVersionString), reproduced below verbatim from
// onnxruntime_c_api.h. Driving a real session — CreateSessionOptions,
// AppendExecutionProvider_VitisAI (ORT ≥ 1.18, ~500 fields deep in the versioned
// OrtApi table), CreateSession, the tensor-IO Run path, and AMD's proprietary
// OrtVitisAIProviderOptions — requires transcribing a large, versioned
// function-pointer table by hand, which is precisely the fragile ABI this shim
// must not invent. That orchestration is therefore DEFERRED to real-hardware
// validation (§16), where the installed Ryzen AI headers can be diffed exactly.
// This engine resolves everything it can safely pin today — the runtime loads,
// the Vitis AI EP library is found, and a real ONNX Runtime version is reported
// — and then fails cleanly with an exact "measurement deferred" error rather
// than risk a silent CPU-fallback score (an EC-P +2 silently-wrong result).

#include "tests/npu/engine/engine.h"
#include "tests/npu/engine/engine_dlopen.h"

#include <QDir>

#include <cstdint>
#include <memory>
#include <utility>

namespace sb::npu {

namespace {

using namespace sb::npu::engine_internal;

// ---- Minimal ONNX Runtime C ABI (verbatim from onnxruntime_c_api.h) ----

using OrtApiVersion = std::uint32_t;

// ORTCHAR_T: wchar_t on Windows, char elsewhere (path handling, verbatim).
#if defined(Q_OS_WIN)
using OrtCharT = wchar_t;
#  define ORT_CCALL __stdcall
#else
using OrtCharT = char;
#  define ORT_CCALL
#endif

struct OrtApiBase;   // 2 members below; the runtime owns it
struct OrtApi {      // the (large, versioned) function table — NOT transcribed
    char _opaque_;
};
struct OrtSession;
struct OrtSessionOptions;

// OrtApiBase: stable since ORT 1.0 (verbatim layout).
struct OrtApiBase {
    const OrtApi *(ORT_CCALL *GetApi)(OrtApiVersion version);
    const char *(ORT_CCALL *GetVersionString)(void);
};

// The free entry point exported by the runtime.
using OrtGetApiBaseFn = const OrtApiBase *(ORT_CCALL *)(void);

QString coreName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("onnxruntime.dll");
#else
    return QStringLiteral("libonnxruntime.so");
#endif
}
QString epName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("vitisai_ep.dll");
#else
    return QStringLiteral("libvitisai_ep.so");
#endif
}

} // namespace

namespace {

class AmdEngine final : public INpuEngine
{
public:
    explicit AmdEngine(QString runtimePath) : m_runtimePath(std::move(runtimePath)) {}

    QString backendName() const override { return QStringLiteral("ryzenai-vitis"); }
    QString runtimeVersion() const override { return m_version; }

    bool init(const LoadedModel &model, QString *error) override
    {
#if !defined(Q_OS_WIN)
        // Linux AMD XDNA is the XRT path (D1, still undecided), not Ryzen AI /
        // Vitis AI EP — the "always compiled" engine fails cleanly rather than
        // inventing the XRT path inside the Windows-centric Vitis AI engine.
        (void)model;
        return failWith(
            QStringLiteral("AMD Ryzen AI / Vitis AI EP runs only on Windows; AMD XDNA on Linux is the XRT path (D1)"),
            error);
#endif
        // Load the runtime + the Vitis AI EP library, both beside the runtime
        // path detect resolved (the RyzenAI deployment dir).
        auto onnx = std::make_unique<Library>(QDir(m_runtimePath).filePath(coreName()), error);
        if (!onnx->isOpen())
            return false;
        auto ep = std::make_unique<Library>(QDir(m_runtimePath).filePath(epName()), error);
        if (!ep->isOpen())
            return false;

        // The one stable, hand-pinnable entry point: OrtGetApiBase → OrtApiBase.
        const OrtGetApiBaseFn getApiBase = resolve<OrtGetApiBaseFn>(*onnx, "OrtGetApiBase", error);
        if (!getApiBase)
            return false;
        const OrtApiBase *base = getApiBase();
        if (!base)
            return failWith(QStringLiteral("OrtGetApiBase returned null"), error);

        // A real ONNX Runtime version, from the stable 2-member OrtApiBase.
        if (base->GetVersionString)
            m_version = QString::fromUtf8(base->GetVersionString());

        m_onnx = std::move(onnx);
        m_ep = std::move(ep);
        m_getApiBase = getApiBase;
        (void)model;

        // The session/providers/run orchestration needs the versioned OrtApi table
        // (~500 fields through SessionOptionsAppendExecutionProvider_VitisAI, ORT
        // ≥ 1.18) + AMD's OrtVitisAIProviderOptions — deferred to §16, see the
        // file header. Fail cleanly and precisely rather than invent or fall back
        // to a CPU session (which would be a silently wrong NPU score).
        return failWith(
            QStringLiteral("AMD Ryzen AI runtime %1 loaded; the Vitis AI session ABI is deferred "
                           "to real-hardware validation (§16) — model %2 was not compiled")
                .arg(m_version.isEmpty() ? QStringLiteral("(unknown version)") : m_version, model.name),
            error);
    }

    bool runWindow(const QString &, const LoadedModel &, Inference &out, QString *error) override
    {
        out.units = 0;
        out.valid = 0;
        return failWith(
            QStringLiteral("the Vitis AI measurement loop is not yet pinned (deferred to §16 validation)"),
            error);
    }

private:
    static bool failWith(const QString &message, QString *error)
    {
        if (error)
            *error = message;
        return false;
    }

    QString m_runtimePath;
    QString m_version;
    std::unique_ptr<Library> m_onnx;
    std::unique_ptr<Library> m_ep;
    OrtGetApiBaseFn m_getApiBase = nullptr;
};

} // namespace

std::unique_ptr<INpuEngine> makeRyzenAiEngine(const QString &runtimePath)
{
    return std::make_unique<AmdEngine>(runtimePath);
}

} // namespace sb::npu

// SPDX-License-Identifier: GPL-3.0-or-later
// The Qualcomm NPU engine (QNN / QAIRT, Windows on ARM64), Phase 4c design §4.1.
// dlopens the QNN core libraries from the user's QAIRT install (the runtime path
// detect resolved), resolves the QNN C bootstrap symbol QnnInterface_getProviders
// with null-checks, and loads a prepared context binary (.bin). No vendor header
// or .lib is compiled in (P-VENDOR); any load/resolve failure is a clean
// init() == false + a clear error, never a throw.
//
// QNN is by far the LEAST-documented of the three vendors (design §4.1, [S+]):
// the exact C API shape and symbol set is Qualcomm-proprietary and versioned
// across QAIRT releases. Rather than invent a fragile ABI, this engine pins the
// one well-known bootstrap symbol (QnnInterface_getProviders) + the graph/context
// symbol *names* described in the QNN docs, and resolves them null-checked; the
// full graph→context→execute orchestration and the context-binary loading are
// DEFERRED to real-hardware validation (§16, Windows on Snapdragon X), where the
// installed QAIRT headers can be diffed exactly. Everything here compiles with
// no vendor SDK, fails cleanly on any unresolved symbol, and reports where it
// stopped precisely enough for the §16 validator to pick up.

#include "tests/npu/engine/engine.h"
#include "tests/npu/engine/engine_dlopen.h"

#include <QDir>

#include <cstdint>
#include <memory>
#include <utility>

namespace sb::npu {

#if defined(SYNTHEON_NPU_QNN_AVAILABLE)

namespace {

using namespace sb::npu::engine_internal;

// The QNN core libraries sit in the exact DLL directory detect's findQnn
// resolved (the dir that holds BOTH QnnHtp.dll and QnnSystem.dll, e.g.
// lib/aarch64-windows-msvc/). Windows-on-ARM names; the ARM64-only guard means
// no Linux/so naming is needed (N4).
QString htpName() { return QStringLiteral("QnnHtp.dll"); }
QString systemName() { return QStringLiteral("QnnSystem.dll"); }

// The QNN bootstrap symbol (the one entry point documented across QAIRT
// releases): fills a provider list used to choose the HTP backend.
// Exact signature flagged [UNCERTAIN]: `Qnn_ErrorHandle_t QnnInterface_getProviders(
//     const QnnInterface_t*** providerList, uint32_t* numProviders);`
using QnnGetProvidersFn = int (*)(const void ***providerList, std::uint32_t *numProviders);

class QnnEngine final : public INpuEngine
{
public:
    explicit QnnEngine(QString runtimePath) : m_runtimePath(std::move(runtimePath)) {}

    QString backendName() const override { return QStringLiteral("qnn"); }
    QString runtimeVersion() const override { return m_version; }   // "" (no stable version symbol pinned)

    bool init(const LoadedModel &model, QString *error) override
    {
        auto htp = std::make_unique<Library>(QDir(m_runtimePath).filePath(htpName()), error);
        if (!htp->isOpen())
            return false;
        auto system = std::make_unique<Library>(QDir(m_runtimePath).filePath(systemName()), error);
        if (!system->isOpen())
            return false;

        // The one well-documented bootstrap symbol, resolved + null-checked.
        const QnnGetProvidersFn getProviders = resolve<QnnGetProvidersFn>(*htp, "QnnInterface_getProviders", error);
        if (!getProviders)
            return failWith(QStringLiteral("QnnInterface_getProviders is missing from %1").arg(htp->path()), error);

        m_htp = std::move(htp);
        m_system = std::move(system);
        m_getProviders = getProviders;
        (void)model;

        // The QnnInterface_t provider table → backend → context-binary load
        // (QnnContext_create / QnnGraph_create / QnnGraph_execute) is a large,
        // versioned, proprietary ABI. Deferred to §16 rather than invented (see
        // the file header). Fail cleanly and precisely; do not guess.
        return failWith(
            QStringLiteral("Qualcomm QNN runtime loaded; the graph/context ABI is deferred to "
                           "real-hardware validation (§16) — model %1 was not loaded")
                .arg(model.name),
            error);
    }

    bool runWindow(const QString &, const LoadedModel &, Inference &out, QString *error,
                   const std::function<bool()> &) override
    {
        out.units = 0;
        out.valid = 0;
        return failWith(QStringLiteral("the QNN measurement loop is not yet pinned (deferred to §16 validation)"),
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
    QString m_version;                    // "" — no stable version entry point is pinned (see header)
    std::unique_ptr<Library> m_htp;
    std::unique_ptr<Library> m_system;
    QnnGetProvidersFn m_getProviders = nullptr;
};

} // namespace

std::unique_ptr<INpuEngine> makeQnnEngine(const QString &runtimePath)
{
    return std::make_unique<QnnEngine>(runtimePath);
}

#endif // SYNTHEON_NPU_QNN_AVAILABLE

} // namespace sb::npu

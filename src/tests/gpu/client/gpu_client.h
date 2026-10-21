// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU test's side of synbench-gpu (Phase 4a design §2.2-§2.5, D12, task
// 14): starts the worker with QProcess, reads its JSON lines, and kills it on
// Stop (polled every 50 ms), a timeout or a protocol error. Killing a process
// is safe, unlike a thread; the worker also exits by itself when its stdin
// (held open here) closes, so it never outlives SynBench.
//
// The worker's environment is built from scratch (§2.2, §4.4), never
// inherited: the display variables and little else, so QT_PLUGIN_PATH,
// LD_LIBRARY_PATH, driver overrides and the like never reach it. On Linux
// VK_LOADER_LAYERS_DISABLE=~implicit~ keeps overlays and recorders out.
//
// Two ways to use it: GpuClient::run() for one worker with a plan (the sizing
// run, the native 2D paths, tests), and GpuSession for the GPU test's
// scene-by-scene order (D12): one long-lived `--serve` worker per API, driven
// by one `run` command per workload. Everything blocks and delivers events on
// the caller's thread, so progress is only ever reported from the run()
// thread (no sampler race).

#include "tests/gpu/model/gpu_identity.h"
#include "tests/gpu/model/protocol.h"

#include <QMap>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVector>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

class QProcess;

namespace sb::gpu {

struct ClientRunSpec {
    QString api;
    QString gpuId;
    QVector<protocol::PlanItem> plan;
    QProcessEnvironment extraEnv;
    std::optional<std::chrono::milliseconds> timeout;  // tests: instead of timeoutFor(plan)
};

struct ClientOutcome {
    enum class Status { Ok, Cancelled, NotAvailable, Failed };
    Status status = Status::Failed;
    QString reason;
    std::optional<protocol::ReadyEvent> ready;
    QVector<protocol::ResultEvent> results;
    QMap<QString, QString> workloadErrors;  // workload -> reason (error events that weren't fatal to the process)
    int exitCode = -1;
    qint64 pid = 0;
    bool ok() const { return status == Status::Ok; }
};

class GpuClient
{
public:
    // The worker: next to the app (Windows, a dev build), or ../libexec from it
    // (an install, the AppImage's usr/libexec). Empty if it isn't there.
    static QString workerPath();
    // §2.2/§4.4: from scratch. Linux: DISPLAY, WAYLAND_DISPLAY, XDG_RUNTIME_DIR,
    // XAUTHORITY, XDG_SESSION_TYPE, XDG_CURRENT_DESKTOP, DBUS_SESSION_BUS_ADDRESS,
    // HOME, LANG, and VK_LOADER_LAYERS_DISABLE=~implicit~. Windows: SystemRoot,
    // LOCALAPPDATA, APPDATA, USERPROFILE, TEMP, TMP, and PATH = <SystemRoot>\System32;<the worker's folder>.
    // Dev hook SYNBENCH_GPU_WORKER_ENV ("K=V|K=V") adds to it.
    static QProcessEnvironment baseEnvironment();
    // Dev hook SYNBENCH_GPU_WORKER_ARGS, split on spaces; appended after the usual arguments.
    static QStringList extraArgs();
    // 2 x the sum of (warm-up + measure) + 30 s.
    static std::chrono::milliseconds timeoutFor(const QVector<protocol::PlanItem> &plan);
    // The longest a single workload may take: the same rule for one item.
    static std::chrono::milliseconds timeoutFor(const protocol::PlanItem &item);

    // One worker, one plan; blocking. `onEvent` hears every event on the caller's thread.
    // Exit 0 -> Ok, 4 -> Cancelled, 5 -> NotAvailable (the reason from its error
    // event), anything else, a crash, a timeout or a protocol error -> Failed.
    static ClientOutcome run(const ClientRunSpec &spec, const std::function<bool()> &cancelled,
                             const std::function<void(const protocol::Event &)> &onEvent = {});
    // `--probe`: the GPU list (empty + *error on failure).
    // *timedOut (optional) says the probe ran out of time (D22), as opposed to failing.
    static QVector<GpuEntry> probe(std::chrono::milliseconds timeout, QString *error, bool *timedOut = nullptr);
    // `--probe-gl --gpu <id>` with the GL environment glEnv.
    static protocol::GlEvent probeGl(const GpuEntry &gpu, const QProcessEnvironment &glEnv, std::chrono::milliseconds timeout);
#if defined(SYNBENCH_DEV_HOOKS)
    // Tests: run `program` (with `extraArgs` before the usual ones) instead of
    // the real worker; an empty program restores the real one.
    static void setWorkerForTests(const QString &program, const QStringList &extraArgs = {});
#endif
};

// One long-lived `--serve` worker (D12, §2.5).
class GpuSession
{
public:
    GpuSession();
    ~GpuSession();  // kill() if still running
    GpuSession(const GpuSession &) = delete;
    GpuSession &operator=(const GpuSession &) = delete;

    // Starts `synbench-gpu --run --serve --api <api> --gpu <id>` (+ extraArgs()),
    // environment baseEnvironment() + extraEnv. Returns at once; call waitReady()
    // after starting all sessions.
    bool start(const QString &api, const QString &gpuId, const QProcessEnvironment &extraEnv, QString *error);
    // Blocks until `ready` (Ok, `ready` filled), an error or exit 5 (NotAvailable),
    // an exit, a timeout or a cancel.
    ClientOutcome waitReady(std::chrono::milliseconds timeout, const std::function<bool()> &cancelled);
    // One `run` command; blocks until `idle`. Timeout = 2 x (warm-up + measure) + 30 s.
    // A result -> Ok with results[0]; an error event then idle -> Failed with
    // workloadErrors filled (an error with na:true -> NotAvailable); the worker
    // exiting -> Failed ("the GPU worker exited with code N: ..."); cancel ->
    // writes `stop`, Cancelled.
    ClientOutcome runWorkload(const protocol::PlanItem &item, const std::function<bool()> &cancelled,
                              const std::function<void(const protocol::Event &)> &onEvent = {});
    void quit();  // "quit\n", wait up to 2 s for exit 0, then kill()
    void kill();
    bool alive() const;
    qint64 pid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_d;
};

} // namespace sb::gpu

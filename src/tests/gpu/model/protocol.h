// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU worker's JSON-lines protocol and its --plan text (Phase 4a design
// §2.3, §2.4). One JSON object per line, at most kMaxLine bytes, UTF-8.
// Unknown keys are ignored; a missing required key is a protocol error. Pure.

#include "tests/gpu/model/gpu_identity.h"

#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QVector>

#include <optional>
#include <variant>

namespace sb::gpu::protocol {

struct PlanItem {
    QString workload;
    int warmupMs = 0, measureMs = 0;
    bool operator==(const PlanItem &) const = default;
};
QString planText(const QVector<PlanItem> &plan);  // "geometry:5000:20000,drawcalls:3000:20000"
// A bad item -> nullopt and "bad plan item 'x'"; an empty text -> an empty list.
std::optional<QVector<PlanItem>> parsePlan(const QString &text, QString *error);

struct GpuEvent {
    GpuEntry gpu;
};
struct GlEvent {
    bool ok = false;
    QString version, renderer, vendor, note, reason;
    quint32 vendorId = 0;
};
struct ReadyEvent {
    QString api, device, version, driver, present, note, layers, screen;
    int renderW = 0, renderH = 0, windowW = 0, windowH = 0;
    double scale = 1;
    bool fit = false, gpuTimestamps = false;
};
struct StageEvent {
    QString workload, phase;  // phase: load|warmup|measure|verify
    int seconds = 0;
};
struct ResultEvent {
    QString workload;
    int frames = 0;
    double seconds = 0, fps = 0, ftP1 = 0, ftP50 = 0, ftP99 = 0, cpuMs = 0;
    std::optional<double> gpuMs;
    // Optional (checkpoint 5, H1): the fastest frame time in ms ("ftMs.min"), for §9's
    // "a frame time < 0.005 ms" rule in the GPU test; the real worker always sends it.
    std::optional<double> ftMin;
    QVector<QPointF> series;
    QByteArray signature;
    // Optional (Antilles' decision after task 9): the present mode resume()
    // chose for this workload, "IMMEDIATE" or "MAILBOX"; "" = not sent.
    QString present;
    // Optional (D1, Zeb's window checks): where the window actually ended up, since the
    // compositor, not the worker, chooses the screen. "" = not sent.
    QString screen;
    int renderW = 0, renderH = 0, windowW = 0, windowH = 0;  // render physical, window logical
    double scale = 0;
    bool fit = false;
    // Optional (checkpoint 6, M2; Windows): the GPU whose display output holds the window, when it isn't the GPU
    // rendering (a hybrid laptop); "" = the same GPU, or not known.
    QString displayGpu;
};
struct ErrorEvent {
    QString workload, reason;
    bool na = false;
};
// A warning the worker wants in the result without failing the run (task 28): e.g. Direct2D's
// fallback from the bundled font. {"ev":"warning","text":"..."}; the GPU test stores it as a note.
struct WarningEvent {
    QString text;
};
struct DoneEvent {};
// Serve mode (D12, §2.3): the worker finished a run command (after its
// result or error) and waits for the next one.
struct IdleEvent {};

using Event = std::variant<GpuEvent, GlEvent, ReadyEvent, StageEvent, ResultEvent, ErrorEvent, WarningEvent, DoneEvent, IdleEvent>;

std::optional<Event> parseLine(const QByteArray &line, QString *error);
QByteArray encode(const Event &event);  // compact JSON, no newline (QJsonDocument::Compact)

inline constexpr qsizetype kMaxLine = 64 * 1024;

} // namespace sb::gpu::protocol

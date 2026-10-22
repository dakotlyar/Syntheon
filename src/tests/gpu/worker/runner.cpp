// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/runner.h"

#include "tests/gpu/worker/scene/overlay.h"
#include "tests/gpu/worker/signature.h"

#include <cmath>
#include <optional>
#include <utility>

namespace sb::gpu {

namespace {

using Clock = Runner::Clock;

double millis(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

// A phase's stage events: one when it starts, then one per whole second (§2.4).
class Phase
{
public:
    Phase(const Runner::Hooks &hooks, Workload w, const char *name, Clock::time_point start)
        : m_hooks(hooks), m_workload(w), m_name(name), m_start(start)
    {
        send(0);
    }
    void tick(Clock::time_point now)
    {
        const int s = int(std::chrono::duration_cast<std::chrono::seconds>(now - m_start).count());
        if (s > m_sent) {
            m_sent = s;
            send(s);
        }
    }

private:
    void send(int s) const
    {
        if (m_hooks.stage)
            m_hooks.stage(m_workload, m_name, s);
    }
    const Runner::Hooks &m_hooks;
    Workload m_workload;
    const char *m_name;
    Clock::time_point m_start;
    int m_sent = 0;
};

// The overlay's FPS (§5.9): round(n / sum of frame ms x 1000) over the frames
// that ended in the last 0.5 s, refreshed every 0.5 s.
struct FpsWindow {
    Clock::time_point since;
    int frames = 0;
    double sumMs = 0;
    void add(double ms)
    {
        ++frames;
        sumMs += ms;
    }
    std::optional<int> take(Clock::time_point now)
    {
        std::optional<int> fps;
        if (frames > 0 && sumMs > 0)
            fps = int(std::lround(frames / sumMs * 1000.0));
        since = now;
        frames = 0;
        sumMs = 0;
        return fps;
    }
};

constexpr auto kOverlayPeriod = std::chrono::milliseconds(500);

} // namespace

const char *workloadName(Workload w)
{
    switch (w) {
    case Workload::Geometry: return "Geometry";
    case Workload::DrawCalls: return "Draw calls";
    case Workload::Shading: return "Shading";
    case Workload::Particles: return "Particles";
    case Workload::Quads: return "Quads";
    case Workload::Sprites: return "Sprites";
    case Workload::Shapes: return "Shapes";
    case Workload::Text: return "Text";
    case Workload::Blits: return "Image blits";
    case Workload::Gradients: return "Gradients";
    }
    return "";
}

const char *apiOverlayName(Api a)
{
    switch (a) {
    case Api::Vulkan: return "Vulkan";
    case Api::Dx12: return "DX12";
    case Api::Dx11: return "DX11";
    case Api::OpenGl: return "OpenGL";
    case Api::Null: return "Null";
    case Api::QtRaster: return "Qt raster";
    case Api::Direct2D: return "Direct2D";
    }
    return "";
}

const char *apiId(Api a)
{
    switch (a) {
    case Api::Vulkan: return "vulkan";
    case Api::Dx12: return "dx12";
    case Api::Dx11: return "dx11";
    case Api::OpenGl: return "opengl";
    case Api::Null: return "null";
    case Api::QtRaster: return "qt";
    case Api::Direct2D: return "d2d";
    }
    return "";
}

Runner::Runner(IBackend &backend, Hooks hooks) : Runner(backend, std::move(hooks), Settings{}) {}

Runner::Runner(IBackend &backend, Hooks hooks, Settings settings)
    : m_backend(backend), m_hooks(std::move(hooks)), m_settings(settings)
{
}

struct Runner::Driver {
    std::string apiName;
    std::function<std::string()> load;
    std::function<std::string(double, std::vector<double> *)> frame;
    std::function<std::string(std::vector<double> *)> drain;
    std::function<std::string(double, std::vector<std::uint8_t> *)> readback;
    std::function<void()> unload;
    std::function<void(const std::string &)> setOverlay;
};

WorkloadResult Runner::run(const WorkloadPlan &plan, const SceneData &scene)
{
    Driver d;
    d.apiName = apiOverlayName(m_backend.api());
    d.load = [&] { return m_backend.load(scene); };
    d.frame = [&](double t, std::vector<double> *gpu) { return m_backend.frame(t, gpu); };
    d.drain = [&](std::vector<double> *gpu) { return m_backend.drain(gpu); };
    d.readback = [&](double t, std::vector<std::uint8_t> *px) { return m_backend.readback(t, px); };
    d.unload = [&] { m_backend.unload(); };
    d.setOverlay = [&](const std::string &text) { m_backend.setOverlay(text); };
    return loop(plan, d);
}

WorkloadResult Runner::runNative(const WorkloadPlan &plan, const NativeOps &ops, INative2D &native)
{
    Driver d;
    d.apiName = native.overlayName();
    d.load = [&] { return native.load(ops); };
    d.frame = [&](double t, std::vector<double> *) { return native.frame(t); };  // no GPU timestamps: a CPU path or a CPU-timed one
    d.drain = [](std::vector<double> *) { return std::string(); };
    d.readback = [&](double t, std::vector<std::uint8_t> *px) { return native.readback(t, px); };
    d.unload = [&] { native.unload(); };
    d.setOverlay = [&](const std::string &text) { native.setOverlay(text); };
    return loop(plan, d);
}

WorkloadResult Runner::loop(const WorkloadPlan &plan, const Driver &driver)
{
    WorkloadResult r;
    r.workload = plan.workload;
    const auto fail = [&](std::string reason) {
        driver.unload();
        r.error = std::move(reason);
        return r;
    };
    const auto stopped = [&] { return m_hooks.stopRequested && m_hooks.stopRequested(); };

    Phase(m_hooks, plan.workload, "load", Clock::now());
    if (std::string e = driver.load(); !e.empty())
        return fail(std::move(e));
    if (stopped())
        return fail(kStopped);

    const std::string sceneName = workloadName(plan.workload), apiName = driver.apiName;
    const auto overlay = [&](std::optional<int> fps) {
        driver.setOverlay(m_settings.overlay ? overlayText(sceneName, apiName, fps) : std::string());
    };
    overlay(std::nullopt);  // "warming up"

    std::vector<double> gpu;  // every frame's GPU time, in frame order (warm-up included)
    std::size_t submitted = 0;
    const Clock::time_point warmStart = Clock::now();
    Clock::time_point prev = warmStart;
    FpsWindow fps{warmStart};
    std::optional<int> warmFps;  // the last 0.5 s of the warm-up, shown when the measurement starts
    // One frame: t is the time since the warm-up started (P6); *ret = the
    // present return, *cpuMs = the time inside frame().
    const auto oneFrame = [&](Clock::time_point *ret, double *cpuMs) -> std::string {
        if (m_hooks.pumpEvents)
            m_hooks.pumpEvents();
        if (stopped())
            return kStopped;
        const Clock::time_point before = Clock::now();
        std::string e = driver.frame(std::chrono::duration<double>(before - warmStart).count(), &gpu);
        *ret = Clock::now();
        *cpuMs = millis(*ret - before);
        ++submitted;
        return e;
    };

    // Warm-up: frames until warmupMs has passed; T0 = the first present
    // return at or after its end.
    const Clock::time_point warmEnd = warmStart + std::chrono::milliseconds(plan.warmupMs);
    Phase warm(m_hooks, plan.workload, "warmup", warmStart);
    Clock::time_point ret;
    do {
        double cpu = 0;
        if (std::string e = oneFrame(&ret, &cpu); !e.empty())
            return fail(std::move(e));
        fps.add(millis(ret - prev));
        prev = ret;
        // §5.9: the overlay says "warming up" for the whole warm-up; the rate it measures
        // meanwhile is kept for the first measured moment.
        if (ret - fps.since >= kOverlayPeriod) {
            if (const auto f = fps.take(ret))
                warmFps = f;
        }
        warm.tick(ret);
        // A screen or scale change during the warm-up is not an error: the compositor only reports
        // the window's real screen after its first frames. The caller re-plans and restarts (DEF-1).
        if (m_hooks.windowReplan && m_hooks.windowReplan())
            return fail(kReplan);
    } while (ret < warmEnd);
    const Clock::time_point t0 = ret;
    const std::size_t warmFrames = submitted;

    // The measured window: every interval until the first present return at
    // or after T0 + measureMs.
    Phase measure(m_hooks, plan.workload, "measure", t0);
    if (const auto f = fps.take(t0))
        overlay(f);
    else if (warmFps)
        overlay(warmFps);
    const Clock::time_point measureEnd = t0 + std::chrono::milliseconds(plan.measureMs);
    prev = t0;
    do {
        if (m_hooks.windowOk && !m_hooks.windowOk())
            return fail(kWindowLost);
        double cpu = 0;
        if (std::string e = oneFrame(&ret, &cpu); !e.empty())
            return fail(std::move(e));
        const double interval = millis(ret - prev);
        prev = ret;
        r.frameMs.push_back(interval);
        r.cpuMs.push_back(cpu);
        fps.add(interval);
        if (ret - fps.since >= kOverlayPeriod) {
            if (const auto f = fps.take(ret))
                overlay(f);
        }
        measure.tick(ret);
    } while (ret < measureEnd);

    if (std::string e = driver.drain(&gpu); !e.empty())
        return fail(std::move(e));
    // The measured frames' GPU times: frames warmFrames.. in submission order
    // (a backend that drops a frame's timestamps shifts the tail by one; the
    // mean is what is reported).
    for (std::size_t i = warmFrames; i < gpu.size() && r.gpuMs.size() < r.frameMs.size(); ++i)
        r.gpuMs.push_back(gpu[i]);
    r.frames = int(r.frameMs.size());
    for (const double ms : r.frameMs)
        r.seconds += ms / 1000.0;

    // The verification frame (§5.7), at a fixed time; not timed.
    Phase(m_hooks, plan.workload, "verify", Clock::now());
    std::vector<std::uint8_t> pixels;
    if (std::string e = driver.readback(plan.warmupMs / 1000.0 + 10.0, &pixels); !e.empty())
        return fail(std::move(e));
    const std::size_t want = std::size_t(m_settings.renderWidth) * m_settings.renderHeight * 4;
    if (pixels.size() != want)
        return fail("the readback returned " + std::to_string(pixels.size()) + " bytes, not "
                    + std::to_string(m_settings.renderWidth) + "x" + std::to_string(m_settings.renderHeight) + " RGBA8");
    r.signature = blockSignature(pixels.data(), m_settings.renderWidth, m_settings.renderHeight);
    driver.unload();
    return r;
}

} // namespace sb::gpu

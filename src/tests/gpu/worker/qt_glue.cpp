// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/qt_glue.h"

#include "tests/gpu/worker/worker_window.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVulkanInstance>

#include <cstring>
#include <memory>

namespace sb::gpu::glue {

PresentTarget headlessTarget()
{
    PresentTarget t;
    t.headless = true;
    return t;
}

std::vector<std::string> vulkanSurfaceExtensions()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1StringView("windows"))
        return {"VK_KHR_surface", "VK_KHR_win32_surface"};
    if (platform == QLatin1StringView("xcb"))
        return {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    if (platform.startsWith(QLatin1StringView("wayland")))
        return {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    return {};
}

PresentTarget windowlessTarget(Api api)
{
    PresentTarget t;
    if (api == Api::Vulkan)
        t.vulkanInstanceExtensions = vulkanSurfaceExtensions();
    return t;
}

PresentTarget rasterTarget(const WorkerWindow &window)
{
    PresentTarget t;
    const QSize size = window.physicalSize();
    t.windowWidth = std::uint32_t(size.width());
    t.windowHeight = std::uint32_t(size.height());
    t.scale = window.devicePixelRatio();
    return t;
}

PresentTarget nativeWindowTarget(WorkerWindow &window)
{
    PresentTarget t = rasterTarget(window);
    t.hwnd = reinterpret_cast<void *>(window.winId());
    return t;
}

PresentTarget vulkanTarget(WorkerWindow &window)
{
    PresentTarget t = rasterTarget(window);
    t.vulkanInstanceExtensions = vulkanSurfaceExtensions();
    t.makeVulkanSurface = [&window](void *vkInstance) -> std::uint64_t {
        const auto instance = static_cast<VkInstance>(vkInstance);
        if (!window.vulkanInstance() || window.vulkanInstance()->vkInstance() != instance) {
            auto adopted = std::make_unique<QVulkanInstance>();
            adopted->setVkInstance(instance);  // not owned: the backend destroys it
            if (!adopted->create())
                return 0;
            window.setVulkanInstance(adopted.get());
            window.keepVulkanInstance(std::move(adopted));
        }
        const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(&window);
        std::uint64_t out = 0;
        static_assert(sizeof surface == sizeof out, "VkSurfaceKHR is a 64-bit handle");
        std::memcpy(&out, &surface, sizeof out);
        return out;
    };
    return t;
}

GlContext::GlContext() = default;

GlContext::~GlContext()
{
    if (m_context)
        m_context->doneCurrent();
}

QSurfaceFormat GlContext::format(bool debug)
{
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setProfile(QSurfaceFormat::CoreProfile);
    f.setVersion(3, 3);  // D10: 3.3 core; drivers give at least that
    f.setSwapInterval(0);  // §5.5: vsync off; Qt applies it when the context is made current on the window
    f.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    f.setDepthBufferSize(0);
    f.setStencilBufferSize(0);
    if (debug)
        f.setOption(QSurfaceFormat::DebugContext);
    return f;
}

std::string GlContext::create(bool debug)
{
    m_context = std::make_unique<QOpenGLContext>();
    m_context->setFormat(format(debug));
    m_surface = std::make_unique<QOffscreenSurface>();
    m_surface->setFormat(format(debug));
    m_surface->create();
    const bool made = m_context->create();
    const QSurfaceFormat got = m_context->format();
    if (made && got.profile() == QSurfaceFormat::CoreProfile && (got.majorVersion() > 3 || (got.majorVersion() == 3 && got.minorVersion() >= 3))) {
        if (m_context->makeCurrent(m_surface.get()))
            return {};
        // A 3.3+ context exists but can't be used: an Error (not "needs OpenGL 3.3"), in the driver's own terms.
        m_context.reset();
        return "couldn't make the OpenGL context current";
    }
    // Which version is there, for the reason (§4.2: "Needs OpenGL 3.3 (the driver has 1.1)").
    std::string have = "less";
    QOpenGLContext probe;
    QOffscreenSurface probeSurface;
    probeSurface.create();
    if (probe.create() && probe.makeCurrent(&probeSurface)) {
        if (const auto *v = reinterpret_cast<const char *>(probe.functions()->glGetString(GL_VERSION)))
            have = std::string(v).substr(0, std::string(v).find(' '));
        probe.doneCurrent();
    }
    m_context.reset();
    return "N/A: Needs OpenGL 3.3 (the driver has " + have + ")";
}

PresentTarget GlContext::offscreenTarget(bool headless)
{
    PresentTarget t;
    t.headless = headless;
    makeOffscreenCurrent();
    QOpenGLContext *ctx = m_context.get();
    t.glGetProcAddress = [ctx](const char *name) -> void * { return reinterpret_cast<void *>(ctx->getProcAddress(name)); };
    return t;
}

PresentTarget GlContext::windowTarget(WorkerWindow &window)
{
    PresentTarget t = rasterTarget(window);
    QOpenGLContext *ctx = m_context.get();
    if (!ctx || !ctx->makeCurrent(&window))
        return t;  // no swap: the backend says "the test window has no OpenGL surface"
    // DEF-2: Qt first makes a desktop-GL context current without a surface (its
    // create() and our offscreen surface), so GL_DRAW_BUFFER/GL_READ_BUFFER of the
    // window's framebuffer start as GL_NONE, and every frame drawn there goes nowhere:
    // the Wayland window stayed empty (transparent on NVIDIA), while GLES contexts,
    // which have no draw-buffer state, were fine. Point both at the back buffer.
    // (xcb's GLX context happened to start with GL_BACK; setting it there is a no-op.)
    if (ctx->defaultFramebufferObject() == 0) {
        using Buffer = void (*)(GLenum);
        const auto drawBuffer = reinterpret_cast<Buffer>(ctx->getProcAddress("glDrawBuffer"));
        const auto readBuffer = reinterpret_cast<Buffer>(ctx->getProcAddress("glReadBuffer"));
        if (drawBuffer && readBuffer) {
            ctx->functions()->glBindFramebuffer(GL_FRAMEBUFFER, 0);
            drawBuffer(GL_BACK);
            readBuffer(GL_BACK);
        }
    }
    t.glGetProcAddress = [ctx](const char *name) -> void * { return reinterpret_cast<void *>(ctx->getProcAddress(name)); };
    t.glSwapBuffers = [ctx, &window] { ctx->swapBuffers(&window); };
    t.glDefaultFramebuffer = ctx->defaultFramebufferObject();
    t.glSwapInterval = ctx->format().swapInterval();
    return t;
}

GlInfo GlContext::info()
{
    GlInfo i;
    if (!m_context || !m_surface || !m_context->makeCurrent(m_surface.get()))
        return i;
    const auto text = [&](GLenum name) {
        const auto *s = reinterpret_cast<const char *>(m_context->functions()->glGetString(name));
        return std::string(s ? s : "");
    };
    i.version = text(GL_VERSION);
    i.renderer = text(GL_RENDERER);
    i.vendor = text(GL_VENDOR);
    return i;
}

GlInfo GlContext::defaultInfo()
{
    GlInfo i;
    QOpenGLContext context;
    QOffscreenSurface surface;
    surface.create();
    if (!context.create() || !context.makeCurrent(&surface))
        return i;
    const auto text = [&](GLenum name) {
        const auto *s = reinterpret_cast<const char *>(context.functions()->glGetString(name));
        return std::string(s ? s : "");
    };
    i.version = text(GL_VERSION);
    i.renderer = text(GL_RENDERER);
    i.vendor = text(GL_VENDOR);
    context.doneCurrent();
    return i;
}

void GlContext::makeOffscreenCurrent()
{
    if (m_context && m_surface)
        m_context->makeCurrent(m_surface.get());
}

} // namespace sb::gpu::glue

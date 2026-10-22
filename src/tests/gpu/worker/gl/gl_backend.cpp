// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/gl/gl_backend.h"

#include "tests/gpu/worker/gl/gl_workload_state.h"

#include <glad/gl.h>

#include "tests/gpu/model/drm_display.h"
#include "tests/gpu/model/gl_vendor.h"
#include "tests/gpu/worker/present_constants.h"
#include "tests/gpu/worker/scene/bitmap_font.h"
#include "tests/gpu/worker/scene/textures.h"
#include "tests/gpu/worker/scene_constants.h"
#if defined(_WIN32)
#include "tests/gpu/worker/dxgi/dxgi_swapchain.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace {
#include "tests/gpu/worker/shaders/generated/bindings.inc"
#include "tests/gpu/worker/shaders/generated/present_ps.glsl.inc"
#include "tests/gpu/worker/shaders/generated/present_vs.glsl.inc"
} // namespace

namespace sb::gpu::gl {

namespace {

// The clear-only workload (task 10's): ldrA = (64, 128, 192, 255).
constexpr float kClearColour[4] = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
constexpr GLuint64 kSecondNs = 1000000000ull;
constexpr int kWaitLimitS = 10;

// glad's loader with the glue's getProcAddress (gladLoadGLUserPtr).
GLADapiproc loadProc(void *userptr, const char *name)
{
    const auto &get = *static_cast<const std::function<void *(const char *)> *>(userptr);
    return reinterpret_cast<GLADapiproc>(get(name));
}

void GLAD_API_PTR onDebug(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar *message, const void *user)
{
    static_cast<GlBackend *>(const_cast<void *>(user))->onDebugMessage(type, message);
}

std::string glString(GLenum name)
{
    const auto *s = reinterpret_cast<const char *>(glGetString(name));
    return s ? s : "";
}

// How many non-software GPUs have this PCI vendor (§4.3: with two, OpenGL can't tell which it uses).
int hardwareGpusOfVendor(std::uint32_t vendor)
{
#if defined(_WIN32)
    return dxgi::countHardwareAdapters(vendor);
#else
    int count = 0;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
            continue;
        std::ifstream f(entry.path() / "device" / "vendor");
        std::string text;
        if (f >> text && std::strtoul(text.c_str(), nullptr, 16) == vendor)
            ++count;
    }
    return count;
#endif
}

// The sampler object a SPIRV-Cross combined sampler's name asks for (task 7's
// names: SPIRV_Cross_Combined<texture><sampler>): s0 linearWrap, s1
// linearClamp, s2 shadowCompare; -1 for a Load (DummySampler).
int samplerOf(const std::string &name)
{
    const auto ends = [&](const char *suffix) {
        const std::size_t n = std::strlen(suffix);
        return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
    };
    if (ends("linearWrap"))
        return 0;
    if (ends("linearClamp"))
        return 1;
    if (ends("shadowCompare"))
        return 2;
    return -1;
}

std::string compileStage(GLenum stage, const char *source, GLuint *out)
{
    const GLuint s = glCreateShader(stage);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        glDeleteShader(s);
        return log;
    }
    *out = s;
    return {};
}

} // namespace

GlBackend::GlBackend() = default;

GlBackend::~GlBackend()
{
    shutdown();
}

void GlBackend::onDebugMessage(std::uint32_t type, const char *message)
{
    if (type != GL_DEBUG_TYPE_ERROR)
        return;
    std::fprintf(stderr, "synbench-gpu: opengl: %s\n", message ? message : "(no message)");
    if (m_validationErrors.fetch_add(1) == 0)
        m_firstValidationError = message ? message : "(no message)";
}

std::string GlBackend::validationProblem()
{
    if (m_validationErrors.load() == 0)
        return {};
    return "validation: " + m_firstValidationError;
}

void GlBackend::shutdown()
{
    if (!m_inited)
        return;
    waitAll();
    unload();
    destroyTarget(m_headlessTarget);
    for (Slot &s : m_slots) {
        if (s.queries[0])
            glDeleteQueries(2, s.queries.data());
        if (s.frameConstants)
            glDeleteBuffers(1, &s.frameConstants);
        if (s.objects)
            glDeleteBuffers(1, &s.objects);
        s = Slot{};
    }
    glDeleteSamplers(3, m_samplers.data());
    m_samplers = {};
    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    if (m_atlas)
        glDeleteTextures(1, &m_atlas);
    if (m_present.id)
        glDeleteProgram(m_present.id);
    m_vao = m_atlas = 0;
    m_present = Program{};
    m_inited = false;
}

std::string GlBackend::makeProgram(const char *shader, const char *vs, const char *fs, Program *out)
{
    GLuint v = 0, f = 0;
    if (std::string e = compileStage(GL_VERTEX_SHADER, vs, &v); !e.empty())
        return std::string("the ") + shader + " vertex shader didn't compile: " + e;
    if (std::string e = compileStage(GL_FRAGMENT_SHADER, fs, &f); !e.empty()) {
        glDeleteShader(v);
        return std::string("the ") + shader + " fragment shader didn't compile: " + e;
    }
    Program p;
    p.id = glCreateProgram();
    glAttachShader(p.id, v);
    glAttachShader(p.id, f);
    glLinkProgram(p.id);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
        glDeleteProgram(p.id);
        return std::string("the ") + shader + " program didn't link: " + log;
    }
    // GLSL 330 has no binding= (SH-3): bindings.inc's blocks and samplers, after the link.
    glUseProgram(p.id);
    for (const GlBinding &b : kGlBindings) {
        if (std::strcmp(b.shader, shader) != 0)
            continue;
        if (b.block) {
            const GLuint index = glGetUniformBlockIndex(p.id, b.glslName);
            if (index != GL_INVALID_INDEX)
                glUniformBlockBinding(p.id, index, GLuint(b.unit));
        } else {
            const GLint location = glGetUniformLocation(p.id, b.glslName);
            if (location >= 0)
                glUniform1i(location, b.unit);
            if (b.unit >= 0 && b.unit < 6)
                p.unitSampler[std::size_t(b.unit)] = samplerOf(b.glslName);
        }
    }
    glUseProgram(0);
    *out = p;
    return {};
}

void GlBackend::useProgram(const Program &p)
{
    glUseProgram(p.id);
    for (GLuint unit = 0; unit < 6; ++unit) {
        const int s = p.unitSampler[unit];
        glBindSampler(unit, s >= 0 ? m_samplers[std::size_t(s)] : 0);
    }
}

std::string GlBackend::makeTarget(Target &t, std::uint32_t w, std::uint32_t h, std::uint32_t internalFormat)
{
    destroyTarget(t);
    t.width = w;
    t.height = h;
    glGenTextures(1, &t.texture);
    glBindTexture(GL_TEXTURE_2D, t.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(internalFormat), GLsizei(w), GLsizei(h), 0, GL_RGBA, internalFormat == GL_RGBA16F ? GL_FLOAT : GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        return "an OpenGL render target is incomplete";
    return {};
}

std::string GlBackend::makeDepthTarget(Target &t, std::uint32_t w, std::uint32_t h)
{
    destroyTarget(t);
    t.width = w;
    t.height = h;
    glGenTextures(1, &t.texture);
    glBindTexture(GL_TEXTURE_2D, t.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, GLsizei(w), GLsizei(h), 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return {};
}

std::string GlBackend::makeFbo(std::uint32_t colourTexture, std::uint32_t depthTexture, std::uint32_t *fbo)
{
    GLuint f = 0;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    if (colourTexture)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colourTexture, 0);
    else {
        glDrawBuffer(GL_NONE);  // depth only
        glReadBuffer(GL_NONE);
    }
    if (depthTexture)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTexture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (m_work)
        m_work->fbos.push_back(f);
    *fbo = f;
    if (status != GL_FRAMEBUFFER_COMPLETE)
        return "an OpenGL render target is incomplete";
    return {};
}

void GlBackend::destroyTarget(Target &t)
{
    if (t.fbo)
        glDeleteFramebuffers(1, &t.fbo);
    if (t.texture)
        glDeleteTextures(1, &t.texture);
    t = Target{};
}

std::string GlBackend::init(const BackendInit &init, const PresentTarget &target, BackendInfo *info)
{
    m_init = init;
    m_headless = target.headless;
    if (!target.glGetProcAddress)
        return "no current OpenGL context";
    const int version = gladLoadGLUserPtr(loadProc, const_cast<std::function<void *(const char *)> *>(&target.glGetProcAddress));
    if (!version || GLAD_VERSION_MAJOR(version) * 10 + GLAD_VERSION_MINOR(version) < 33) {
        const std::string have = version && glad_glGetString ? glString(GL_VERSION) : "";
        return "N/A: Needs OpenGL 3.3 (the driver has " + (have.empty() ? std::string("less") : have.substr(0, have.find(' '))) + ")";
    }
    m_inited = true;  // GL 3.3 core: every entry point below exists
    const std::string vendor = glString(GL_VENDOR), renderer = glString(GL_RENDERER);

    // §4.3, D17: the context must be on the chosen GPU. A software renderer
    // counts only with --allow-software; Microsoft's GLon12 is labelled (task 23
    // matches its adapter).
    const bool glOn12 = vendor == "Microsoft Corporation" && renderer.rfind("D3D12 (", 0) == 0;
    if (glSoftwareRenderer(renderer)) {
        if (!init.allowSoftware)
            return "N/A: OpenGL runs on " + renderer + " here, not on this GPU";
    } else if (glOn12) {
        // GLon12: its device (the text inside the parentheses) must be the chosen DXGI adapter.
#if defined(_WIN32)
        const std::string inside = renderer.size() > 8 ? renderer.substr(7, renderer.size() - 8) : std::string();
        const std::string chosen = dxgi::adapterDescription(init, init.allowSoftware);
        if (!glContainsNoCase(chosen, inside) || !glContainsNoCase(inside, chosen))  // equal, ignoring case
            return "N/A: OpenGL runs on " + renderer + " here, not on this GPU";
#endif
    } else {
        const std::uint32_t pci = glVendorPci(vendor);
        if (pci == 0)
            return "N/A: Can't tell which GPU OpenGL uses (" + vendor + ")";
        if (pci != init.vendorId)
            return "N/A: OpenGL runs on " + renderer + " here, not on this GPU";
        if (hardwareGpusOfVendor(pci) > 1)
            return "N/A: Can't tell which GPU OpenGL uses (" + vendor + ")";
#if !defined(_WIN32)
        // Dave's Wayland rule (also decided by the GUI's probe): a GPU that drives no enabled display output
        // can't show its window on this session, so a worker started without a probe entry answers N/A too.
        if (init.waylandSession) {
            const std::optional<bool> drives = drmGpuDrivesDisplay("/", init.pciSlot);
            if (drives.has_value() && !*drives)
                return "N/A: OpenGL's test window can't be shown from this GPU on this Wayland session (it doesn't drive a display)";
        }
#endif
    }

    if (init.validation) {
        if (GLAD_GL_KHR_debug) {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(onDebug, this);
        } else {
            std::fprintf(stderr, "synbench-gpu: opengl: no KHR_debug; running without validation\n");
        }
    }
    GLint alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    if (alignment > GLint(sc::kBlock))
        return "uniform alignment > 256";

    // Revised (Dave, 2026-10-21): anisotropic filtering is required, so that the images match the other APIs (D20).
    if (!GLAD_GL_ARB_texture_filter_anisotropic && !GLAD_GL_EXT_texture_filter_anisotropic)
        return "N/A: OpenGL needs anisotropic filtering (GL_ARB/EXT_texture_filter_anisotropic)";

    // The samplers (§13.1): s0 trilinear wrap with 8x anisotropy, s1 linear clamp, s2 the
    // comparison sampler (LEQUAL, border white).
    glGenSamplers(3, m_samplers.data());
    glSamplerParameteri(m_samplers[0], GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(m_samplers[0], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_samplers[0], GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(m_samplers[0], GL_TEXTURE_WRAP_T, GL_REPEAT);
    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    glSamplerParameterf(m_samplers[0], GL_TEXTURE_MAX_ANISOTROPY, std::min(8.0f, maxAniso));
    glSamplerParameteri(m_samplers[1], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_samplers[1], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_samplers[1], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_samplers[1], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLfloat white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glSamplerParameteri(m_samplers[2], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_samplers[2], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_samplers[2], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glSamplerParameteri(m_samplers[2], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glSamplerParameterfv(m_samplers[2], GL_TEXTURE_BORDER_COLOR, white);
    glSamplerParameteri(m_samplers[2], GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(m_samplers[2], GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    if (std::string e = makeProgram("present", kPresentVsGlsl, kPresentPsGlsl, &m_present); !e.empty())
        return e;
    glGenVertexArrays(1, &m_vao);  // GL-4: core profile draws need a VAO (the full-screen triangle has no attributes)

    // The glyph atlas, once (§5.9): read with texelFetch, row 0 = the CPU's row 0.
    const Texture atlas = glyphAtlas();
    glGenTextures(1, &m_atlas);
    glBindTexture(GL_TEXTURE_2D, m_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(atlas.width), GLsizei(atlas.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.rgba8.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Per slot: the frame constants (a uniform buffer, written after the slot's fence) and 2 timestamps (§5.6).
    GLint timerBits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &timerBits);
    m_timestamps = timerBits > 0;
    m_constants.assign(sc::kFrameConstantsBytes, 0);
    for (Slot &s : m_slots) {
        glGenBuffers(1, &s.frameConstants);
        glBindBuffer(GL_UNIFORM_BUFFER, s.frameConstants);
        glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(sc::kFrameConstantsBytes), nullptr, GL_DYNAMIC_DRAW);
        if (m_timestamps)
            glGenQueries(2, s.queries.data());
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    if (info) {
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        info->deviceName = glOn12 ? renderer.substr(7, renderer.size() > 8 ? renderer.size() - 8 : 0) : renderer;
        info->apiVersion = "OpenGL " + std::to_string(major) + "." + std::to_string(minor);
        info->driver = glString(GL_VERSION);
        info->vendorId = init.vendorId;
        info->deviceId = init.deviceId;
        info->gpuTimestamps = m_timestamps;
        info->note = glOn12 ? "via D3D12 compatibility layer" : "";
        info->presentMode.clear();  // known after resume() (the result's "present")
    }
    return validationProblem();
}

std::string GlBackend::load(const SceneData &scene)
{
    if (m_loaded)
        unload();
    m_work = std::make_unique<WorkloadState>();
    m_work->workload = scene.workload;
    // The render targets (§5.3, D12: load() .. unload()): ldrA always (the clear
    // workload's target, Particles' tone map output); the scenes add theirs.
    if (std::string e = makeTarget(m_ldrA, m_init.renderWidth, m_init.renderHeight); !e.empty())
        return e;
    m_loaded = true;
    m_lastDraws = 0;
    std::string e;
    switch (scene.workload) {
    case Workload::Geometry:
    case Workload::DrawCalls: e = loadScene(scene); break;
    case Workload::Shading: e = loadShading(scene); break;
    case Workload::Particles: e = loadParticles(scene); break;
    case Workload::Quads:
    case Workload::Shapes:
    case Workload::Text:
    case Workload::Blits:
    case Workload::Gradients:
    case Workload::Sprites: break;  // still the clear workload (task 29)
    }
    if (!e.empty())
        return e;
    // The scenes' object constants: one 256-byte slot per instance, a uniform buffer per frame slot,
    // written with glMapBufferRange(INVALIDATE_BUFFER) after the slot's fence and bound per draw
    // with glBindBufferRange (256 i).
    if (!m_work->scene.instances.empty()) {
        for (Slot &s : m_slots) {
            glGenBuffers(1, &s.objects);
            glBindBuffer(GL_UNIFORM_BUFFER, s.objects);
            glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(m_work->scene.instances.size() * sc::kObjectSlot), nullptr, GL_DYNAMIC_DRAW);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
    return validationProblem();
}

void GlBackend::unload()
{
    if (!m_inited)
        return;
    waitAll();
    for (Slot &s : m_slots) {
        s.submitted = false;
        if (s.objects)
            glDeleteBuffers(1, &s.objects);
        s.objects = 0;
    }
    releaseWorkload();
    destroyTarget(m_ldrA);
    m_loaded = false;
    m_lastDraws = 0;
}

std::string GlBackend::suspend()
{
    if (!m_inited)
        return {};
    std::string e = waitAll();
    glFinish();
    for (Slot &s : m_slots)
        s.submitted = false;
    m_swap = nullptr;
    m_resumed = false;
    if (!e.empty())
        return e;
    return validationProblem();
}

std::string GlBackend::resume(const PresentTarget &target)
{
    if (!m_inited)
        return "resume() before init()";
    m_windowScale = target.scale > 0 ? target.scale : 1.0;
    m_presentModeName.clear();
    if (m_headless) {
        if (!m_headlessTarget.fbo || m_headlessTarget.width != m_init.renderWidth || m_headlessTarget.height != m_init.renderHeight) {
            if (std::string e = makeTarget(m_headlessTarget, m_init.renderWidth, m_init.renderHeight); !e.empty())
                return e;
        }
        m_defaultFramebuffer = m_headlessTarget.fbo;
        m_windowW = m_headlessTarget.width;
        m_windowH = m_headlessTarget.height;
    } else {
        if (!target.glSwapBuffers)
            return "the test window has no OpenGL surface";
        // Revised (Dave): the glue made the context current on the re-shown window,
        // re-applied swap interval 0 and re-read the default framebuffer.
        m_swap = target.glSwapBuffers;
        m_defaultFramebuffer = target.glDefaultFramebuffer;
        m_windowW = target.windowWidth;
        m_windowH = target.windowHeight;
        // §5.5: the interval Qt reports (0: vsync off).
        m_presentModeName = "swap interval " + std::to_string(target.glSwapInterval);
    }
    m_resumed = true;
    return validationProblem();
}

std::string GlBackend::waitSlot(std::uint32_t index, std::vector<double> *gpuMs)
{
    Slot &s = m_slots[index];
    if (!s.submitted)
        return {};
    // GL-5: 1 s steps with GL_SYNC_FLUSH_COMMANDS_BIT, never forever.
    const auto sync = static_cast<GLsync>(s.fence);
    bool done = false;
    for (int i = 0; i < kWaitLimitS && !done; ++i) {
        const GLenum r = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, kSecondNs);
        if (r == GL_WAIT_FAILED) {
            glDeleteSync(sync);
            s.fence = nullptr;
            s.submitted = false;
            return "glClientWaitSync failed";
        }
        done = r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
    }
    glDeleteSync(sync);
    s.fence = nullptr;
    s.submitted = false;
    if (!done)
        return "LOST: the GPU stopped responding";
    // §5.6: the results only when available (they are, after the fence).
    if (gpuMs && m_timestamps) {
        GLint available = 0;
        glGetQueryObjectiv(s.queries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            GLuint64 begin = 0, end = 0;
            glGetQueryObjectui64v(s.queries[0], GL_QUERY_RESULT, &begin);
            glGetQueryObjectui64v(s.queries[1], GL_QUERY_RESULT, &end);
            if (end >= begin)
                gpuMs->push_back(double(end - begin) / 1e6);
        }
    }
    return {};
}

std::string GlBackend::waitAll()
{
    for (std::uint32_t k = 0; k < m_slots.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_slots.size()), nullptr); !e.empty())
            return e;
    }
    return {};
}

void GlBackend::uploadConstants(Slot &slot)
{
    // The CPU copy (Frame, Present, Lights, the pass blocks) after the slot's fence: the first 8 KiB.
    glBindBuffer(GL_UNIFORM_BUFFER, slot.frameConstants);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(sc::blockOffset(sc::kPostPasses)), m_constants.data());
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GlBackend::bindFrameConstants(Slot &slot)
{
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, slot.frameConstants, 0, GLsizeiptr(sc::kBlock));
}

void GlBackend::bindPassConstants(Slot &slot, std::size_t offset, std::size_t bytes)
{
    glBindBufferRange(GL_UNIFORM_BUFFER, 2, slot.frameConstants, GLintptr(offset), GLsizeiptr(bytes));
}

void GlBackend::recordWorkload(Slot &slot, double t)
{
    switch (m_work ? m_work->workload : Workload::Quads) {
    case Workload::Geometry:
    case Workload::DrawCalls: recordScene(slot, t); return;
    case Workload::Shading: recordShading(slot, t); return;
    case Workload::Particles: recordParticles(slot, t); return;
    case Workload::Quads:
    case Workload::Shapes:
    case Workload::Text:
    case Workload::Blits:
    case Workload::Gradients:
    case Workload::Sprites: break;
    }
    uploadConstants(slot);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ldrA.fbo);
    glViewport(0, 0, GLsizei(m_ldrA.width), GLsizei(m_ldrA.height));
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(kClearColour[0], kClearColour[1], kClearColour[2], kClearColour[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    m_lastDraws = 0;  // the clear workload draws nothing before the present pass
}

void GlBackend::presentPass(Slot &slot, std::uint32_t fbo, std::uint32_t w, std::uint32_t h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, GLsizei(w), GLsizei(h));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    useProgram(m_present);
    bindFrameConstants(slot);
    bindPassConstants(slot, sc::kPresentOffset, sc::kBlock);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, outputTexture());
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_atlas);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

std::string GlBackend::frame(double t, std::vector<double> *gpuMs)
{
    if (!m_loaded || !m_resumed)
        return "frame() before load() and resume()";
    const std::uint32_t si = std::uint32_t(m_frame % m_slots.size());
    Slot &s = m_slots[si];
    // §5.4: frame n waits for frame n-2's fence before it reuses the slot (its constants, its queries).
    if (std::string e = waitSlot(si, gpuMs); !e.empty())
        return e;
    // GL-1: flipV = 1; the overlay box from the bottom (task 7). The workload writes the rest and uploads the lot.
    const PresentConstants pc =
        presentConstants(tonemapped() ? kPresentTonemap : kPresentCopy, m_overlay, m_windowScale, m_windowW, m_windowH, true);
    std::memcpy(m_constants.data() + sc::kPresentOffset, &pc, sizeof pc);
    if (m_timestamps)
        glQueryCounter(s.queries[0], GL_TIMESTAMP);
    recordWorkload(s, t);
    presentPass(s, m_defaultFramebuffer, m_windowW, m_windowH);
    if (m_timestamps)
        glQueryCounter(s.queries[1], GL_TIMESTAMP);
    s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s.submitted = true;
    ++m_frame;
    if (m_swap)
        m_swap();
    else
        glFlush();
    return validationProblem();
}

std::string GlBackend::drain(std::vector<double> *gpuMs)
{
    // The oldest pending frame first, so gpuMs stays in submission order.
    for (std::uint32_t k = 0; k < m_slots.size(); ++k) {
        if (std::string e = waitSlot(std::uint32_t((m_frame + k) % m_slots.size()), gpuMs); !e.empty())
            return e;
    }
    return validationProblem();
}

std::string GlBackend::readTarget(const Target &t, std::vector<std::uint8_t> *rgba8)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<std::uint8_t> rows(std::size_t(t.width) * t.height * 4);
    glReadPixels(0, 0, GLsizei(t.width), GLsizei(t.height), GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    // glReadPixels' rows are bottom-up: flip, so row 0 is the top as on every API.
    const std::size_t pitch = std::size_t(t.width) * 4;
    rgba8->resize(rows.size());
    for (std::uint32_t y = 0; y < t.height; ++y)
        std::memcpy(rgba8->data() + std::size_t(y) * pitch, rows.data() + std::size_t(t.height - 1 - y) * pitch, pitch);
    return validationProblem();
}

std::string GlBackend::readback(double t, std::vector<std::uint8_t> *rgba8)
{
    if (!m_loaded)
        return "readback() before load()";
    if (std::string e = waitAll(); !e.empty())
        return e;
    // §5.7: the workload's output before the present pass, exactly render size.
    Slot &s = m_slots[0];
    if (!tonemapped()) {
        recordWorkload(s, t);
        return readTarget(outputTarget(), rgba8);
    }
    // A tone mapped scene (sceneColor is RGBA16F, linear): the present pass in mode 0,
    // no overlay, into an RGBA8 target at render size (task 11's rule), flipV as ever.
    const Target &target = m_work->readTarget;
    const PresentConstants pc = presentConstants(kPresentTonemap, std::string(), 1.0, target.width, target.height, true);
    std::memcpy(m_constants.data() + sc::kPresentOffset, &pc, sizeof pc);
    recordWorkload(s, t);
    presentPass(s, target.fbo, target.width, target.height);
    return readTarget(target, rgba8);
}

#if defined(SYNBENCH_DEV_HOOKS)
std::string GlBackend::readbackPresented(std::vector<std::uint8_t> *rgba8)
{
    if (!m_headless || !m_resumed || m_frame == 0)
        return "readbackPresented() needs a headless frame";
    if (std::string e = waitAll(); !e.empty())
        return e;
    return readTarget(m_headlessTarget, rgba8);
}
#endif

} // namespace sb::gpu::gl

namespace sb::gpu {
std::unique_ptr<IBackend> makeOpenGlBackend()
{
    return std::make_unique<gl::GlBackend>();
}
} // namespace sb::gpu

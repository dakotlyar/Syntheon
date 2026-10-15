// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/common/opengl_probe.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QThread>

namespace sb::sysinfo {

OpenGlInfo probeOpenGl()
{
    OpenGlInfo info;
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())
        || QThread::currentThread() != QCoreApplication::instance()->thread()) {
        info.error = QStringLiteral("needs the GUI thread");
        return info;
    }

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setVersion(4, 6);  // drivers return the newest they support

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        context.setFormat(QSurfaceFormat::defaultFormat());  // e.g. GLES-only
        if (!context.create()) {
            info.error = QStringLiteral("couldn't create an OpenGL context");
            return info;
        }
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!context.makeCurrent(&surface)) {
        info.error = QStringLiteral("couldn't make the OpenGL context current");
        return info;
    }
    QOpenGLFunctions *gl = context.functions();
    const auto str = [gl](GLenum name) {
        return QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(name)));
    };
    info.version = str(GL_VERSION);
    info.renderer = str(GL_RENDERER);
    info.vendor = str(GL_VENDOR);
    info.isEs = context.isOpenGLES();
    info.ok = !info.version.isEmpty();
    context.doneCurrent();
    return info;
}

} // namespace sb::sysinfo

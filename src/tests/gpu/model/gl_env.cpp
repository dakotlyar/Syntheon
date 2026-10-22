// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/model/gl_env.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace sb::gpu {

namespace {

const QString kMesaJson = QStringLiteral("/usr/share/glvnd/egl_vendor.d/50_mesa.json");
const QString kNvidiaJson = QStringLiteral("/usr/share/glvnd/egl_vendor.d/10_nvidia.json");

bool isMesaDriver(const GpuEntry &gpu)
{
    for (const char *kernel : {"amdgpu", "i915", "xe", "nouveau", "msm", "panfrost"}) {
        if (gpu.kernelDriver == QLatin1StringView(kernel))
            return true;
    }
    return gpu.apis.value(QStringLiteral("vulkan")).driver.contains(QLatin1StringView("Mesa"));
}

// "OpenGL 4.6" or "4.6.0 NVIDIA ..." -> (4, 6); (0, 0) when there is none.
std::pair<int, int> glVersionOf(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("(\\d+)\\.(\\d+)"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? std::pair<int, int>{m.captured(1).toInt(), m.captured(2).toInt()} : std::pair<int, int>{0, 0};
}

} // namespace

bool defaultFileExists(const QString &path)
{
    return QFileInfo::exists(path);
}

QString driPrimeValue(const QString &pciSlot)
{
    QString v = pciSlot;
    v.replace(QLatin1Char(':'), QLatin1Char('_'));
    v.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("pci-") + v;
}

QProcessEnvironment glEnvironmentFor(const GpuEntry &gpu, bool windows, const std::function<bool(const QString &)> &fileExists)
{
    QProcessEnvironment env;
    if (windows)
        return env;
    if (gpu.kernelDriver == QLatin1StringView("nvidia")) {
        env.insert(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1"));
        env.insert(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("nvidia"));
        if (fileExists(kNvidiaJson))
            env.insert(QStringLiteral("__EGL_VENDOR_LIBRARY_FILENAMES"), kNvidiaJson);
    } else if (isMesaDriver(gpu)) {
        if (!gpu.pciSlot.isEmpty())
            env.insert(QStringLiteral("DRI_PRIME"), driPrimeValue(gpu.pciSlot));
        env.insert(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("mesa"));
        if (fileExists(kMesaJson))
            env.insert(QStringLiteral("__EGL_VENDOR_LIBRARY_FILENAMES"), kMesaJson);
    }
    return env;
}

QProcessEnvironment glEnvironment(const GpuEntry &gpu, const std::function<bool(const QString &)> &fileExists)
{
#if defined(Q_OS_WIN)
    return glEnvironmentFor(gpu, true, fileExists);
#else
    return glEnvironmentFor(gpu, false, fileExists);
#endif
}

ApiAvailability verifyGl(const protocol::GlEvent &gl, const GpuEntry &chosen, const QVector<GpuEntry> &all, bool linuxArm, bool windowsArm)
{
    ApiAvailability a;
    if (linuxArm) {
        a.reason = QStringLiteral("OpenGL isn't tested on Linux ARM");
        return a;
    }
    // Windows on ARM without Microsoft's compatibility pack: the GDI software renderer is all there is.
    if (windowsArm && gl.renderer.contains(QLatin1StringView("GDI Generic"))) {
        a.reason = QStringLiteral("OpenGL needs Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack");
        a.hint = QStringLiteral("https://apps.microsoft.com/detail/9nqpsl29bfff");
        return a;
    }
    const auto [major, minor] = glVersionOf(gl.version);
    if (major == 0) {
        a.reason = !gl.ok && !gl.reason.isEmpty() ? gl.reason : QStringLiteral("the OpenGL probe failed");
        return a;
    }
    if (!gl.ok || major < 3 || (major == 3 && minor < 3)) {
        if (!gl.ok && (major > 3 || (major == 3 && minor >= 3)) && !gl.reason.isEmpty())
            a.reason = gl.reason;  // a 3.3+ driver that still gave no 3.3 core context: its own words
        else
            a.reason = QStringLiteral("Needs OpenGL 3.3 (the driver has %1.%2)").arg(major).arg(minor);
        return a;
    }
    const QString notThisGpu = QStringLiteral("OpenGL runs on %1 here, not on this GPU").arg(gl.renderer);
    if (isSoftwareGl(gl.renderer)) {
        if (chosen.kind != GpuKind::Software) {
            a.reason = notThisGpu;
            return a;
        }
    } else if (isGlOn12(gl.vendor, gl.renderer)) {
        // Microsoft's GLon12: tested, the device is the text inside the parentheses (the DXGI adapter's description).
        if (glOn12DeviceName(gl.renderer).compare(chosen.name, Qt::CaseInsensitive) != 0) {
            a.reason = notThisGpu;
            return a;
        }
        a.note = QStringLiteral("via D3D12 compatibility layer");
    } else {
        const std::optional<quint32> pci = glVendorToPci(gl.vendor);
        if (!pci) {
            a.reason = QStringLiteral("Can't tell which GPU OpenGL uses (%1)").arg(gl.vendor);
            return a;
        }
        if (*pci != chosen.vendorId) {
            a.reason = notThisGpu;
            return a;
        }
        int sameVendor = 0;
        for (const GpuEntry &g : all)
            sameVendor += (g.kind != GpuKind::Software && g.vendorId == *pci) ? 1 : 0;
        if (sameVendor > 1) {
            a.reason = QStringLiteral("Can't tell which GPU OpenGL uses (%1)").arg(gl.vendor);
            return a;
        }
    }
    a.ok = true;
    a.version = QStringLiteral("OpenGL %1.%2").arg(major).arg(minor);
    a.driver = gl.renderer;
    return a;
}

} // namespace sb::gpu

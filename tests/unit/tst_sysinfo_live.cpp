// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo/collector.h"
#include "sysinfo/common/opengl_probe.h"
#include "sysinfo/common/vulkan_probe.h"
#include "sysinfo/common/x86_cpu.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTest>

using namespace sb;
using namespace sb::sysinfo;

// The real collector on the machine running the tests (a developer box or a
// CI runner). Only checks what every machine has; prints the snapshot.
class TestSysinfoLive : public QObject
{
    Q_OBJECT

private slots:
    void collect()
    {
        QElapsedTimer t;
        t.start();
        const SystemInfo info = collectSystemInfo();
        qInfo("collectSystemInfo took %lld ms", t.elapsed());
        QVERIFY(!info.machine.hostname.isEmpty());
        QVERIFY(info.cpu.threads > 0);
        QVERIFY(info.cpu.cores > 0);
        QVERIFY(!info.cpu.name.isEmpty());
        QVERIFY(info.memory.totalBytes > 0);
        QVERIFY(!info.os.name.isEmpty());
        QVERIFY(!info.os.arch.isEmpty());
        QVERIFY(!info.volumes.isEmpty());
        QVERIFY(!info.app.qtVersion.isEmpty());
#if defined(Q_PROCESSOR_X86_64)
        QVERIFY(info.cpu.features.contains(QStringLiteral("sse2")));
        QVERIFY(info.cpu.features.contains(QStringLiteral("sse4.2")));  // x86-64-v2 baseline (D11)
#endif
        const QByteArray json = QJsonDocument(info.toJson()).toJson();
        QVERIFY(!json.contains("serial"));
        QVERIFY(!json.contains("uuid"));
        qInfo("%s", json.constData());
    }

    void x86CpuidMatchesItself()
    {
#if defined(Q_PROCESSOR_X86_64)
        const X86Cpu c = queryX86Cpu();
        QVERIFY(!c.vendor.isEmpty());
        QVERIFY(!c.brand.isEmpty());
        QCOMPARE(c.features.first(), QStringLiteral("sse2"));
#else
        QVERIFY(queryX86Cpu().features.isEmpty());
#endif
    }

    void vulkan()
    {
        const VulkanInfo vk = probeVulkan();
        if (vk.instanceVersion.isEmpty())
            QSKIP(qPrintable(QStringLiteral("no Vulkan here: %1").arg(vk.error)));
        QVERIFY(vk.instanceVersion.startsWith(QLatin1StringView("1.")));
        for (const VulkanDevice &d : vk.devices) {
            qInfo("Vulkan device: %s (%s) api %s driver %s %s", qPrintable(d.name), qPrintable(d.type),
                  qPrintable(d.apiVersion), qPrintable(d.driverName), qPrintable(d.driverInfo));
            QVERIFY(!d.name.isEmpty());
        }
    }

    void openGl()
    {
        const OpenGlInfo gl = probeOpenGl();
        if (!gl.ok)
            QSKIP(qPrintable(QStringLiteral("no OpenGL context here: %1").arg(gl.error)));
        qInfo("OpenGL %s on %s", qPrintable(gl.version), qPrintable(gl.renderer));
        QVERIFY(!gl.renderer.isEmpty());
    }
};

QTEST_MAIN(TestSysinfoLive)
#include "tst_sysinfo_live.moc"

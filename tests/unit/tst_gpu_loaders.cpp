// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU test's third-party loaders (Phase 4a task 6): volk starts empty and
// loads the Vulkan loader at run time (a machine without one is fine); glad's
// header compiles. No window and no GPU work.
#include <glad/gl.h>
#include <volk.h>

#include <QTest>

class TestGpuLoaders : public QObject
{
    Q_OBJECT

private slots:
    void volkStartsEmptyThenLoadsTheLoader()
    {
        QCOMPARE(volkGetInstanceVersion(), 0u);  // nothing loaded yet
        const VkResult r = volkInitialize();
        QVERIFY2(r == VK_SUCCESS || r == VK_ERROR_INITIALIZATION_FAILED, "volkInitialize returned something else");
        qInfo("volkInitialize: %s", r == VK_SUCCESS ? "VK_SUCCESS (a Vulkan loader is installed)"
                                                    : "VK_ERROR_INITIALIZATION_FAILED (no Vulkan loader here)");
        if (r == VK_SUCCESS)
            QVERIFY(volkGetInstanceVersion() >= VK_API_VERSION_1_0);
    }

    void gladHeaderCompiles() { QCOMPARE(GLAD_VERSION_MAJOR(0), 0); }
};

QTEST_GUILESS_MAIN(TestGpuLoaders)
#include "tst_gpu_loaders.moc"

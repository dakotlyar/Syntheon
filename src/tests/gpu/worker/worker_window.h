// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GPU test's window (Phase 4a design §3.3): a fixed-size QWindow of the
// planned logical size, centred on its screen, on top where the platform
// allows. Esc or closing it stops the test; windowOk() says whether it is still
// exposed, the same size and on the same screen at the same scale as at its
// first expose since it was last shown.

#include "tests/gpu/model/render_size.h"

#include <QPointer>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QWindow>

#include <memory>

class QVulkanInstance;

class WorkerWindow : public QWindow
{
public:
    WorkerWindow(QSurface::SurfaceType type, const sb::gpu::WindowPlan &plan, QScreen *screen);
    // Destroys the platform window (and with it a Vulkan surface) before the
    // QVulkanInstance it was made with.
    ~WorkerWindow() override;
    bool stopRequested() const;   // Esc or close
    bool windowOk() const;        // exposed, and size/screen/scale unchanged since the first expose
    void setWorkloadTitle(const QString &api, const QString &workload);  // "SynBench GPU test · Vulkan · Geometry"

    // Task 9: whether it has been exposed since the last show(), and why
    // windowOk() is false (§3.2, §3.3 reasons; "" while it is ok).
    bool exposedSinceShow() const { return m_armed; }
    QString problem() const;
    QSize physicalSize() const;   // lround(logical x devicePixelRatio)
    // D1: the compositor chooses the screen (Wayland ignores setPosition), so after the first
    // expose the worker re-plans for the screen it really got: a new fixed size, and a new
    // baseline once the window has settled (rebase()).
    void applyPlan(const sb::gpu::WindowPlan &plan);
    void rebase();  // take the baseline now, if exposed
    // Task 10 (qt_glue::vulkanTarget): the QVulkanInstance that adopted the
    // backend's VkInstance lives as long as this window.
    void keepVulkanInstance(std::unique_ptr<QVulkanInstance> instance);

protected:
    void keyPressEvent(QKeyEvent *) override; bool event(QEvent *) override; void exposeEvent(QExposeEvent *) override;

private:
    bool m_stop = false;
    bool m_armed = false;  // the baseline below is set (the first expose since show())
    QSize m_size;
    QPointer<QScreen> m_screen;
    qreal m_dpr = 0;
    std::unique_ptr<QVulkanInstance> m_vulkan;
};

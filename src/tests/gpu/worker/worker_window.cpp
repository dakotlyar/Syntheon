// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/worker_window.h"

#include <QExposeEvent>
#include <QGuiApplication>
#include <QKeyEvent>

#include <cmath>

WorkerWindow::WorkerWindow(QSurface::SurfaceType type, const sb::gpu::WindowPlan &plan, QScreen *screen)
{
    setSurfaceType(type);
    if (screen)
        setScreen(screen);
    setFlag(Qt::WindowStaysOnTopHint);
    const QSize size(plan.windowLogical.width, plan.windowLogical.height);
    setMinimumSize(size);
    setMaximumSize(size);
    resize(size);
    if (screen) {
        const QRect avail = screen->availableGeometry();
        setPosition(avail.x() + (avail.width() - size.width()) / 2, avail.y() + (avail.height() - size.height()) / 2);
    }
    setTitle(QStringLiteral("SynBench GPU test"));
}

bool WorkerWindow::stopRequested() const
{
    return m_stop;
}

QSize WorkerWindow::physicalSize() const
{
    const qreal dpr = devicePixelRatio();
    return {int(std::lround(width() * dpr)), int(std::lround(height() * dpr))};
}

bool WorkerWindow::windowOk() const
{
    return problem().isEmpty();
}

QString WorkerWindow::problem() const
{
    if (!m_armed || !isExposed() || windowStates().testFlag(Qt::WindowMinimized))
        return QStringLiteral("the test window was hidden or minimised during the measurement");
    if (physicalSize() != m_size || screen() != m_screen || devicePixelRatio() != m_dpr)
        return QStringLiteral("the test window was resized or moved to another screen during the measurement");
    return {};
}

void WorkerWindow::setWorkloadTitle(const QString &api, const QString &workload)
{
    setTitle(QStringLiteral("SynBench GPU test · %1 · %2").arg(api, workload));
}

void WorkerWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        m_stop = true;
        e->accept();
        return;
    }
    QWindow::keyPressEvent(e);
}

bool WorkerWindow::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::Close:
        // Closing is a stop (§2.2); the worker hides and exits itself.
        m_stop = true;
        e->ignore();
        return true;
    case QEvent::Show:
    case QEvent::Hide:
        m_armed = false;  // a new baseline at the next expose
        break;
    default:
        break;
    }
    return QWindow::event(e);
}

void WorkerWindow::exposeEvent(QExposeEvent *e)
{
    if (isExposed() && !m_armed) {
        m_armed = true;
        m_size = physicalSize();
        m_screen = screen();
        m_dpr = devicePixelRatio();
        raise();
        requestActivate();
    }
    QWindow::exposeEvent(e);
}

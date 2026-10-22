// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4a task 24 (D20, §5.7): every API draws the four scenes like every other API. The
// worker's real backends on software drivers, headless at 160x90 and content ÷ 64, then the
// GPU test's own verdict (judgeSignatures) over the live signatures: Linux, Vulkan on lavapipe
// against OpenGL on llvmpipe (the worker under its own xvfb-run, EGL/GLX pinned to Mesa);
// Windows, DX12 against DX11 on WARP. A mismatch fails with the per-scene mean and max: it
// means one backend draws something different, so the tolerance is never widened here.
#include "gpu_worker_process.h"

#include "tests/gpu/model/best_of.h"

#include <QStandardPaths>
#include <QTest>

using namespace sb::gpu;
using namespace sb::gpu::testing;

namespace {

const QString kWorker = QStringLiteral(SB_GPU_WORKER);
const QStringList kScenes = {QStringLiteral("geometry"), QStringLiteral("drawcalls"), QStringLiteral("shading"), QStringLiteral("particles")};
// Task 31 (§5.7, the fill workloads): at content scale 1, i.e. the full 128 alpha-blended layers and 100,000
// sprites (25 batches, so GL-3's re-pointed attributes are really exercised).
const QStringList kFills = {QStringLiteral("quads"), QStringLiteral("sprites")};

// One API's signatures of `scenes`, in order; empty with *why on a failure ("skip:" starts a skip).
QVector<QByteArray> run(const QString &api, const QStringList &scenes, const QString &contentScale, QString *why)
{
    QStringList plan;
    for (const QString &scene : scenes)
        plan << scene + QStringLiteral(":200:300");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString program = kWorker;
    QStringList args;
    QString gpu;
#if defined(Q_OS_WIN)
    gpu = kWarpId;
#else
    if (api == QLatin1StringView("opengl")) {
        // Headless GL needs an X display for Qt's offscreen GLX surface: the worker's own xvfb-run.
        program = QStandardPaths::findExecutable(QStringLiteral("xvfb-run"));
        if (program.isEmpty()) {
            *why = QStringLiteral("skip:xvfb-run isn't installed");
            return {};
        }
        args = {QStringLiteral("-a"), QStringLiteral("-s"), QStringLiteral("-screen 0 1920x1080x24"), kWorker};
        env.remove(QStringLiteral("WAYLAND_DISPLAY"));
        addSoftwareGl(env);  // llvmpipe through Mesa's GLX and EGL only (M4)
    } else {
        if (lavapipeIcd().isEmpty()) {
            *why = QStringLiteral("skip:lavapipe (the software Vulkan driver) isn't installed");
            return {};
        }
        addSoftwareVulkan(env);
    }
    gpu = kLavapipeId;
#endif
    WorkerProcess w(program,
                    args + QStringList{QStringLiteral("--run"), QStringLiteral("--api"), api, QStringLiteral("--gpu"), gpu, QStringLiteral("--allow-software"),
                                       QStringLiteral("--render-size"), QStringLiteral("160x90"), QStringLiteral("--content-scale"), contentScale,
                                       QStringLiteral("--headless"), QStringLiteral("--plan"),
                                       plan.join(QLatin1Char(','))},
                    env);
    if (!w.started()) {
        *why = QStringLiteral("the worker didn't start");
        return {};
    }
    const int code = w.finish(120000);
    if (code == 5) {
        *why = QStringLiteral("skip:%1 isn't available here: %2").arg(api, errors(w.events()));
        return {};
    }
    if (code != 0) {
        *why = QStringLiteral("%1: exit %2: %3 %4").arg(api).arg(code).arg(errors(w.events()), w.stderrText());
        return {};
    }
    QVector<QByteArray> out;
    const QList<pr::ResultEvent> rs = results(w.events());
    for (const QString &scene : scenes) {
        QByteArray sig;
        for (const pr::ResultEvent &r : rs) {
            if (r.workload == scene)
                sig = r.signature;
        }
        out.append(sig);
    }
    if (out.size() != scenes.size() || out.contains(QByteArray())) {
        *why = QStringLiteral("%1: not every scene sent a signature").arg(api);
        return {};
    }
    return out;
}

} // namespace

class TestGpuSignature : public QObject
{
    Q_OBJECT

private slots:
    void sameImageOnEveryApi() { compare(kScenes, QStringLiteral("64")); }
    void fillsOnEveryApi() { compare(kFills, QStringLiteral("1")); }

private:
    void compare(const QStringList &scenes, const QString &contentScale)
    {
#if defined(Q_OS_WIN)
        const QStringList apis = {QStringLiteral("dx12"), QStringLiteral("dx11")};
#else
        const QStringList apis = {QStringLiteral("vulkan"), QStringLiteral("opengl")};
#endif
        QVector<QVector<QByteArray>> sigs;
        for (const QString &api : apis) {
            QString why;
            const QVector<QByteArray> s = run(api, scenes, contentScale, &why);
            if (s.isEmpty()) {
                if (why.startsWith(QLatin1StringView("skip:"))) {
#if defined(Q_OS_LINUX)
                    SB_SKIP_OR_FAIL(qPrintable(why.mid(5)));
#else
                    QSKIP(qPrintable(why.mid(5)));
#endif
                }
                QFAIL(qPrintable(why));
            }
            sigs.append(s);
        }
        QStringList report;
        bool allMatch = true;
        for (int scene = 0; scene < scenes.size(); ++scene) {
            QVector<QByteArray> inRunOrder;
            for (const auto &s : std::as_const(sigs))
                inRunOrder.append(s[scene]);
            const SignatureVerdict verdict = judgeSignatures(inRunOrder);
            QVERIFY(verdict.reference >= 0);
            for (int i = 0; i < inRunOrder.size(); ++i) {
                if (i == verdict.reference)
                    continue;
                const SigDiff &d = verdict.diffToReference[i];
                report << QStringLiteral("%1 %2 vs %3: mean %4/255, max %5/255")
                              .arg(scenes[scene], apis[i], apis[verdict.reference]).arg(d.mean, 0, 'f', 2).arg(d.max);
            }
            allMatch = allMatch && verdict.outliers.isEmpty();
        }
        qInfo("%s", qPrintable(report.join(QStringLiteral("; "))));
        QVERIFY2(allMatch, qPrintable(report.join(QStringLiteral("; "))));
    }
};

QTEST_GUILESS_MAIN(TestGpuSignature)
#include "tst_gpu_signature.moc"

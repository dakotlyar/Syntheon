// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/controllers/app_controller.h"

#include <QVariantMap>

#include "sysinfo/collector.h"

#include "synbench_version.h"

#include <QCoreApplication>
#include <QFile>
#include <QLoggingCategory>
#include <QThreadPool>

#include <cstdio>

#ifdef Q_OS_WIN
#include "privileged/win_inproc/windows_inprocess_ops.h"
#endif

Q_LOGGING_CATEGORY(lcApp, "synbench.app")

namespace sb {

#ifdef Q_OS_LINUX
LinuxHelperClient::Options AppController::defaultHelperOptions()
{
    LinuxHelperClient::Options o;
    o.expectedBuildId = LinuxHelperClient::thisBuildId();
    o.requireRoot = true;
    return o;
}

AppController::AppController(LaunchProvider launch, LinuxHelperClient::Options options, QObject *parent)
    : QObject(parent), m_launch(std::move(launch)), m_helper(std::move(options))
{
    connect(&m_helper, &LinuxHelperClient::ready, this, [this] {
        setAuth(AuthState::Ready);
        readSmbiosWhenReady();
    });
    connect(&m_helper, &LinuxHelperClient::failed, this,
            [this](LinuxHelperClient::Failure, const QString &message) {
                m_smbiosRequested = false;  // try again after a successful Retry
                setAuth(AuthState::Failed, message);
                applyUdevFallback();
            });
}
#else
AppController::AppController(QObject *parent)
    : QObject(parent), m_ops(std::make_unique<WindowsInProcessOps>())
{
}
#endif

AppController::~AppController()
{
#ifdef Q_OS_LINUX
    // AP5-R2-6: a pool task may be blocked in a privileged request (up to the
    // 15 s timeout). Closing the helper fails every pending request at once,
    // so the wait below ends promptly instead of freezing the window.
    m_helper.shutdown();
#endif
    // Pool tasks capture `this`; let them finish first.
    QThreadPool::globalInstance()->waitForDone();
}

void AppController::abortPrivileged()
{
#ifdef Q_OS_LINUX
    m_helper.shutdown();
#endif
}

PrivilegedOps &AppController::privileged()
{
    if (m_noElevation)
        return m_unavailable;
#ifdef Q_OS_LINUX
    return m_helper;
#else
    return *m_ops;
#endif
}

QString AppController::authStateName() const
{
    switch (m_authState) {
    case AuthState::Authorising: return QStringLiteral("authorising");
    case AuthState::Ready: return QStringLiteral("ready");
    case AuthState::Failed: return QStringLiteral("failed");
    }
    return {};
}

void AppController::setAuth(AuthState state, const QString &message)
{
    m_authState = state;
    m_authMessage = message;
    emit authChanged();
}

void AppController::setSection(const QString &section)
{
    if (section == m_section)
        return;
    m_section = section;
    emit sectionChanged();
}

QString AppController::headerMachine() const
{
    return m_infoReady ? m_info.machineLabel() : QString();
}

QString AppController::headerDetail() const
{
    if (!m_infoReady)
        return {};
    QStringList parts;
    parts << QStringLiteral("%1 %2").arg(m_info.os.name, m_info.os.arch).trimmed();
    if (m_info.battery.present)
        parts << (m_info.battery.acOnline ? QStringLiteral("on AC power")
                                          : QStringLiteral("on battery · %1%").arg(qRound(std::max(0.0, m_info.battery.chargePct))));
    else
        parts << QStringLiteral("no battery");
    return parts.join(QStringLiteral(" · "));
}

QString AppController::emulationText() const
{
    if (!m_info.emulated)
        return {};
    return QStringLiteral("This is the x64 build running emulated on an ARM64 PC, so results are not native. "
                          "Install the ARM64 build for real numbers.");
}

QString AppController::thirdPartyNotices() const
{
    QFile f(QStringLiteral(":/synbench/THIRD-PARTY-NOTICES.txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

QString AppController::version() const
{
    return m_info.app.version.isEmpty() ? QStringLiteral(SYNBENCH_DISPLAY_VERSION) : m_info.app.version;
}

void AppController::refreshPower()
{
    if (m_powerBusy || !m_infoReady)
        return;
    m_powerBusy = true;
    QThreadPool::globalInstance()->start([this] {
        const BatteryInfo b = sysinfo::readBatteryNow();
        QMetaObject::invokeMethod(this, [this, b] {
            m_powerBusy = false;
            applyPower(b);
        });
    });
}

void AppController::applyPower(BatteryInfo b)
{
#ifdef SYNBENCH_DEV_HOOKS
    if (m_powerForTest)
        b = *m_powerForTest;
#endif
    const bool changed = b.present != m_info.battery.present || b.acOnline != m_info.battery.acOnline
                         || qRound(b.chargePct) != qRound(m_info.battery.chargePct);
    // Keep the one-off fields (design capacity, health, cycles) from the full snapshot.
    m_info.battery.present = b.present;
    m_info.battery.acOnline = b.acOnline;
    m_info.battery.chargePct = b.chargePct;
    if (changed)
        emit powerChanged();
}

#ifdef SYNBENCH_DEV_HOOKS
void AppController::setPowerForTest(std::optional<BatteryInfo> power)
{
    m_powerForTest = std::move(power);
    if (!m_infoReady)
        return;  // the start-up snapshot applies it
    if (m_powerForTest)
        applyPower(*m_powerForTest);
    else
        refreshPower();  // back to the real readings now, not at the next poll
}
#endif

void AppController::startHelper()
{
#ifdef Q_OS_LINUX
    setAuth(AuthState::Authorising);
    if (m_launch)
        m_helper.start(m_launch());
    else
        m_helper.start();
#else
    setAuth(AuthState::Ready);  // already elevated (P12)
#endif
}

void AppController::start()
{
    m_powerTimer.setInterval(5000);
    connect(&m_powerTimer, &QTimer::timeout, this, &AppController::refreshPower);
    m_powerTimer.start();

    if (m_noElevation) {
        setAuth(AuthState::Ready);
        m_smbiosRequested = true;  // nothing to ask for
    } else {
        startHelper();
    }

    QThreadPool::globalInstance()->start([this] {
        const SystemInfo info = sysinfo::collectSystemInfo();
        QMetaObject::invokeMethod(this, [this, info] {
            m_info = info;
#ifdef SYNBENCH_DEV_HOOKS
            if (m_powerForTest) {
                m_info.battery.present = m_powerForTest->present;
                m_info.battery.acOnline = m_powerForTest->acOnline;
                m_info.battery.chargePct = m_powerForTest->chargePct;
            }
#endif
            sysinfo::addOpenGlInfo(m_info);  // GUI thread
            m_infoReady = true;
            qCInfo(lcApp) << "system info collected for" << m_info.machineLabel();
            emit systemInfoChanged();
            emit powerChanged();
            if (m_noElevation || m_authState == AuthState::Failed)
                applyUdevFallback();
            readSmbiosWhenReady();
        });
    });
}

void AppController::retryAuth()
{
    if (m_authState != AuthState::Failed)
        return;
    startHelper();
}

void AppController::quitFailed(const QString &message)
{
    std::fputs((message.toLocal8Bit() + "\n").constData(), stderr);
    QCoreApplication::exit(1);
}

void AppController::quit()
{
    emit quitRequested();
}

void AppController::readSmbiosWhenReady()
{
    if (!m_infoReady || m_smbiosRequested || !privileged().isAvailable())
        return;
#ifdef Q_OS_WIN
    if (m_info.memory.modulesKnown)
        return;  // the Windows collector already read SMBIOS in-process
#endif
    m_smbiosRequested = true;
    // Blocking request: off the GUI thread.
    QThreadPool::globalInstance()->start([this] {
        PrivError error;
        auto tables = privileged().readSmbios(&error);
        QMetaObject::invokeMethod(this, [this, tables, error] { onSmbios(tables, error); });
    });
}

void AppController::onSmbios(std::optional<SmbiosTables> tables, PrivError error)
{
    if (!tables) {
        // An ordinary error with a reason (D1: nothing is "needs admin").
        qCWarning(lcApp) << "couldn't read SMBIOS:" << error.message;
        applyUdevFallback();
        return;
    }
    const auto smbios = sysinfo::parseSmbiosTables(*tables);
    if (!smbios) {
        qCWarning(lcApp) << "unrecognised SMBIOS entry point";
        applyUdevFallback();
        return;
    }
    sysinfo::applySmbios(m_info, *smbios);
    qCInfo(lcApp) << "memory modules:" << m_info.memory.modules.size();
    emit systemInfoChanged();
}

void AppController::applyUdevFallback()
{
#ifdef Q_OS_LINUX
    if (!m_infoReady || m_info.memory.modulesSource == QLatin1StringView("smbios"))
        return;
    if (sysinfo::applyUdevMemory(m_info)) {
        qCInfo(lcApp) << "memory modules from udev (the helper isn't available):" << m_info.memory.modules.size();
        emit systemInfoChanged();
    }
#endif
}

QString AppController::systemSummary() const
{
    if (!m_infoReady)
        return {};
    const double gib = double(m_info.memory.totalBytes) / (1024.0 * 1024 * 1024);
    return QStringLiteral("%1 · %2 · %3 GB RAM · %4")
        .arg(m_info.machineLabel(), m_info.cpu.name)
        .arg(gib, 0, 'f', 1)
        .arg(m_info.os.name);
}

QStringList AppController::memoryModules() const
{
    QStringList out;
    for (const MemoryModule &m : m_info.memory.modules) {
        QStringList parts;
        if (!m.locator.isEmpty())
            parts << m.locator + QLatin1Char(':');
        parts << QStringLiteral("%1 GB").arg(double(m.sizeBytes) / (1024.0 * 1024 * 1024), 0, 'f', 0);
        if (!m.type.isEmpty())
            parts << m.type;
        if (!m.formFactor.isEmpty())
            parts << m.formFactor;
        if (m.configuredSpeedMts > 0 || m.speedMts > 0)
            parts << QStringLiteral("%1 MT/s").arg(m.configuredSpeedMts > 0 ? m.configuredSpeedMts : m.speedMts);
        if (m.rank > 0)
            parts << QStringLiteral("%1R").arg(m.rank);
        if (m.dataWidth > 0)
            parts << QStringLiteral("%1-bit").arg(m.dataWidth);
        if (!m.manufacturer.isEmpty())
            parts << m.manufacturer;
        if (!m.partNumber.isEmpty())
            parts << m.partNumber;
        out << parts.join(u' ');
    }
    return out;
}

namespace {

QString gibText(qint64 bytes)
{
    const double g = double(bytes) / (1024.0 * 1024 * 1024);
    return g >= 100 ? QStringLiteral("%1 GB").arg(qRound(g)) : QStringLiteral("%1 GB").arg(g, 0, 'f', 1);
}

QString capacityText(qint64 bytes)
{
    const double tb = double(bytes) / 1e12;
    return tb >= 1 ? QStringLiteral("%1 TB").arg(tb, 0, 'f', tb >= 10 ? 0 : 1) : QStringLiteral("%1 GB").arg(qRound(double(bytes) / 1e9));
}

QVariantMap infoRow(const QString &label, const QString &value)
{
    return {{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
}

} // namespace

QVariantList AppController::systemRows() const
{
    QVariantList out;
    if (!m_infoReady)
        return out;
    const SystemInfo &i = m_info;
    out << infoRow(tr("Machine"), i.machineLabel());
    if (!i.machine.biosVersion.isEmpty())
        out << infoRow(tr("Firmware"), (i.machine.biosVendor + QLatin1Char(' ') + i.machine.biosVersion).trimmed());

    QStringList shape;
    if (i.cpu.cores > 0)
        shape << tr("%1 cores / %2 threads").arg(i.cpu.cores).arg(i.cpu.threads);
    if (i.cpu.performanceCores > 0 && i.cpu.efficiencyCores > 0)
        shape << tr("%1 performance + %2 efficiency").arg(i.cpu.performanceCores).arg(i.cpu.efficiencyCores);
    if (i.cpu.maxMhz > 0)
        shape << tr("up to %1 GHz").arg(i.cpu.maxMhz / 1000.0, 0, 'f', 1);
    out << infoRow(tr("Processor"), shape.isEmpty() ? i.cpu.name : i.cpu.name + QStringLiteral(", ") + shape.join(QStringLiteral(", ")));

    QString mem = gibText(i.memory.totalBytes);
    if (i.memory.modulesKnown && !i.memory.modules.isEmpty()) {
        const MemoryModule &m = i.memory.modules.first();
        QStringList d;
        if (!m.type.isEmpty())
            d << (m.formFactor.isEmpty() ? m.type : m.type + QLatin1Char(' ') + m.formFactor);
        if (m.configuredSpeedMts > 0 || m.speedMts > 0)
            d << QStringLiteral("%1 MT/s").arg(m.configuredSpeedMts > 0 ? m.configuredSpeedMts : m.speedMts);
        d << (i.memory.modules.size() == 1 ? tr("1 module") : tr("%1 modules").arg(i.memory.modules.size()));
        // Phase 3 P16, as the first module reports them.
        if (m.rank > 0)
            d << QStringLiteral("%1R").arg(m.rank);
        if (m.dataWidth > 0)
            d << tr("%1-bit each").arg(m.dataWidth);
        mem += QStringLiteral(", ") + d.join(QStringLiteral(", "));
    } else {
        mem += QStringLiteral(", ") + tr("module details unavailable");
    }
    out << infoRow(tr("Memory"), mem);

    QStringList gpus;
    for (const GpuInfo &g : i.gpus) {
        QString t = g.name;
        if (g.vramBytes > 0)
            t += QStringLiteral(" (%1)").arg(gibText(g.vramBytes));
        if (!g.driverVersion.isEmpty())
            t += QStringLiteral(", ") + g.driverVersion;
        gpus << t;
    }
    out << infoRow(tr("Graphics"), gpus.isEmpty() ? tr("None found") : gpus.join(QLatin1Char('\n')));

    QStringList vols;
    for (const VolumeInfo &v : i.volumes) {
        const QString kind = v.kind.isEmpty() ? v.bus : (v.bus + QLatin1Char(' ') + v.kind);
        vols << QStringLiteral("%1 %2 (%3)").arg(capacityText(v.totalBytes), kind, v.mount).simplified();
    }
    out << infoRow(tr("Storage"), vols.isEmpty() ? tr("None found") : vols.join(QLatin1Char('\n')));

    QStringList npus;
    for (const NpuInfo &n : i.npus)
        npus << n.name;
    out << infoRow(tr("NPU"), npus.isEmpty() ? tr("None found") : npus.join(QLatin1Char('\n')));

    QString os = (i.os.name + QLatin1Char(' ') + i.os.version).simplified() + QStringLiteral(", ") + i.os.arch;
    if (!i.os.session.isEmpty())
        os += QStringLiteral(", ") + i.os.session;
    out << infoRow(tr("Operating system"), os);
    out << infoRow(tr("SynBench"), QStringLiteral("%1 (%2, Qt %3)").arg(version(), i.cpu.arch, i.app.qtVersion));
    return out;
}

QVariantList AppController::apiPills() const
{
    QVariantList out;
    if (!m_infoReady)
        return out;
    auto pill = [&](const QString &text, bool good) {
        out << QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("tone"), good ? QStringLiteral("good") : QStringLiteral("na")}};
    };
    const ApiInfo &a = m_info.apis;
    if (!a.d3d12FeatureLevel.isEmpty())
        pill(tr("DirectX 12"), true);
    if (!a.d3d11FeatureLevel.isEmpty())
        pill(tr("DirectX 11"), true);
    pill(a.vulkanInstanceVersion.isEmpty() ? tr("Vulkan: not found") : tr("Vulkan %1").arg(a.vulkanInstanceVersion), !a.vulkanInstanceVersion.isEmpty());
    if (!a.openGlVersion.isEmpty())
        pill(a.openGlIsEs ? tr("OpenGL ES %1").arg(a.openGlVersion) : tr("OpenGL %1").arg(a.openGlVersion), true);
    return out;
}

QStringList AppController::batteryLines() const
{
    if (!m_infoReady)
        return {};
    const BatteryInfo &b = m_info.battery;
    if (!b.present)
        return {tr("No battery found")};
    QStringList out;
    if (b.designWh > 0)
        out << tr("%1 Wh design · %2 Wh full charge").arg(b.designWh, 0, 'f', 1).arg(b.fullWh, 0, 'f', 1);
    QStringList s;
    if (b.healthPct > 0)
        s << tr("Health %1%").arg(qRound(b.healthPct));
    if (b.chargePct >= 0)
        s << tr("currently %1%").arg(qRound(b.chargePct));
    s << (b.acOnline ? tr("on AC power") : tr("on battery"));
    if (b.cycleCount >= 0)
        s << (b.cycleCount == 1 ? tr("1 cycle") : tr("%1 cycles").arg(b.cycleCount));
    out << s.join(QStringLiteral(", "));
    return out;
}

} // namespace sb

#include "MainWindow.h"

#include "BackgroundActivity.h"
#include "DeployLogs.h"
#include "MainWindowInternal.h"
#include "StartupTrace.h"

#include <QTimer>

namespace {

const QString kAgentWatchers = QStringLiteral("agent-watchers");
const QString kAgentHousekeeping = QStringLiteral("agent-housekeeping");
const QString kLocalMesh = QStringLiteral("local-mesh");
const QString kOrganizationTasks = QStringLiteral("organization-tasks");
const QString kHostLogFeeds = QStringLiteral("host-log-feeds");
const QString kFleetLogFeeds = QStringLiteral("fleet-log-feeds");

constexpr int kLogFeedsDelayMs = 15 * 1000;
constexpr int kOrganizationTasksDelayMs = 25 * 1000;

}

void MainWindow::registerCorePackageServices()
{
    if (m_corePackageServicesRegistered)
        return;
    m_corePackageServicesRegistered = true;

    m_corePackageServices.add(
        kAgentWatchers, {QStringLiteral("agents"), QStringLiteral("desktops")},
        [this] {
            if (m_machineAgentTimer)
                m_machineAgentTimer->start();
            if (m_cloudAgentWatchTimer)
                m_cloudAgentWatchTimer->start();
        },
        [this] {
            if (m_machineAgentTimer)
                m_machineAgentTimer->stop();
            if (m_cloudAgentWatchTimer)
                m_cloudAgentWatchTimer->stop();
        });

    m_corePackageServices.add(
        kAgentHousekeeping, {QStringLiteral("agents")},
        [this] {
            purgeExpiredAgentRecycleBinInBackground();
            if (m_agentRecycleSweepTimer)
                m_agentRecycleSweepTimer->start(60 * 60 * 1000);
#ifndef FORKMESH_WINDOW_TESTS
            startAgentParityWatch();
#endif
        },
        [this] {
            if (m_agentRecycleSweepTimer)
                m_agentRecycleSweepTimer->stop();
            stopAgentParityWatch();
        });

    m_corePackageServices.add(
        kLocalMesh, {QStringLiteral("chat"), QStringLiteral("desktops")},
        [this] {
            if (m_backend)
                startLocalMesh();
        },
        [this] { stopLocalMesh(); });

    m_corePackageServices.add(
        kOrganizationTasks, {QStringLiteral("tasks"), QStringLiteral("agents")},
        [this] {
#ifndef FORKMESH_WINDOW_TESTS
            QTimer::singleShot(kOrganizationTasksDelayMs, this, [this] {
                if (!corePackageServiceRunning(kOrganizationTasks))
                    return;
                forkmesh::StartupTraceStep step(
                    QStringLiteral("delayed startup: refresh organization task badge"));
                const forkmesh::BackgroundRoutine routine(
                    QStringLiteral("delayed startup: refresh organization task badge"));
                refreshOrganizationTaskBadge();
            });
#endif
        },
        {});

    m_corePackageServices.add(
        kHostLogFeeds, {QStringLiteral("hosts"), QStringLiteral("logs")},
        [this] {
            QTimer::singleShot(kLogFeedsDelayMs, this, [this] {
                if (!corePackageServiceRunning(kHostLogFeeds))
                    return;
                m_hostLogFeedsArmed = true;
                syncHostLogFeeds();
            });
        },
        [this] {
            m_hostLogFeedsArmed = false;
            stopHostLogFeeds();
        });

    m_corePackageServices.add(
        kFleetLogFeeds, {QStringLiteral("logs")},
        [this] {
            QTimer::singleShot(kLogFeedsDelayMs, this, [this] {
                if (!corePackageServiceRunning(kFleetLogFeeds))
                    return;
                m_coreLogFeedsArmed = true;
                syncCoreLogFeeds();
                startDeployLogFollower();
            });
        },
        [this] {
            m_coreLogFeedsArmed = false;
            stopCoreLogFeeds();
            if (m_fleetInventoryTimer)
                m_fleetInventoryTimer->stop();
            if (m_deployLogFollower)
                m_deployLogFollower->stop();
        });
}

void MainWindow::syncCorePackageServices()
{
    if (!m_corePackageServicesRegistered || m_closingDown)
        return;
    const QStringList before = m_corePackageServices.running();
    m_corePackageServices.sync(
        [this](const QString &packageId) { return packageSurfaceShown(packageId); });
    const QStringList after = m_corePackageServices.running();
    if (before == after)
        return;
    QStringList stopped;
    for (const QString &id : before) {
        if (!after.contains(id))
            stopped.append(id);
    }
    QStringList started;
    for (const QString &id : after) {
        if (!before.contains(id))
            started.append(id);
    }
    qInfo().noquote() << QStringLiteral("[core services] started: %1; stopped: %2; running: %3")
                             .arg(started.isEmpty() ? QStringLiteral("none") : started.join(QStringLiteral(", ")),
                                  stopped.isEmpty() ? QStringLiteral("none") : stopped.join(QStringLiteral(", ")),
                                  after.isEmpty() ? QStringLiteral("none") : after.join(QStringLiteral(", ")));
}

bool MainWindow::corePackageServiceRunning(const QString &id) const
{
    return m_corePackageServices.isRunning(id);
}

#include "ForkMeshVersion.h"
#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlUpdate.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QJsonObject>
#include <QProcess>
#include <QPushButton>
#include <QTimer>

namespace updatectl = forkmesh::localcontrol::update;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

const qint64 kProcessStartedMs = QDateTime::currentMSecsSinceEpoch();
constexpr int kUpdateFlushMs = 500;

} // namespace

QJsonObject MainWindow::localControlUpdateState() const
{
    updatectl::RunState run;
    run.running = restartInProgress() || m_computerRestarting;
    if (m_computerRestarting) {
        run.kind = updatectl::computerRestartActionName();
    }
    if (m_restartSpinButton) {
        run.kind = m_restartKind == RestartKind::Update ? QStringLiteral("update")
                                                        : QStringLiteral("rebuild");
        run.percent = m_restartProgress;
        run.phase = m_restartPhase;
        run.activity = restartCountdownText();
    } else if (run.running) {
        run.kind = QStringLiteral("rebuild");
    }
    return updatectl::stateObject(m_updateFeed, run);
}

void MainWindow::watchLocalControlUpdate(int connectionId)
{
    UpdateWatcher &watcher = m_updateWatchers[connectionId];
    watcher.sentSeq = m_updateFeed.seq();
    watcher.sentState = localControlUpdateState();
    if (!m_updateWatchTimer) {
        m_updateWatchTimer = new QTimer(this);
        m_updateWatchTimer->setInterval(kUpdateFlushMs);
        connect(m_updateWatchTimer, &QTimer::timeout, this,
                &MainWindow::flushLocalControlUpdate);
    }
    if (!m_updateWatchTimer->isActive())
        m_updateWatchTimer->start();
}

void MainWindow::flushLocalControlUpdate()
{
    if (!m_localControl || m_updateWatchers.isEmpty()) {
        m_updateWatchers.clear();
        if (m_updateWatchTimer)
            m_updateWatchTimer->stop();
        return;
    }
    const QJsonObject state = localControlUpdateState();
    for (auto it = m_updateWatchers.begin(); it != m_updateWatchers.end();) {
        const QJsonObject batch = m_updateFeed.batchSince(it->sentSeq);
        const bool hasLines = !batch.value(QStringLiteral("lines")).toArray().isEmpty();
        if (!hasLines && state == it->sentState) {
            ++it;
            continue;
        }
        QJsonObject data = state;
        for (auto field = batch.begin(); field != batch.end(); ++field)
            data.insert(field.key(), field.value());
        if (!m_localControl->sendEventTo(it.key(), QStringLiteral("update.progress"), data)) {
            it = m_updateWatchers.erase(it);
            continue;
        }
        it->sentSeq = m_updateFeed.seq();
        it->sentState = state;
        ++it;
    }
    if (m_updateWatchers.isEmpty() && m_updateWatchTimer)
        m_updateWatchTimer->stop();
}

void MainWindow::startLocalControlUpdate()
{
    if (restartInProgress())
        return;
    QPushButton *button = m_navRebuildButton       ? m_navRebuildButton
                          : m_profileRebuildButton ? m_profileRebuildButton
                                                   : m_rebuildButton;
    if (button)
        startRestartSpin(button);
    quickRebuildRestart();
    flushLocalControlUpdate();
}

void MainWindow::restartComputer()
{
    QProcess *process = new QProcess(this);
    process->setProgram(QStringLiteral("systemctl"));
    process->setArguments({QStringLiteral("--no-ask-password"), QStringLiteral("reboot")});
    process->setProcessChannelMode(QProcess::MergedChannels);
    const auto failed = [this, process](const QString &message) {
        if (!m_computerRestarting) {
            process->deleteLater();
            return;
        }
        m_computerRestarting = false;
        setUpdateStatus(message, true);
        logSystem(message);
        flashMessage(message);
        process->deleteLater();
    };
    connect(process, &QProcess::errorOccurred, this,
            [failed, process](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    failed(QStringLiteral("Could not request a computer restart: %1")
                               .arg(process->errorString()));
            });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, failed, process](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                    logSystem(QStringLiteral("=== Computer restart requested ==="));
                    process->deleteLater();
                    return;
                }
                failed(updatectl::computerRestartFailure(
                    exitCode, QString::fromUtf8(process->readAllStandardOutput())));
            });
    process->start();
}

void MainWindow::handleLocalControlUpdateRequest(int connectionId, int requestId,
                                                 const QString &method,
                                                 const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);

    if (method == QLatin1String("update.status")) {
        QJsonObject reply = localControlUpdateState();
        const QJsonArray lines = m_updateFeed.backlog();
        reply.insert(QStringLiteral("lines"), lines);
        reply.insert(QStringLiteral("seq"), double(m_updateFeed.seq()));
        reply.insert(QStringLiteral("first"), double(m_updateFeed.seq() - lines.size() + 1));
        reply.insert(QStringLiteral("version"), QStringLiteral(FORKMESH_VERSION));
        reply.insert(QStringLiteral("startedAtMs"), double(kProcessStartedMs));
        watchLocalControlUpdate(connectionId);
        server->respond(connectionId, requestId, reply);
        return;
    }
    if (method == QLatin1String("update.unwatch")) {
        m_updateWatchers.remove(connectionId);
        server->respond(connectionId, requestId, QJsonObject{});
        return;
    }
    if (method == QLatin1String("update.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        const bool computerRestart = action == updatectl::computerRestartActionName();
        if (action != updatectl::actionName() && !computerRestart) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_action"));
            return;
        }
        if (restartInProgress() || m_computerRestarting) {
            server->respondError(connectionId, requestId, QStringLiteral("already_running"));
            return;
        }
        int agentsRunning = 0;
        for (const AgentSession &session : std::as_const(m_agentSessions))
            if (isStoppableAgentSession(session.id))
                ++agentsRunning;
        const QString token = m_updateControlConfirmations.issue(
            device.key, action,
            computerRestart ? updatectl::computerRestartSubjectName() : updatectl::subjectName(),
            QDateTime::currentMSecsSinceEpoch());
        server->respond(
            connectionId, requestId,
            {{QStringLiteral("token"), token},
             {QStringLiteral("action"), action},
             {QStringLiteral("subject"), computerRestart ? updatectl::computerRestartSubjectName()
                                                          : updatectl::subjectName()},
             {QStringLiteral("summary"),
              computerRestart ? updatectl::computerRestartSummary(agentsRunning)
                              : updatectl::actionSummary(agentsRunning, QStringLiteral(FORKMESH_VERSION))},
             {QStringLiteral("expiresInMs"), double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("update.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        const bool computerRestart = action == updatectl::computerRestartActionName();
        const hostctl::ConfirmationStore::Result verdict = m_updateControlConfirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, action,
            computerRestart ? updatectl::computerRestartSubjectName() : updatectl::subjectName(),
            QDateTime::currentMSecsSinceEpoch());
        if ((action != updatectl::actionName() && !computerRestart) ||
            verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = action != updatectl::actionName() && !computerRestart
                                    ? QStringLiteral("unknown_action")
                                    : hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused a rebuild and restart from %1: %2.")
                          .arg(localControlDeviceLabel(connectionId), why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        if (restartInProgress() || m_computerRestarting) {
            server->respondError(connectionId, requestId, QStringLiteral("already_running"));
            return;
        }
        if (computerRestart) {
            logSystem(QStringLiteral("Computer restart requested from %1.")
                          .arg(localControlDeviceLabel(connectionId)));
            watchLocalControlUpdate(connectionId);
            m_computerRestarting = true;
            m_updateFeed.reset();
            m_updateFeed.append(QStringLiteral("==> Restarting computer\n$ systemctl reboot\n"));
            setUpdateStatus(QStringLiteral("Restarting computer..."));
            server->respond(connectionId, requestId,
                            {{QStringLiteral("started"), true},
                             {QStringLiteral("action"), action},
                             {QStringLiteral("subject"), updatectl::computerRestartSubjectName()},
                             {QStringLiteral("message"), QStringLiteral("The computer is restarting.")}});
            QTimer::singleShot(250, this, [this] { restartComputer(); });
            return;
        }
        logSystem(QStringLiteral("Rebuild and restart requested from %1.")
                      .arg(localControlDeviceLabel(connectionId)));
        watchLocalControlUpdate(connectionId);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), updatectl::actionName()},
                         {QStringLiteral("subject"), updatectl::subjectName()},
                         {QStringLiteral("message"),
                          QStringLiteral("Rebuilding. The desktop restarts when the build is done.")}});
        QTimer::singleShot(0, this, [this] { startLocalControlUpdate(); });
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

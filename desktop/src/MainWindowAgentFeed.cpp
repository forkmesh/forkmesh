#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentActivityFeed.h"
#include "AgentAnnouncer.h"
#include "AgentSonifier.h"
#include "AppPaths.h"
#include "ClaudePeerMessaging.h"
#include "ClaudeSessionScan.h"
#include "LocalControl.h"
#include "MachineAgentScan.h"
#include "SystemStats.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>

using forkmesh::sonify::Motion;

namespace {

forkmesh::agentfeed::RunState runStateFor(const AgentSession &session)
{
    using forkmesh::agentfeed::RunState;
    if (session.status == AgentStatus::Running)
        return RunState::Running;
    if (session.status == AgentStatus::Waiting)
        return RunState::Waiting;
    if (session.status == AgentStatus::Success)
        return RunState::Done;
    if (session.status == AgentStatus::Failed || session.status == AgentStatus::Stopped)
        return RunState::Failed;
    return RunState::Idle;
}

} // namespace

forkmesh::sonify::AgentSonifier *MainWindow::ensureAgentSonifier()
{
    if (m_agentSonifier)
        return m_agentSonifier;
    m_agentSonifier = new forkmesh::sonify::AgentSonifier(this);
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion != Motion::Rest)
            m_agentSonifier->setMuted(motion, m_agentMutedMotions.contains(int(motion)));
    }
    m_agentSonifier->setNoteObserver(
        [](int sessionId, Motion motion, const forkmesh::sonify::Note &note,
           double delaySeconds) {
            forkmesh::agentfeed::publishNote(sessionId, motion, delaySeconds, note.gain,
                                             note.seed);
        });
    return m_agentSonifier;
}

void MainWindow::feedAgentActivity(int sessionId, const QJsonObject &event)
{
    if (m_agentSonifier)
        m_agentSonifier->handleEvent(sessionId, event);
    forkmesh::agentfeed::publishAgentEvent(sessionId, event);
}

QString MainWindow::externalActivityTranscript(int sessionId) const
{
    const auto surfaced = m_externalSurfaced.constFind(sessionId);
    if (surfaced != m_externalSurfaced.constEnd())
        return surfaced->path;
    const MachineAgent agent = m_machineAgents.value(sessionId);
    return agent.kind == QLatin1String("claude") ? agent.transcript : QString();
}

void MainWindow::pumpExternalAgentActivity()
{
    const bool watching = forkmesh::agentfeed::watched();
    QList<int> sessionIds = m_externalSurfaced.keys();
    sessionIds += m_machineAgents.keys();
    for (const int sessionId : std::as_const(sessionIds)) {
        const bool heard =
            m_agentSonifier && m_agentSonifier->isListening(sessionId);
        const QString path = externalActivityTranscript(sessionId);
        if ((!watching && !heard) || path.isEmpty()) {
            m_agentActivityExternalOffset.remove(sessionId);
            continue;
        }
        if (m_agentActivityExternalReads.contains(sessionId))
            continue;
        const qint64 offset = m_agentActivityExternalOffset.value(sessionId, -1);
        m_agentActivityExternalReads.insert(sessionId);
        struct Tail {
            qint64 offset = 0;
            QList<QJsonObject> events;
        };
        runOffThread<Tail>(
            [path, offset] {
                Tail tail;
                if (offset < 0) {
                    tail.offset = QFileInfo(path).size();
                    return tail;
                }
                tail.events = ClaudeSessionScan::readEvents(path, offset, &tail.offset);
                return tail;
            },
            [this, sessionId, path](Tail tail) {
                m_agentActivityExternalReads.remove(sessionId);
                if (externalActivityTranscript(sessionId) != path)
                    return;
                m_agentActivityExternalOffset.insert(sessionId, tail.offset);
                for (const QJsonObject &event : std::as_const(tail.events))
                    feedAgentActivity(sessionId, event);
            });
    }
    for (auto it = m_agentActivityExternalOffset.begin();
         it != m_agentActivityExternalOffset.end();) {
        if (!externalActivityTranscript(it.key()).isEmpty())
            ++it;
        else
            it = m_agentActivityExternalOffset.erase(it);
    }
}

void MainWindow::tickMachineAgents()
{
    bool heard = false;
    if (m_agentSonifier) {
        for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it)
            heard = heard || m_agentSonifier->isListening(it.key());
    }
    const bool phone = m_localControl && !m_localControl->connectedDevices().isEmpty();
    if (!forkmesh::agentfeed::watched() && !heard && !phone) {
        if (m_machineAgentScanInFlight) {
            ++m_machineAgentScanGeneration;
            m_machineAgentScanInFlight = false;
        }
        if (!m_machineAgents.isEmpty())
            applyMachineAgents({});
        m_localControlOutsideTails.clear();
        return;
    }
    scanMachineAgents();
    pumpExternalAgentActivity();
    ringLocalControlOutsideTranscripts();
}

void MainWindow::scanMachineAgents()
{
    if (m_machineAgentScanInFlight)
        return;
    QSet<qint64> ownRoots{QCoreApplication::applicationPid()};
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (isExternalSession(session.id))
            continue;
        const qint64 pid = agentSessionProcessId(session.id);
        if (pid > 0)
            ownRoots.insert(pid);
    }
    QList<ExternalClaudeSession> surfaced;
    for (auto it = m_externalSurfaced.constBegin(); it != m_externalSurfaced.constEnd(); ++it) {
        if (externalIsLive(it->uuid))
            surfaced.append(it.value());
    }
    const QSet<QString> ownCwds = ownStreamCwds();
    const QSet<qint64> foreignRoots = localControlTerminalPids();
    const quint64 generation = ++m_machineAgentScanGeneration;
    m_machineAgentScanInFlight = true;
    const QString home = QDir::homePath();
    const QString dataRoot = forkmesh::paths::dataRoot();
    const QString terminalDir = TerminalHandoff::runtimeDir();
    struct Scan {
        QList<MachineAgent> agents;
        QList<ClaudePeerSession> peers;
        QList<TerminalHandoffSession> terminals;
        QHash<QString, QList<qint64>> surfacedPids;
    };
    runOffThread<Scan>(
        [ownRoots, ownCwds, foreignRoots, surfaced, home, dataRoot, terminalDir] {
            const SystemStats::ProcessTable table = SystemStats::processTable();
            Scan result;
            QList<MachineAgent> &agents = result.agents;
            agents = MachineAgentScan::scan(table, ownRoots, ownCwds, foreignRoots);
            QStringList extraDirs;
            for (const MachineAgent &agent : std::as_const(agents)) {
                if (!agent.configDir.isEmpty())
                    extraDirs.append(agent.configDir);
            }
            result.peers = ClaudePeerMessaging::liveSessions(
                ClaudePeerMessaging::configDirs(home, dataRoot, extraDirs));
            result.terminals = TerminalHandoff::liveSessions(terminalDir);
            if (surfaced.isEmpty())
                return result;
            QList<qint64> claudePids;
            for (const MachineAgent &agent : std::as_const(agents)) {
                if (agent.kind == QLatin1String("claude"))
                    claudePids.append(agent.pid);
            }
            QSet<qint64> surfacedPids;
            QSet<QString> surfacedPaths;
            for (const ExternalClaudeSession &ext : surfaced) {
                surfacedPaths.insert(ext.path);
                for (const qint64 pid :
                     ClaudeSessionScan::findSessionPids(ext.uuid, ext.cwd, claudePids)) {
                    surfacedPids.insert(pid);
                    result.surfacedPids[ext.uuid].append(pid);
                }
            }
            agents.erase(std::remove_if(agents.begin(), agents.end(),
                                        [&](const MachineAgent &agent) {
                                            return surfacedPids.contains(agent.pid)
                                                   || surfacedPaths.contains(agent.transcript);
                                        }),
                         agents.end());
            return result;
        },
        [this, generation](Scan result) {
            if (generation != m_machineAgentScanGeneration)
                return;
            m_machineAgentScanInFlight = false;
            applyMachineAgents(result.agents, result.peers, result.terminals,
                               result.surfacedPids);
        });
}

void MainWindow::applyMachineAgents(const QList<MachineAgent> &agents,
                                    const QList<ClaudePeerSession> &peers,
                                    const QList<TerminalHandoffSession> &terminals,
                                    const QHash<QString, QList<qint64>> &surfacedPids)
{
    QHash<int, int> reachBefore;
    for (auto it = m_externalSurfaced.constBegin(); it != m_externalSurfaced.constEnd(); ++it)
        reachBefore.insert(it.key(), outsideAgentReach(it.key()));
    for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it)
        reachBefore.insert(it.key(), outsideAgentReach(it.key()));
    QHash<int, MachineAgent> next;
    for (const MachineAgent &agent : agents)
        next.insert(externalTempIdFor(QStringLiteral("pid:%1").arg(agent.pid)), agent);
    for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it) {
        const auto kept = next.constFind(it.key());
        if (kept == next.constEnd() || kept->transcript != it->transcript)
            m_agentActivityExternalOffset.remove(it.key());
        if (kept == next.constEnd()) {
            m_externalTempId.remove(QStringLiteral("pid:%1").arg(it->pid));
            if (m_agentSonifier && m_agentSonifier->isListening(it.key()))
                m_agentSonifier->stopListening(it.key());
        }
    }
    bool changed = QSet<int>(next.keyBegin(), next.keyEnd()) !=
                   QSet<int>(m_machineAgents.keyBegin(), m_machineAgents.keyEnd());
    m_machineAgents = std::move(next);
    m_claudePeerSessions = peers;
    m_terminalHandoffSessions = terminals;
    m_externalSurfacedPids = surfacedPids;
    QHash<int, int> reachAfter;
    for (auto it = m_externalSurfaced.constBegin(); it != m_externalSurfaced.constEnd(); ++it)
        reachAfter.insert(it.key(), outsideAgentReach(it.key()));
    for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it)
        reachAfter.insert(it.key(), outsideAgentReach(it.key()));
    changed = changed || reachAfter != reachBefore;
    if (changed)
        notifyLocalControlAgentsChanged();
}

void MainWindow::noteAgentActivityTask(const forkmesh::BackgroundTicket &ticket, bool started)
{
    forkmesh::agentfeed::TaskEvent task;
    task.id = ticket.id;
    task.word = backgroundTaskWord(ticket.kind);
    task.started = started;
    if (!started) {
        const QString outcome = ticket.outcome.trimmed();
        task.failed = !forkmesh::backgroundFailureText(outcome).isEmpty()
                      || outcome == QLatin1String("failed");
        forkmesh::agentfeed::publishTask(task);
        return;
    }
    task.detail = forkmesh::backgroundDetailNote(ticket.detail);
    if (!ticket.workingDirectory.isEmpty()) {
        forkmesh::BackgroundOutcomeContext context;
        describeBackgroundWorkingDirectory(ticket.workingDirectory, &context);
        if (context.agent.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const int id = context.agent.mid(1).section(QLatin1Char(' '), 0, 0).toInt(&ok);
            if (ok)
                task.sessionId = id;
        }
    }
    forkmesh::agentfeed::publishTask(task);
}

bool MainWindow::listenToEverything(bool on)
{
    if (!on) {
        if (m_agentSonifier) {
            const QList<int> playing = m_agentSonifier->listeningSessions();
            for (int sessionId : playing)
                m_agentSonifier->stopListening(sessionId);
            m_agentSonifier->stopDesktop();
            m_agentListenStatus.clear();
        }
        refreshAgentListenButton();
        return false;
    }
    bool any = ensureAgentSonifier()->listenDesktop();
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (beginAgentListening(session.id, /*ringIfFinished=*/false))
            any = true;
    }
    if (!any)
        flashPingMessage(
            QStringLiteral("No audio output on this machine, so there is "
                           "nothing to listen to."),
            /*error=*/true);
    refreshAgentListenButton();
    return any;
}

void MainWindow::installAgentActivityHost()
{
    using namespace forkmesh::agentfeed;
    Host host;
    const auto describe = [this](const AgentSession &session) {
        Agent agent;
        agent.id = session.id;
        agent.name = session.name;
        agent.task = session.issueTitle;
        if (session.cloud) {
            const QString node = session.cloudNode.isEmpty()
                                     ? QStringLiteral("an unclaimed node")
                                     : session.cloudNode;
            agent.task = (session.status == AgentStatus::Queued
                              ? QStringLiteral("%1, web run queued for %2")
                              : QStringLiteral("%1, web run on %2"))
                             .arg(session.issueTitle, node);
        }
        agent.prompt = session.prompt;
        agent.lastReply = m_lastAssistantText.value(session.id);
        agent.state = runStateFor(session);
        agent.turns = session.numTurns;
        agent.tokens = session.totalTokens;
        const AgentDiffStat diff = m_agentDiffStats.value(session.id);
        agent.files = diff.files;
        agent.linesAdded = diff.added;
        agent.linesRemoved = diff.removed;
        agent.conflicted = diff.conflicted;
        agent.merged = session.merged;
        agent.stopped = session.status == AgentStatus::Stopped;
        agent.listening = m_agentSonifier && m_agentSonifier->isListening(session.id);
        if (agent.listening)
            agent.voice = m_agentSonifier->voiceFor(session.id);
        return agent;
    };
    const auto describeMachine = [this](int sessionId, const MachineAgent &machine) {
        Agent agent;
        agent.id = sessionId;
        const QString folder = QFileInfo(machine.cwd).fileName();
        agent.name = folder.isEmpty() ? MachineAgentScan::label(machine.kind) : folder;
        agent.task = machine.otherUser.isEmpty()
                         ? QStringLiteral("%1, started outside ForkMesh")
                               .arg(MachineAgentScan::label(machine.kind))
                         : QStringLiteral("%1, running in %2's login")
                               .arg(MachineAgentScan::label(machine.kind), machine.otherUser);
        agent.prompt = machine.cwd;
        agent.state = RunState::Running;
        agent.listening = m_agentSonifier && m_agentSonifier->isListening(sessionId);
        if (agent.listening)
            agent.voice = m_agentSonifier->voiceFor(sessionId);
        const QString provider = MachineAgentScan::provider(machine.kind);
        agent.faceKey = provider + QLatin1Char('|') + machine.model;
        agent.face = forkmesh::ui::agentModelIcon(provider, machine.model);
        return agent;
    };
    host.agents = [this, describe, describeMachine] {
        QList<Agent> agents;
        agents.reserve(m_agentSessions.size() + m_machineAgents.size());
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            Agent agent = describe(session);
            agent.faceKey = session.provider + QLatin1Char('|') + session.model;
            agent.face = forkmesh::ui::agentStatusModelIcon(session);
            agents.append(agent);
        }
        for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it)
            agents.append(describeMachine(it.key(), it.value()));
        return agents;
    };
    host.agent = [this, describe, describeMachine](int sessionId, Agent *agent) {
        const AgentSession *session = findAgentSession(sessionId);
        if (!session) {
            const auto machine = m_machineAgents.constFind(sessionId);
            if (machine == m_machineAgents.constEnd())
                return false;
            if (agent)
                *agent = describeMachine(sessionId, machine.value());
            return true;
        }
        if (agent)
            *agent = describe(*session);
        return true;
    };
    host.isExternal = [this](int sessionId) { return isExternalSession(sessionId); };
    host.processRoots = [this](QList<ProcessRoot> *roots, QList<ExternalRun> *externals) {
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            if (isExternalSession(session.id)) {
                const auto ext = m_externalSurfaced.constFind(session.id);
                if (ext != m_externalSurfaced.constEnd() && externalIsLive(ext->uuid))
                    externals->append({session.id, ext->uuid, ext->cwd});
                continue;
            }
            const qint64 pid = agentSessionProcessId(session.id);
            if (pid > 0)
                roots->append({session.id, pid, true});
        }
        for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it)
            roots->append({it.key(), it->pid, true});
    };
    host.showAgent = [this](int sessionId) {
        if (const auto machine = m_machineAgents.constFind(sessionId);
            machine != m_machineAgents.constEnd()) {
            flashPingMessage(
                machine->otherUser.isEmpty()
                    ? QStringLiteral("%1 was started outside ForkMesh, in %2")
                          .arg(MachineAgentScan::label(machine->kind), machine->cwd)
                    : QStringLiteral("%1 is running in %2's login on this computer. ForkMesh "
                                     "cannot read or steer another login's agents; open "
                                     "ForkMesh in that login to follow it.")
                          .arg(MachineAgentScan::label(machine->kind), machine->otherUser),
                /*error=*/false);
            return;
        }
        switchToAgentsTab(sessionId);
        if (!m_agentTable)
            return;
        for (int row = 0; row < m_agentTable->rowCount(); ++row) {
            QTableWidgetItem *cell = m_agentTable->item(row, 0);
            if (cell && cell->data(Qt::UserRole).toInt() == sessionId) {
                m_agentTable->selectRow(row);
                break;
            }
        }
    };
    host.showLogs = [this] {
        showSection(4);
        raise();
        activateWindow();
    };
    host.listening = [this](int sessionId) {
        return m_agentSonifier && m_agentSonifier->isListening(sessionId);
    };
    host.toggleListening = [this](int sessionId) { toggleAgentListening(sessionId); };
    host.listen = [this](int sessionId) {
        return beginAgentListening(sessionId, /*ringIfFinished=*/false);
    };
    host.listenToEverything = [this](bool on) { return listenToEverything(on); };
    host.muted = [this](Motion motion) {
        return m_agentMutedMotions.contains(int(motion));
    };
    host.setMuted = [this](Motion motion, bool muted) {
        if (muted)
            m_agentMutedMotions.insert(int(motion));
        else
            m_agentMutedMotions.remove(int(motion));
        if (m_agentSonifier)
            m_agentSonifier->setMuted(motion, muted);
    };
    host.playCue = [this](int sessionId, Motion motion, quint32 seed) {
        return m_agentSonifier && m_agentSonifier->handleCue(sessionId, motion, seed);
    };
    host.playNetwork = [this](int sessionId, qint64 up, qint64 down) {
        if (m_agentSonifier)
            m_agentSonifier->handleNetwork(sessionId, up, down);
    };
    host.recentLogLines = [this] { return m_networkLog; };
    host.logStyle = [this](const QString &line) {
        const LogLineStyle &style = logStyleFor(line);
        return LogStyle{style.badge, style.accent, style.message};
    };
    host.runningTasks = [this] {
        return m_backgroundTaskWords;
    };
    host.relayClients = [this] {
        const QString ownId = m_profileIdentity.publicKey();
        QList<RelayClient> clients;
        for (const MemberInfo &member : std::as_const(m_homeRoster)) {
            if (member.self || !member.online || member.id.isEmpty()
                || (!ownId.isEmpty() && member.id == ownId))
                continue;
            clients.append({member.id, member.name, member.platform, member.accountKind,
                            member.nodeName, member.version});
        }
        return clients;
    };
    host.relayDiagnostics = [this] {
        const QList<QJsonObject> rows = m_backend ? m_backend->networkDiagnostics()
                                                  : QList<QJsonObject>();
        return rows.isEmpty() ? QJsonObject() : rows.first();
    };
    host.savedHosts = [] {
        QSettings settings;
        return forkmesh::control::loadSavedHosts(settings, forkmesh::ui::kHostsSetting,
                                                 nullptr);
    };
    host.replySpeech = [this] { return agentProseSpoken(); };
    host.setReplySpeech = [this](bool on, QWidget *anchor) {
        if (!on) {
            setAgentProseSpoken(false);
            return false;
        }
        if (setAgentProseSpoken(true))
            return true;
        if (forkmesh::announce::locateVoice().ok())
            flashPingMessage(QStringLiteral("There is no audio output on this "
                                            "machine to read replies through."),
                             /*error=*/true);
        else
            offerAgentAnnouncerVoice(anchor, /*forProse=*/true);
        return false;
    };
    host.speakReply = [this](int sessionId, const QString &who, const QString &text) {
        noteAgentProse(sessionId, who, text);
    };
    host.ping = [this](const QString &text, bool error) { flashPingMessage(text, error); };
    host.log = [this](const QString &text) { logSystem(text); };
    host.cloudflareToken = [] {
        return forkmesh::control::cloudflareApiTokenFromVariables(ActionStore::variables());
    };
    host.cloudflareZoneName = [] {
        QString configured =
            QSettings().value(QStringLiteral("control/cloudflareZone")).toString().trimmed();
        if (configured.isEmpty())
            configured =
                forkmesh::control::cloudflareZoneNameFromVariables(ActionStore::variables());
        return configured;
    };
    host.cloudflare = [this](const QString &token, const QString &path,
                             const QByteArray &method, const QJsonObject &body,
                             CloudflareReply onDone) {
        if (!m_networkAccess) {
            onDone({}, QStringLiteral("network access is unavailable"));
            return;
        }
        cloudflareApiCall(token, path, method, body, std::move(onDone));
    };
    setHost(this, std::move(host));
    if (!m_machineAgentTimer) {
        m_machineAgentTimer = new QTimer(this);
        m_machineAgentTimer->setInterval(2000);
        connect(m_machineAgentTimer, &QTimer::timeout, this, &MainWindow::tickMachineAgents);
        m_machineAgentTimer->start();
    }
    if (!m_cloudAgentWatchTimer) {
        m_cloudAgentWatchTimer = new QTimer(this);
        m_cloudAgentWatchTimer->setInterval(1000);
        connect(m_cloudAgentWatchTimer, &QTimer::timeout, this, [this] {
            if (forkmesh::agentfeed::watched())
                refreshCloudAgentSessions();
            pumpCloudAgentStream();
        });
        m_cloudAgentWatchTimer->start();
    }
}

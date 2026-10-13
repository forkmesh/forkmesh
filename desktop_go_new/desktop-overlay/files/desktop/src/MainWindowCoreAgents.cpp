#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"

#include "AgentProcess.h"
#include "AgentRuntimeSession.h"
#include "AgentStore.h"
#include "ClaudeStreamSession.h"
#include "CodexAppServerSession.h"
#include "AppPaths.h"
#include "CoreAgentRun.h"
#include "CoreAgentStore.h"
#include "CoreClient.h"
#include "CoreProcess.h"
#include "CoreTerminal.h"
#include "LocalControlAttachments.h"
#include "TerminalWidget.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>

#include <utility>

namespace {

constexpr int kCoreAgentAdoptionTimeoutMs = 30000;

bool coreAgentsWanted()
{
    return !forkmesh::core::disabledByEnvironment() &&
           QSettings().value(QStringLiteral("core/enabled"), true).toBool() &&
           forkmesh::coreagentproc::enabledInSettings();
}

bool adoptableStatus(const AgentSession &session)
{
    return !session.merged && session.status != AgentStatus::Queued &&
           session.status != AgentStatus::Stopped && session.status != AgentStatus::Cleared;
}

bool turnEndedWithResult(const QList<QJsonObject> &events)
{
    for (auto it = events.crbegin(); it != events.crend(); ++it) {
        const QString type = it->value(QStringLiteral("type")).toString();
        if (type.isEmpty() || type.startsWith(QLatin1Char('_')) ||
            type == QLatin1String("stream_event"))
            continue;
        return type == QLatin1String("result");
    }
    return false;
}

} // namespace

struct MainWindow::CoreAgentAdoption {
    bool scanned = false;
    bool fallbackArmed = false;
    bool listing = false;
    QHash<int, forkmesh::coreagentproc::LiveProcess> live;
    QSet<int> pending;
    QHash<int, QJsonObject> waiting;
    QList<forkmesh::coreagentproc::LiveProcess> rejected;
};

void MainWindow::installCoreAgentProcesses()
{
    if (!m_coreClient)
        return;
    CoreProcess::install(m_coreClient);
    const QString storeRoot = forkmesh::paths::dataRoot() + QStringLiteral("/agents");
    {
        QPointer<CoreClient> agentRunClient = m_coreClient;
        forkmesh::coreagentrun::Backend backend;
        backend.available = [agentRunClient, storeRoot] {
            return agentRunClient && agentRunClient->isConnected() &&
                   forkmesh::coreagentstore::routes(storeRoot);
        };
        backend.hasRoute = [agentRunClient](const QString &method) {
            return agentRunClient && agentRunClient->hasNativeRoute(method);
        };
        backend.request = [agentRunClient](const QString &method, const QJsonObject &params,
                                           forkmesh::coreagentrun::Done done) {
            if (!agentRunClient || !agentRunClient->isConnected()) {
                done(false, {}, QStringLiteral("core_unavailable"));
                return;
            }
            agentRunClient->request(method, params,
                                    [done](bool ok, const QJsonValue &result, const QString &error) {
                                        done(ok, result.toObject(), error);
                                    });
        };
        backend.notify = [agentRunClient](const QString &method, const QJsonObject &params) {
            return agentRunClient && agentRunClient->isConnected() &&
                   agentRunClient->notify(method, params);
        };
        backend.credentials = [] { return localProviderCredentialValues(); };
        forkmesh::coreagentrun::setBackend(std::move(backend));
        connect(m_coreClient, &CoreClient::coreEvent, this,
                [](const QString &event, const QJsonObject &data) {
                    forkmesh::coreagentrun::dispatch(event, data);
                });
    }
    CoreClient *client = m_coreClient;
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this, client](bool connected) {
        if (connected)
            forkmesh::coreagentstore::attach(
                forkmesh::core::readEndpoint(client->stateDirectory()));
        else
            forkmesh::coreagentstore::detach();
        forkmesh::coreagentrun::connectionChanged(connected);
        if (connected && client->hasNativeRoute(QStringLiteral("core.agentrun.salt")))
            client->request(QStringLiteral("core.agentrun.salt"), {},
                            [](bool ok, const QJsonValue &result, const QString &) {
                                if (ok)
                                    forkmesh::localcontrol::attachments::setProcessSalt(
                                        QByteArray::fromHex(result.toObject()
                                                                .value(QStringLiteral("salt"))
                                                                .toString()
                                                                .toLatin1()));
                            });
        if (connected)
            reattachCoreAgentSessions();
    });
    if (m_coreClient->isConnected())
        forkmesh::coreagentstore::attach(
            forkmesh::core::readEndpoint(m_coreClient->stateDirectory()));
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            [] { AgentProcess::setDesktopQuitting(true); });
}

bool MainWindow::adoptCoreAgentAtStartup(AgentSession &session)
{
    if (session.status == AgentStatus::Running && coreAgentsWanted() &&
        QSettings().value(QStringLiteral("core/agentTerminalSession")).toInt() == session.id &&
        !CoreTerminalBackend::persistedTerminal(QStringLiteral("agent")).isEmpty()) {
        m_terminalSessionId = session.id;
        return true;
    }
    if (!m_coreAgentAdoption)
        m_coreAgentAdoption = std::make_shared<CoreAgentAdoption>();
    CoreAgentAdoption &adoption = *m_coreAgentAdoption;
    if (!adoption.scanned) {
        adoption.scanned = true;
        const bool wanted = coreAgentsWanted();
        const QList<forkmesh::coreagentproc::LiveProcess> found =
            forkmesh::coreagentproc::scan(forkmesh::core::defaultStateDirectory());
        for (const forkmesh::coreagentproc::LiveProcess &process : found) {
            const QString runtime = process.meta.value(QStringLiteral("runtime")).toString();
            const bool codexParsed =
                runtime == AgentRuntimeIds::CodexAppServer &&
                process.meta.value(QStringLiteral("parse")).toString() == QLatin1String("codex");
            const bool runnerParsed =
                runtime == AgentRuntimeIds::ManagedRunner &&
                process.meta.value(QStringLiteral("parse")).toString() == QLatin1String("log");
            if (runtime != AgentRuntimeIds::ClaudeStreamJson && !codexParsed && !runnerParsed)
                continue;
            if (!wanted) {
                forkmesh::coreagentproc::terminateShim(process);
                continue;
            }
            const int sid = process.meta.value(QStringLiteral("sessionId")).toString().toInt();
            if (sid <= 0) {
                adoption.rejected.append(process);
                continue;
            }
            const auto existing = adoption.live.constFind(sid);
            if (existing != adoption.live.constEnd()) {
                if (existing->createdMs >= process.createdMs) {
                    adoption.rejected.append(process);
                    continue;
                }
                adoption.rejected.append(*existing);
            }
            adoption.live.insert(sid, process);
        }
    }
    const auto it = adoption.live.constFind(session.id);
    if (it == adoption.live.constEnd())
        return false;
    if (!adoptableStatus(session)) {
        adoption.rejected.append(*it);
        adoption.live.remove(session.id);
        return false;
    }
    if (!session.inFlightSteerMessage.isEmpty()) {
        session.inFlightSteerMessage.clear();
        if (m_agentStore)
            m_agentStore->saveSession(session);
    }
    adoption.pending.insert(session.id);
    if (!adoption.fallbackArmed) {
        adoption.fallbackArmed = true;
        QTimer::singleShot(kCoreAgentAdoptionTimeoutMs, this,
                           [this] { fallBackFromCoreAgentAdoption(); });
    }
    return true;
}

void MainWindow::stopUnadoptedCoreAgent(const QString &key, bool running)
{
    if (!m_coreClient || key.isEmpty())
        return;
    if (running)
        m_coreClient->notify(QStringLiteral("core.agentproc.stop"),
                             {{QStringLiteral("key"), key},
                              {QStringLiteral("graceMs"), 1500},
                              {QStringLiteral("forget"), true}});
    else
        m_coreClient->notify(QStringLiteral("core.agentproc.forget"),
                             {{QStringLiteral("key"), key}});
}

void MainWindow::reattachCoreAgentSessions()
{
    if (!m_coreAgentAdoption || !m_coreClient || !m_coreClient->isConnected())
        return;
    const std::shared_ptr<CoreAgentAdoption> adoption = m_coreAgentAdoption;
    if ((adoption->pending.isEmpty() && adoption->rejected.isEmpty()) || adoption->listing)
        return;
    if (!m_coreClient->hasNativeRoute(QStringLiteral("core.agentproc.list"))) {
        fallBackFromCoreAgentAdoption();
        return;
    }
    adoption->listing = true;
    m_coreClient->request(
        QStringLiteral("core.agentproc.list"), {},
        [this, adoption](bool ok, const QJsonValue &result, const QString &) {
            adoption->listing = false;
            if (!ok || m_coreAgentAdoption != adoption)
                return;
            QHash<QString, QJsonObject> byKey;
            const QJsonArray processes =
                result.toObject().value(QStringLiteral("processes")).toArray();
            for (const QJsonValue &value : processes) {
                const QJsonObject process = value.toObject();
                byKey.insert(process.value(QStringLiteral("key")).toString(), process);
            }
            const QList<forkmesh::coreagentproc::LiveProcess> rejected =
                std::exchange(adoption->rejected, {});
            for (const forkmesh::coreagentproc::LiveProcess &process : rejected) {
                const QJsonObject info = byKey.value(process.key);
                if (!info.isEmpty())
                    stopUnadoptedCoreAgent(process.key,
                                           info.value(QStringLiteral("running")).toBool());
            }
            const QList<int> pending(adoption->pending.cbegin(), adoption->pending.cend());
            for (int sid : pending) {
                if (!adoption->pending.contains(sid))
                    continue;
                const QJsonObject info = byKey.value(adoption->live.value(sid).key);
                if (info.isEmpty()) {
                    adoption->pending.remove(sid);
                    requeueUnattachedCoreAgent(sid);
                    continue;
                }
                beginCoreAgentReattach(sid, info);
            }
        });
}

void MainWindow::beginCoreAgentReattach(int sessionId, const QJsonObject &info)
{
    CoreAgentAdoption &adoption = *m_coreAgentAdoption;
    adoption.pending.remove(sessionId);
    const QString key = info.value(QStringLiteral("key")).toString();
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || !adoptableStatus(*session) || m_streamSessions.contains(sessionId) ||
        m_codexStreams.contains(sessionId)) {
        stopUnadoptedCoreAgent(key, info.value(QStringLiteral("running")).toBool());
        return;
    }
    adoption.waiting.insert(sessionId, info);
    if (ensureStreamEventsLoadedAsync(sessionId) || !m_streamEventsLoading.contains(sessionId))
        finishCoreAgentReattach(sessionId);
}

void MainWindow::coreAgentEventsLoaded(int sessionId)
{
    if (m_coreAgentAdoption && m_coreAgentAdoption->waiting.contains(sessionId))
        finishCoreAgentReattach(sessionId);
}

void MainWindow::finishCoreAgentReattach(int sessionId)
{
    const QJsonObject info = m_coreAgentAdoption->waiting.take(sessionId);
    const QString key = info.value(QStringLiteral("key")).toString();
    const bool running = info.value(QStringLiteral("running")).toBool();
    AgentSession *session = findAgentSession(sessionId);
    if (!session || !adoptableStatus(*session) || m_streamSessions.contains(sessionId) ||
        m_codexStreams.contains(sessionId)) {
        stopUnadoptedCoreAgent(key, running);
        return;
    }
    const QJsonObject meta = info.value(QStringLiteral("meta")).toObject();
    if (meta.value(QStringLiteral("runtime")).toString() == AgentRuntimeIds::ManagedRunner) {
        finishCoreRunnerReattach(sessionId, key, meta,
                                 qint64(info.value(QStringLiteral("pid")).toDouble()));
        return;
    }
    const QJsonObject ack = info.value(QStringLiteral("ack")).toObject();
    qint64 stdoutOffset = qint64(ack.value(QStringLiteral("stdout")).toDouble());
    const qint64 stderrOffset = qint64(ack.value(QStringLiteral("stderr")).toDouble());
    const QList<QJsonObject> events = m_streamEvents.value(sessionId);
    for (const QJsonObject &event : events)
        if (event.value(QStringLiteral("_sk")).toString() == key)
            stdoutOffset = qMax(stdoutOffset,
                                qint64(event.value(QStringLiteral("_so")).toDouble()));
    const qint64 pid = qint64(info.value(QStringLiteral("pid")).toDouble());

    const QString accountId = meta.value(QStringLiteral("accountId")).toString();
    m_streamAccountId[sessionId] =
        accountId.isEmpty() ? activeAgentAccount(session->provider).id : accountId;
    const QString worktree = meta.value(QStringLiteral("worktree"))
                                 .toString(meta.value(QStringLiteral("cwd")).toString());
    if (!worktree.isEmpty())
        m_streamWorktree[sessionId] = worktree;
    const QString repoPath = meta.value(QStringLiteral("repoPath")).toString(worktree);
    const forkmesh::agentguard::Config guardrails =
        cliTranscriptGuardrails(*session, repoPath, nullptr);
    m_streamSessionInfo[sessionId] = *session;

    if (meta.value(QStringLiteral("runtime")).toString() == AgentRuntimeIds::CodexAppServer) {
        finishCoreCodexReattach(sessionId, key, meta, stdoutOffset, stderrOffset, pid, guardrails,
                                events);
        return;
    }

    auto *stream = new ClaudeStreamSession(this);
    m_streamSessions.insert(sessionId, stream);
    stream->setFinishBarrier([this](std::function<void()> completion) {
        if (!m_agentStore) {
            completion();
            return;
        }
        m_agentStore->whenTranscriptWritesDrained(this, std::move(completion));
    });
    const QStringList providerCredentialValues = localProviderCredentialValues();
    const int sid = sessionId;
    connect(stream, &AgentRuntimeSession::event, this,
            [this, sid, guardrails](const QJsonObject &ev) {
                handleCliRuntimeEvent(sid, ev, guardrails);
            });
    connect(stream, &AgentRuntimeSession::rawLine, this,
            [this, sid, providerCredentialValues](const QString &rawLine) {
                const QString line = redactProviderCredentials(rawLine, providerCredentialValues);
                noteAgentActivity(sid, line.size());
                QString &buf = m_streamRaw[sid];
                buf.append(line).append(QLatin1String("\n\n"));
                if (buf.size() > 400000)
                    buf.remove(0, buf.size() - 300000);
                if (sid == m_selectedAgentSessionId)
                    appendAgentRawLog(line + QStringLiteral("\n\n"));
            });
    connect(stream, &AgentRuntimeSession::stderrText, this,
            [this, sid, providerCredentialValues](const QString &rawText) {
                const QString text = redactProviderCredentials(rawText, providerCredentialValues);
                if (text.isEmpty())
                    return;
                noteAgentActivity(sid, text.size());
                QString &buf = m_streamRaw[sid];
                buf.append(QLatin1String("[stderr] ")).append(text);
                if (!buf.endsWith(QLatin1Char('\n')))
                    buf += QLatin1Char('\n');
                if (buf.size() > 400000)
                    buf.remove(0, buf.size() - 300000);
                if (sid == m_selectedAgentSessionId)
                    appendAgentRawLog(QStringLiteral("[stderr] ") + text);
            });
    connect(stream, &AgentRuntimeSession::finished, this,
            [this, sid](int exitCode) { finishCliTranscriptRun(sid, false, exitCode); });
    if (!stream->attachProcess(key, meta, stdoutOffset, stderrOffset, pid)) {
        m_streamSessions.remove(sessionId);
        stream->deleteLater();
        requeueUnattachedCoreAgent(sessionId);
        return;
    }
    logSystem(QStringLiteral("Agent #%1: re-attached to its Claude Code process (pid %2) "
                             "after the restart; output resumes at byte %3.")
                  .arg(sessionId)
                  .arg(pid)
                  .arg(stdoutOffset));
    if (session->status == AgentStatus::Running && turnEndedWithResult(events)) {
        m_agentCompletionChecks.insert(sessionId);
        completeAgentSessionWhenSubprocessesExit(sessionId);
    }
    updateAgentStatusCell(sessionId);
    if (sessionId == m_selectedAgentSessionId)
        showAgentSession(sessionId);
}

void MainWindow::requeueUnattachedCoreAgent(int sessionId)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session || session->status != AgentStatus::Running || !m_agentStore)
        return;
    session->status = AgentStatus::Queued;
    session->lastError.clear();
    session->finishedAtMs = 0;
    m_agentStore->saveSession(*session);
    m_agentStore->appendLog(*session,
                            QStringLiteral("\n==> Resuming after ForkMesh restart."));
    if (!m_agentQueue.contains(sessionId))
        m_agentQueue.append(sessionId);
    m_startupQuietAgentSessions.insert(sessionId);
    const bool wasQuiet = m_agentQuietResume;
    m_agentQuietResume = true;
    processAgentQueue();
    m_agentQuietResume = wasQuiet;
}

void MainWindow::fallBackFromCoreAgentAdoption()
{
    if (!m_coreAgentAdoption)
        return;
    CoreAgentAdoption &adoption = *m_coreAgentAdoption;
    const QList<forkmesh::coreagentproc::LiveProcess> rejected =
        std::exchange(adoption.rejected, {});
    if (!m_coreClient || !m_coreClient->isConnected())
        for (const forkmesh::coreagentproc::LiveProcess &process : rejected)
            forkmesh::coreagentproc::terminateShim(process);
    const QList<int> pending(adoption.pending.cbegin(), adoption.pending.cend());
    adoption.pending.clear();
    for (int sid : pending) {
        const forkmesh::coreagentproc::LiveProcess process = adoption.live.value(sid);
        logSystem(QStringLiteral("Agent #%1: forkmesh-core did not hand back its Claude Code "
                                 "process; stopping it and resuming the session instead.")
                      .arg(sid));
        forkmesh::coreagentproc::terminateShim(process);
        requeueUnattachedCoreAgent(sid);
    }
}

void MainWindow::finishCoreCodexReattach(int sessionId, const QString &key, const QJsonObject &meta,
                                         qint64 stdoutOffset, qint64 stderrOffset, qint64 pid,
                                         const forkmesh::agentguard::Config &guardrails,
                                         const QList<QJsonObject> &events)
{
    auto *stream = new CodexAppServerSession(this);
    m_codexStreams.insert(sessionId, stream);
    const QStringList providerCredentialValues = localProviderCredentialValues();
    const int sid = sessionId;
    connect(stream, &AgentRuntimeSession::event, this,
            [this, sid, guardrails](const QJsonObject &ev) {
                handleCliRuntimeEvent(sid, ev, guardrails);
            });
    connect(stream, &AgentRuntimeSession::rawLine, this,
            [this, sid, providerCredentialValues](const QString &rawLine) {
                const QString line = redactProviderCredentials(rawLine, providerCredentialValues);
                noteAgentActivity(sid, line.size());
                QString &buf = m_streamRaw[sid];
                buf.append(line).append(QLatin1String("\n\n"));
                if (buf.size() > 400000)
                    buf.remove(0, buf.size() - 300000);
                if (sid == m_selectedAgentSessionId)
                    appendAgentRawLog(line + QStringLiteral("\n\n"));
            });
    connect(stream, &AgentRuntimeSession::stderrText, this,
            [this, sid, providerCredentialValues](const QString &rawText) {
                const QString text = redactProviderCredentials(rawText, providerCredentialValues);
                if (text.isEmpty())
                    return;
                noteAgentActivity(sid, text.size());
                QString &buf = m_streamRaw[sid];
                buf.append(QLatin1String("[stderr] ")).append(text);
                if (!buf.endsWith(QLatin1Char('\n')))
                    buf += QLatin1Char('\n');
                if (buf.size() > 400000)
                    buf.remove(0, buf.size() - 300000);
                if (sid == m_selectedAgentSessionId)
                    appendAgentRawLog(QStringLiteral("[stderr] ") + text);
            });
    connect(stream, &AgentRuntimeSession::finished, this,
            [this, sid](int exitCode) { finishCliTranscriptRun(sid, true, exitCode); });
    connect(stream, &CodexAppServerSession::modelsListed, this,
            [this](const QJsonArray &models) { applyListedCodexModels(models); });
    if (!stream->attachProcess(key, meta, stdoutOffset, stderrOffset, pid)) {
        m_codexStreams.remove(sessionId);
        stream->deleteLater();
        requeueUnattachedCoreAgent(sessionId);
        return;
    }
    logSystem(QStringLiteral("Agent #%1: re-attached to its Codex app-server (pid %2) after the "
                             "restart; the core kept its JSON-RPC session.")
                  .arg(sessionId)
                  .arg(pid));
    AgentSession *session = findAgentSession(sessionId);
    if (session && session->status == AgentStatus::Running && turnEndedWithResult(events)) {
        m_agentCompletionChecks.insert(sessionId);
        completeAgentSessionWhenSubprocessesExit(sessionId);
    }
    updateAgentStatusCell(sessionId);
    if (sessionId == m_selectedAgentSessionId)
        showAgentSession(sessionId);
}

void MainWindow::resumeCoreAgentTerminal()
{
    const int sid = QSettings().value(QStringLiteral("core/agentTerminalSession")).toInt();
    if (!m_agentTerminal || sid <= 0 || m_agentTerminal->isRunning())
        return;
    m_terminalSessionId = sid;
    m_agentTerminal->resumeOr([this, sid] {
        QSettings().remove(QStringLiteral("core/agentTerminalSession"));
        if (m_terminalSessionId == sid)
            m_terminalSessionId = -1;
        requeueUnattachedCoreAgent(sid);
    });
}

#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentIntake.h"
#include "AgentWindows.h"
#include "AgentWorktree.h"
#include "LocalControlHandoff.h"
#include "VirtualMachineRuntime.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>

using namespace forkmesh::ui;

namespace intake = forkmesh::agentintake;

namespace {

QString intakeErrorMessage(const QString &code)
{
    if (code == QLatin1String("bad_repository"))
        return QStringLiteral("repository must be owner/name.");
    if (code == QLatin1String("bad_task"))
        return QStringLiteral("prompt is required and must be at most %1 characters.")
            .arg(intake::kMaxPromptChars);
    if (code == QLatin1String("bad_title"))
        return QStringLiteral("title must be at most %1 characters.")
            .arg(intake::kMaxTitleChars);
    if (code == QLatin1String("bad_branch"))
        return QStringLiteral("branch is not a valid branch name.");
    if (code == QLatin1String("bad_worktree"))
        return QStringLiteral("worktree must be a clean absolute path.");
    if (code == QLatin1String("bad_provider"))
        return QStringLiteral("provider is not an agent provider this desktop knows.");
    if (code == QLatin1String("bad_model"))
        return QStringLiteral("model is not a valid model id.");
    if (code == QLatin1String("bad_priority"))
        return QStringLiteral("priority must be a whole number from 1 to 99.");
    if (code == QLatin1String("bad_task_id"))
        return QStringLiteral("task must be a 32-character hex organization task id.");
    return QStringLiteral("The request is malformed.");
}

bool protectedBranch(const QString &branch, const QString &checkoutBranch)
{
    static const QStringList reserved{QStringLiteral("main"), QStringLiteral("master"),
                                      QStringLiteral("HEAD"), QStringLiteral("trunk")};
    return reserved.contains(branch) || branch.startsWith(QLatin1String("refs/")) ||
           branch.startsWith(QLatin1String("release/")) ||
           (!checkoutBranch.isEmpty() && branch == checkoutBranch);
}

}

void MainWindow::ensureAgentIntakeServer()
{
    if (m_agentIntake || m_agentIntakeFailed || forkmesh::vm::active())
        return;
    auto server = std::make_unique<intake::Server>();
    QString error;
    if (!server->listen(&error)) {
        m_agentIntakeFailed = true;
        qWarning().noquote() << "agent intake socket unavailable:" << error;
        return;
    }
    server->setAsyncHandler([this](const intake::Request &request,
                                   intake::Server::Reply reply) {
        const int requester =
            intake::authorizedSession(m_agentIntakeTokens, request.token);
        if (requester <= 0) {
            const QString terminal = terminalCallerForToken(request.token);
            if (!terminal.isEmpty() &&
                request.method.startsWith(forkmesh::terminalbridge::MethodPrefix)) {
                reply(handleTerminalRequest(0, terminal, request.method, request.params));
                return;
            }
            reply(intake::unauthorizedReply(request.method));
            return;
        }
        if (request.method.startsWith(forkmesh::terminalbridge::MethodPrefix)) {
            reply(handleTerminalRequest(requester, QString(), request.method, request.params));
            return;
        }
        if (request.method.startsWith(intake::ReleaseMethodPrefix)) {
            handleAgentReleaseRequest(requester, request, std::move(reply));
            return;
        }
        if (request.method == forkmesh::agentwindows::CaptureMethod) {
            agentWindows()->capture(requester, request.params, std::move(reply));
            return;
        }
        if (request.method == forkmesh::agentwindows::ActMethod) {
            agentWindows()->act(requester, request.params, std::move(reply));
            return;
        }
        if (request.method == forkmesh::agenthub::RunnersMethod) {
            listRunnersForAgent(requester, std::move(reply));
            return;
        }
        reply(handleAgentIntakeRequest(request));
    });
    m_agentIntake = std::move(server);
}

QStringList MainWindow::agentIntakeEnv(int sessionId)
{
    const QStringList cleared{intake::SocketEnv, intake::TokenEnv};
    if (sessionId <= 0 || forkmesh::vm::active())
        return cleared;
    ensureAgentIntakeServer();
    if (!m_agentIntake)
        return cleared;
    QString &token = m_agentIntakeTokens[sessionId];
    if (token.isEmpty())
        token = intake::mintToken(sessionId);
    return {QStringLiteral("%1=%2").arg(intake::SocketEnv, m_agentIntake->socketPath()),
            QStringLiteral("%1=%2").arg(intake::TokenEnv, token)};
}

void MainWindow::addAgentRunMcpConfig(int sessionId, QJsonObject &options)
{
    const QString token = m_agentIntakeTokens.value(sessionId);
    if (sessionId <= 0 || forkmesh::vm::active() || token.isEmpty() || !m_agentIntake)
        return;
    const QString config = writeForkmeshMcpConfig(
        QStringLiteral("agent-mcp"), QStringLiteral("session-%1.json").arg(sessionId),
        {{intake::SocketEnv, m_agentIntake->socketPath()}, {intake::TokenEnv, token}});
    if (config.isEmpty())
        return;
    options.insert(QStringLiteral("mcpConfig"), config);
    options.insert(QStringLiteral("mcpReadOnly"),
                   QJsonArray{config, QFileInfo(mcpServerScriptPath()).absolutePath()});
}

QJsonObject MainWindow::handleAgentIntakeRequest(const intake::Request &request)
{
    const int requester = intake::authorizedSession(m_agentIntakeTokens, request.token);
    if (requester <= 0)
        return intake::unauthorizedReply(request.method);
    if (request.method.startsWith(forkmesh::agentwindows::MethodPrefix))
        return handleAgentWindowRequest(requester, request);
    if (request.method == forkmesh::agenthub::SuggestMethod ||
        request.method == forkmesh::agenthub::ListMethod ||
        request.method == forkmesh::agenthub::MessageMethod ||
        request.method == forkmesh::agenthub::NotifyMethod)
        return handleAgentHubRequest(requester, request);
    if (request.method != intake::CreateMethod)
        return intake::errorReply(QStringLiteral("unknown_method"),
                                  QStringLiteral("Unknown method."));
    intake::CreateRequest create;
    QString code;
    if (!intake::parseCreateRequest(request.params, &create, &code))
        return intake::errorReply(code, intakeErrorMessage(code));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_agentIntakeLimiter.used(now) >= intake::kRateLimitCount)
        return intake::errorReply(
            QStringLiteral("rate_limited"),
            QStringLiteral("Agents may create at most %1 agents every %2 minutes on "
                           "this desktop. Try again later.")
                .arg(intake::kRateLimitCount)
                .arg(intake::kRateWindowMs / 60000));
    QJsonObject reply = createAgentFromIntake(requester, create);
    if (reply.value(QStringLiteral("ok")).toBool())
        m_agentIntakeLimiter.allow(now);
    return reply;
}

QJsonObject MainWindow::handleAgentWindowRequest(int sessionId,
                                                 const intake::Request &request)
{
    return agentWindows()->handle(sessionId, agentWindowLabel(sessionId),
                                  sessionWorkdir(sessionId), request.method, request.params);
}

QJsonObject MainWindow::createAgentFromIntake(int requesterId,
                                              const intake::CreateRequest &request)
{
    int repoIndex = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        if (r.owner.compare(request.owner, Qt::CaseInsensitive) == 0 &&
            r.name.compare(request.name, Qt::CaseInsensitive) == 0 &&
            !r.previewOnly && !r.localPath.trimmed().isEmpty()) {
            repoIndex = i;
            break;
        }
    }
    if (repoIndex < 0)
        return intake::errorReply(
            QStringLiteral("no_checkout"),
            QStringLiteral("%1/%2 is not a repository this desktop has checked out.")
                .arg(request.owner, request.name));
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    const QString slug = repo.owner + QLatin1Char('/') + repo.name;

    QString provider = defaultAgentProvider();
    if (!request.provider.isEmpty()) {
        if (!agentProviderIsKnown(request.provider))
            return intake::errorReply(QStringLiteral("bad_provider"),
                                      intakeErrorMessage(QStringLiteral("bad_provider")));
        provider = request.provider;
    }
    QString model = request.model;
    if (model.isEmpty() && provider == QLatin1String("claude-code"))
        model = QSettings().value(kClaudeCodeModelSetting).toString();

    QString branch = request.branch;
    const RepositoryRecord local = writableRecordFor(repo);
    const QString gitDir = local.localPath.trimmed();
    if (!request.worktree.isEmpty() || !branch.isEmpty()) {
        if (gitDir.isEmpty())
            return intake::errorReply(
                QStringLiteral("read_only"),
                QStringLiteral("%1 is read-only on this desktop, so an agent cannot "
                               "work on its branches here.")
                    .arg(slug));
    }
    if (!request.worktree.isEmpty()) {
        const QFileInfo info(request.worktree);
        const QString canonical = info.canonicalFilePath();
        if (!info.isDir() || canonical.isEmpty())
            return intake::errorReply(QStringLiteral("bad_worktree"),
                                      QStringLiteral("worktree is not a directory."));
        const QString head = worktreeHeadBranch(canonical);
        if (head.isEmpty())
            return intake::errorReply(
                QStringLiteral("bad_worktree"),
                QStringLiteral("worktree has no branch checked out."));
        if (branch.isEmpty())
            branch = head;
        else if (head != branch)
            return intake::errorReply(
                QStringLiteral("worktree_mismatch"),
                QStringLiteral("worktree has %1 checked out, not %2.").arg(head, branch));
        const QString holder = worktreePathForBranch(gitDir, branch);
        if (holder.isEmpty() || QFileInfo(holder).canonicalFilePath() != canonical)
            return intake::errorReply(
                QStringLiteral("worktree_mismatch"),
                QStringLiteral("worktree is not a worktree of %1.").arg(slug));
        const QString root = forkmesh::vm::active() ? forkmesh::vm::worktreeRoot()
                                                    : forkmesh::agentwt::root(gitDir);
        if (QFileInfo(canonical).absolutePath() != QFileInfo(root).canonicalFilePath())
            return intake::errorReply(
                QStringLiteral("worktree_not_agent"),
                QStringLiteral("Only ForkMesh agent worktrees under %1 can be reused; "
                               "pass branch alone to start a fresh worktree.")
                    .arg(root));
    }

    const QString requester = QStringLiteral("Agent #%1").arg(requesterId);
    QString taskId = request.task;
    if (request.handoff) {
        const AgentSession *caller = findAgentSession(requesterId);
        const QString own =
            caller ? caller->orgTaskId.trimmed().toLower() : QString();
        if (own.isEmpty())
            return intake::errorReply(
                QStringLiteral("no_task"),
                QStringLiteral("%1 is not linked to an organization task, so it has "
                               "none to hand off; pass task to link the new agent "
                               "to one.")
                    .arg(requester));
        if (!taskId.isEmpty() && taskId != own)
            return intake::errorReply(
                QStringLiteral("task_mismatch"),
                QStringLiteral("%1 is linked to task %2, not %3; handoff hands over "
                               "the calling agent's own task.")
                    .arg(requester, own, taskId));
        taskId = own;
    }
    int id = 0;
    if (!branch.isEmpty()) {
        if (!forkmesh::localcontrol::handoffRefNameValid(branch))
            return intake::errorReply(QStringLiteral("bad_branch"),
                                      intakeErrorMessage(QStringLiteral("bad_branch")));
        if (protectedBranch(branch, worktreeHeadBranch(gitDir)))
            return intake::errorReply(
                QStringLiteral("protected_branch"),
                QStringLiteral("An agent cannot be put to work on %1; name an agent "
                               "or feature branch.")
                    .arg(branch));
        if (!fetchServedBranch(local, branch, false))
            return intake::errorReply(
                QStringLiteral("branch_not_found"),
                QStringLiteral("Branch %1 is not in %2 on this desktop.").arg(branch, slug));
        const QString title = request.title.isEmpty()
                                  ? forkmesh::agentwt::promptTitle(request.prompt)
                                  : request.title;
        id = startAgentOnExistingBranch(repoIndex, branch, QString(), 0, title,
                                        request.prompt, provider,
                                        codexChatGptModelId(model), request.paused);
    } else {
        id = startAdHocAgentForRepo(repoIndex, request.prompt, provider,
                                    false, model, request.title,
                                    false, false, taskId,
                                    QString(), QString(),
                                    request.hasPriority ? request.priority : 50,
                                    false, QString(), false,
                                    request.paused);
    }
    if (id <= 0)
        return intake::errorReply(
            QStringLiteral("not_started"),
            QStringLiteral("The desktop could not create the agent; its Agents page "
                           "shows why."));

    if (AgentSession *session = findAgentSession(id); session && m_agentStore) {
        if (request.hasPriority)
            session->priority = request.priority;
        if (session->held)
            session->heldBy = requester;
        m_agentStore->saveSession(*session);
        m_agentStore->appendLog(
            *session,
            session->held
                ? QStringLiteral("==> Created by %1 through the ForkMesh connector; "
                                 "paused until you start it.\n")
                      .arg(requester)
                : QStringLiteral("==> Created by %1 through the ForkMesh connector.\n")
                      .arg(requester));
    }
    if (!taskId.isEmpty()) {
        if (AgentSession *session = findAgentSession(id);
            session && session->orgTaskId.isEmpty()) {
            session->orgTask = true;
            session->orgTaskAttached = false;
            recordOrgTaskFields(id, taskId, QString(), false);
        }
        if (AgentSession *session = findAgentSession(id); session && m_agentStore)
            m_agentStore->appendLog(
                *session,
                request.handoff
                    ? QStringLiteral("==> %1 handed over organization task %2.\n")
                          .arg(requester, taskId)
                    : QStringLiteral("==> Linked to organization task %1.\n")
                          .arg(taskId));
    }
    if (request.handoff) {
        unlinkAgentSessionFromOrgTask(requesterId);
        if (AgentSession *caller = findAgentSession(requesterId); caller && m_agentStore)
            m_agentStore->appendLog(
                *caller,
                QStringLiteral("==> Handed organization task %1 to agent #%2.\n")
                    .arg(taskId)
                    .arg(id));
        refreshAgentTaskLinkUi(requesterId);
        if (m_organizationTasksLoaded)
            refreshOrganizationTaskAgentRows();
    }
    reloadAgents();
    notifyLocalControlAgentsChanged();
    const QString started =
        request.handoff
            ? QStringLiteral("%1 handed task %2 to %3 agent #%4 for %5.")
                  .arg(requester, taskId,
                       request.paused ? QStringLiteral("paused") : QStringLiteral("new"))
                  .arg(id)
                  .arg(slug)
        : request.paused
            ? QStringLiteral("%1 added a paused agent for %2. Start it from "
                             "the Agents list.")
                  .arg(requester, slug)
            : QStringLiteral("%1 started an agent for %2.").arg(requester, slug);
    if (QSettings().value(kAgentStartPingSetting, true).toBool())
        flashMessage(started);
    else
        logSystem(started);

    const AgentSession *created = findAgentSession(id);
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("id"), id},
        {QStringLiteral("state"), agentIntakeSessionState(id)},
        {QStringLiteral("paused"), created && created->held},
        {QStringLiteral("repository"), slug},
        {QStringLiteral("branch"), created ? created->branchName : branch},
        {QStringLiteral("title"), created ? created->issueTitle : request.title},
        {QStringLiteral("provider"), provider},
        {QStringLiteral("model"), created ? created->model : model},
        {QStringLiteral("priority"), created ? created->priority : request.priority},
        {QStringLiteral("task"), created ? created->orgTaskId : taskId},
        {QStringLiteral("handedOff"), request.handoff},
    };
}

QString MainWindow::agentIntakeSessionState(int sessionId) const
{
    const AgentSession *session = nullptr;
    for (const AgentSession &candidate : m_agentSessions)
        if (candidate.id == sessionId) {
            session = &candidate;
            break;
        }
    if (!session)
        return QStringLiteral("unknown");
    if (session->awaitingStart())
        return QStringLiteral("paused");
    if (session->status == AgentStatus::Queued)
        return m_agentQueue.contains(sessionId) ? QStringLiteral("queued")
                                                : QStringLiteral("starting");
    return session->status;
}

bool MainWindow::startHeldAgentSession(int sessionId)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session || !session->awaitingStart() || !m_agentStore)
        return false;
    const QString heldBy = session->heldBy;
    session->held = false;
    session->heldBy.clear();
    if (!m_agentStore->saveSession(*session)) {
        session->held = true;
        session->heldBy = heldBy;
        flashMessage(QStringLiteral("Could not start agent #%1: its record could not "
                                    "be saved.")
                         .arg(sessionId),
                     true);
        return false;
    }
    m_agentStore->appendLog(*session, QStringLiteral("\n==> Started by you.\n"));
    if (!m_agentQueue.contains(sessionId))
        m_agentQueue.append(sessionId);
    const bool full = runningAgentCount() >= maxRunningAgents();
    reloadAgents();
    processAgentQueue();
    if (full)
        flashMessage(
            QString::fromUtf8("Queued \xE2\x80\x94 %1 agents are already running "
                              "(limit set in Settings).")
                .arg(maxRunningAgents()));
    if (sessionId == m_selectedAgentSessionId)
        showAgentSession(sessionId);
    updateAgentActionState();
    notifyLocalControlAgentsChanged();
    return true;
}


#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AccountCapability.h"
#include "ForkMeshPackageRegistry.h"
#include "RepoStatsStore.h"
#include "AgentCliLocator.h"
#include "AgentJail.h"
#include "AgentScratch.h"
#include "AgentCapabilityBroker.h"
#include "AgentCapabilityStore.h"
#include "AgentPermissionController.h"
#include "AgentPromptImages.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyBoard.h"
#include "AgentGuardrailBoard.h"
#include "AgentToolPolicy.h"
#include "AgentRuntimeRegistry.h"
#include "AgentResumeIdentity.h"
#include "AgentWorktree.h"
#include "SafeDelete.h"
#include "FlowLayout.h"
#include "KebabHeaderView.h"
#include "CodexAppServerSession.h"
#include "NodeEventSocket.h"
#include "UsageLimitCalendar.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDebug>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextLayout>
#include <QTextOption>
#include <QUrl>
#include <QEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QButtonGroup>
#include <QFrame>
#include <QShowEvent>
#include <QSignalBlocker>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <csignal>
#endif
#include "MainWindowAgentsInternal.h"

using namespace forkmesh::ui;

namespace {

struct AgentPullRead {
    bool ok = false;
    bool answered = false;
    QByteArray output;
    QString error;
};

AgentPullRead agentPullGit(const QString &workdir, const QStringList &args)
{
    WorkerProcessRequest request;
    request.program = QStringLiteral("git");
    request.arguments = QStringList{QStringLiteral("-C"), workdir} + args;
    request.timeoutMs = kAgentPullGitTimeoutMs;
    request.activityKind = QStringLiteral("git");
    request.activityDetail =
        gitArgsCrumb(QStringLiteral("git"), request.arguments);
    request.activityRoutine = forkmesh::BackgroundRoutine::current();
    const WorkerProcessResult result = runWorkerProcess(request);
    AgentPullRead read;
    read.ok = result.ok;
    read.answered = result.started && !result.timedOut && !result.cancelled &&
                    result.exitStatus == QProcess::NormalExit;
    read.output = result.output;
    if (!result.ok)
        read.error = QStringLiteral("git %1 %2")
                         .arg(args.value(0), workerProcessOutcome(result));
    return read;
}

}

AgentPullCapture forkmesh::ui::captureAgentPull(const QString &workdir,
                                               const QString &baseRef,
                                               const QString &baseBranch)
{
    AgentPullCapture capture;
    QString base = baseRef;
    if (!baseBranch.isEmpty() && baseBranch != baseRef) {
        const AgentPullRead mergeBase = agentPullGit(
            workdir, {QStringLiteral("merge-base"), QStringLiteral("HEAD"),
                      baseBranch});
        if (!mergeBase.answered) {
            capture.failure =
                QStringLiteral("finding where the branch left %1 failed (%2)")
                    .arg(baseBranch, mergeBase.error);
            return capture;
        }
        const QString found = QString::fromUtf8(mergeBase.output).trimmed();
        if (mergeBase.ok && !found.isEmpty()) {
            const AgentPullRead ancestry = agentPullGit(
                workdir, {QStringLiteral("merge-base"),
                          QStringLiteral("--is-ancestor"), baseRef, found});
            if (!ancestry.answered) {
                capture.failure =
                    QStringLiteral("checking %1 against %2 failed (%3)")
                        .arg(baseRef, baseBranch, ancestry.error);
                return capture;
            }
            if (ancestry.ok)
                base = found;
        }
    }
    const AgentPullRead diff = agentPullGit(
        workdir, {QStringLiteral("diff"), QStringLiteral("--binary"), base});
    if (!diff.ok) {
        capture.failure =
            QStringLiteral("reading the changes in %1 failed (%2)")
                .arg(workdir, diff.error);
        return capture;
    }
    capture.patch = QString::fromUtf8(diff.output);
    if (capture.patch.trimmed().isEmpty())
        return capture;
    const AgentPullRead commits = agentPullGit(
        workdir,
        {QStringLiteral("format-patch"), QStringLiteral("--stdout"), base});
    if (commits.ok)
        capture.commits = QString::fromUtf8(commits.output);
    return capture;
}

void MainWindow::maybeCreatePullForStreamSession(int sessionId,
                                                 std::function<void()> then)
{
    m_agentPullFailures.remove(sessionId);
    m_agentPullNotNeeded.remove(sessionId);
    AgentSession *s = findAgentSession(sessionId);
    const auto refuse = [this, sessionId, &then](const QString &reason) {
        if (!reason.isEmpty())
            m_agentPullFailures.insert(sessionId, reason);
        if (then)
            then();
    };
    if (!s || !s->createPr || s->prNumber > 0 || !m_agentStore) {
        refuse(QString());
        return;
    }
    if (s->baseRef.isEmpty()) {
        refuse(QStringLiteral("the agent has no recorded base commit"));
        return;
    }
    if (repoIndexFor(s->owner, s->name) < 0) {
        refuse(QStringLiteral("%1/%2 is no longer in this app")
                   .arg(s->owner, s->name));
        return;
    }
    if (auto waiting = m_agentPullCaptures.find(sessionId);
        waiting != m_agentPullCaptures.end()) {
        if (then)
            waiting->append(std::move(then));
        return;
    }
    const AgentSession session = *s;
    QList<std::function<void()>> waiters;
    if (then)
        waiters.append(std::move(then));
    m_agentPullCaptures.insert(sessionId, waiters);
    const QString workdir = sessionWorkdir(sessionId);
    runOffThread<AgentPullCapture>(
        [workdir, baseRef = session.baseRef, baseBranch = session.baseBranch] {
            return captureAgentPull(workdir, baseRef, baseBranch);
        },
        [this, sessionId, session](AgentPullCapture capture) {
            const QList<std::function<void()>> waiting =
                m_agentPullCaptures.take(sessionId);
            const auto resume = qScopeGuard([waiting] {
                for (const std::function<void()> &next : waiting) {
                    if (next)
                        next();
                }
            });
            if (!m_agentStore)
                return;
            if (!capture.failure.isEmpty()) {
                m_agentPullFailures.insert(sessionId, capture.failure);
                m_agentStore->appendLog(
                    session,
                    QStringLiteral("!! Could not create pull request: %1.\n")
                        .arg(capture.failure));
                return;
            }
            if (capture.patch.trimmed().isEmpty()) {
                m_agentPullNotNeeded.insert(sessionId);
                m_agentPullFailures.insert(
                    sessionId, QStringLiteral("the branch has no changes"));
                m_agentStore->appendLog(
                    session,
                    QStringLiteral("==> No code changes; no pull request created.\n"));
                return;
            }
            if (const AgentSession *live = findAgentSession(sessionId);
                live && live->prNumber > 0)
                return;
            m_agentStore->writePatch(session, capture.patch);
            landAgentPullForSession(session, capture.patch, capture.commits);
        });
}

// `session` is taken by value: PullStore::createPull (and submitPullToInbox)
// pump the GUI event loop while waiting on git, and a reloadAgents() fired
// during the pump rebuilds m_agentSessions — a reference into it would dangle
// (git-pump UAF family).
void MainWindow::landAgentPullForSession(AgentSession session, const QString &patch,
                                         const QString &commits)
{
    if (!m_agentStore || patch.trimmed().isEmpty())
        return;
    const int ri = repoIndexFor(session.owner, session.name);
    if (ri < 0)
        return;
    const RepositoryRecord repo = m_repositories.at(ri);
    const QString prTitle =
        session.issueNumber > 0
            ? QStringLiteral("Agent: issue #%1 %2").arg(session.issueNumber).arg(session.issueTitle)
            : QStringLiteral("Agent: %1").arg(session.issueTitle);
    const QString prBody =
        session.issueNumber > 0
            ? QStringLiteral("Created from a %1 session for issue #%2.")
                  .arg(agentProviderName(session.provider))
                  .arg(session.issueNumber)
            : QStringLiteral("Created from a %1 agent session.")
                  .arg(agentProviderName(session.provider));
    const QString base = session.baseBranch.isEmpty() ? session.baseRef : session.baseBranch;
    PullStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity, m_userName);
    if (store.canWrite()) {
        QString error;
        const int pr = store.createPull(prTitle, prBody, base, session.branchName, patch,
                                        commits, /*branchBacked=*/true, &error);
        if (pr > 0) {
            session.prNumber = pr;
            if (AgentSession *live = findAgentSession(session.id))
                live->prNumber = pr;
            m_agentStore->saveSession(session);
            m_agentStore->appendLog(
                session, QStringLiteral("==> Created pull request #%1.\n").arg(pr));
            linkAgentPullToIssue(session, pr); // record it in the issue's Development section
            publishAgentPullRunnerRef(session, repo, pr);
        } else {
            m_agentPullFailures.insert(
                session.id, error.trimmed().isEmpty()
                                ? QStringLiteral("the pull request store refused it")
                                : error.trimmed());
            m_agentStore->appendLog(
                session, QStringLiteral("!! Could not create pull request: %1\n").arg(error));
        }
        return;
    }
    PullRequest pr;
    pr.title = prTitle;
    pr.description = prBody;
    pr.base = base;
    pr.head = session.branchName;
    pr.patch = patch;
    pr.commits = commits;
    const QString nodeName = accountNameFromInput(m_userName, QString());
    if (!nodeName.isEmpty() && !pr.head.contains(QLatin1Char(':')))
        pr.head = nodeName + QLatin1Char(':') + pr.head;
    store.setSigningRepository(repo.owner, repo.name);
    submitPullToInbox(store.makeSignedPull(pr), repo, /*quiet=*/true);
    m_agentPullFailures.insert(
        session.id,
        QStringLiteral("this node cannot write %1/%2's pull requests, so it was "
                       "sent to the owner's inbox instead")
            .arg(repo.owner, repo.name));
    m_agentStore->appendLog(
        session, QStringLiteral("==> Sent pull request to %1/%2 (source of truth).\n")
                     .arg(repo.owner, repo.name));
}

void MainWindow::publishAgentPullRunnerRef(const AgentSession &session,
                                           const RepositoryRecord &repo, int pr)
{
    const QString gitDir = repo.localPath.trimmed();
    const QString prRef = QStringLiteral("refs/pr/%1/head").arg(pr);
    const QString runnerRef = QStringLiteral("refs/heads/pr/%1").arg(pr);
    const QString owner = repo.owner;
    const QString name = repo.name;
    const QString mirrorPath = repo.mirrorPath.trimmed();
    const auto reloadIfOpen = [this, owner, name] {
        const int ri = repoIndexFor(owner, name);
        if (ri >= 0 && ri == m_repoDetailIndex)
            reloadPulls();
    };
    runOffThread<QString>(
        [gitDir, prRef, runnerRef] {
            QByteArray tip;
            QByteArray runnerTip;
            if (gitDir.isEmpty() ||
                !runGitCapture(gitDir, {QStringLiteral("rev-parse"), prRef}, &tip,
                               nullptr) ||
                tip.trimmed().isEmpty() ||
                !runGitCapture(gitDir, {QStringLiteral("rev-parse"), runnerRef},
                               &runnerTip, nullptr) ||
                runnerTip.trimmed() != tip.trimmed())
                return QString();
            return QString::fromUtf8(tip).trimmed();
        },
        [this, session, pr, gitDir, runnerRef, owner, name, mirrorPath,
         reloadIfOpen](QString commit) {
            if (!m_agentStore)
                return;
            if (commit.isEmpty()) {
                m_agentStore->appendLog(
                    session,
                    QStringLiteral("!! Could not resolve pull request #%1 to run checks.\n")
                        .arg(pr));
                reloadIfOpen();
                return;
            }
            const QString explicitKey = owner + QLatin1Char('\x1f') + name +
                                        QLatin1Char('\x1f') + commit.toLower();
            m_explicitActionPushes.insert(explicitKey);
            runOffThread<QStringList>(
                [gitDir, mirrorPath, runnerRef] {
                    if (mirrorPath.isEmpty())
                        return QStringList{QStringLiteral("\x1f")};
                    QByteArray previous;
                    const bool hadRef = runGitCapture(
                        mirrorPath,
                        {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                         QStringLiteral("--quiet"),
                         runnerRef + QStringLiteral("^{commit}")},
                        &previous, nullptr);
                    QString pushError;
                    if (runGitCapture(gitDir,
                                      {QStringLiteral("push"), mirrorPath,
                                       runnerRef + QLatin1Char(':') + runnerRef},
                                      nullptr, &pushError))
                        return QStringList{
                            QString(),
                            hadRef ? QString::fromUtf8(previous).trimmed()
                                   : QString()};
                    return QStringList{QStringLiteral("\x1f") + pushError};
                },
                [this, session, pr, commit, runnerRef, owner, name, explicitKey,
                 reloadIfOpen](QStringList outcome) {
                    if (!m_agentStore)
                        return;
                    const QString failure = outcome.value(0);
                    const QString before = outcome.value(1);
                    const int ri = repoIndexFor(owner, name);
                    if (failure.isEmpty() && ri >= 0) {
                        queueWorkflowsForCommit(ri, owner, name, commit, runnerRef,
                                                WorkflowTrigger::Push,
                                                ActionPullContext(), before);
                        QTimer::singleShot(60000, this, [this, explicitKey] {
                            m_explicitActionPushes.remove(explicitKey);
                        });
                        m_agentStore->appendLog(
                            session,
                            QStringLiteral("==> Queued repository checks for pull "
                                           "request #%1.\n")
                                .arg(pr));
                    } else if (failure.isEmpty()) {
                        m_explicitActionPushes.remove(explicitKey);
                        m_agentStore->appendLog(
                            session,
                            QStringLiteral("!! Published pull request #%1's test ref, "
                                           "but %2/%3 was removed from this app before "
                                           "its checks could be queued.\n")
                                .arg(pr)
                                .arg(owner, name));
                    } else {
                        m_explicitActionPushes.remove(explicitKey);
                        const QString pushError = failure.mid(1).trimmed();
                        const QString detail =
                            pushError.isEmpty()
                                ? QStringLiteral("the served mirror is unavailable")
                                : pushError.right(240);
                        m_agentStore->appendLog(
                            session,
                            QStringLiteral("!! Could not publish pull request #%1's "
                                           "test ref to the action runner: %2\n")
                                .arg(pr)
                                .arg(detail));
                    }
                    reloadIfOpen();
                });
        });
}

// "YOLO" auto-merge: a session launched with the quick-add YOLO
// toggle lands its branch in the repo's default branch as soon as its run
// finishes cleanly — what "Merge into main" does, without waiting for a click.
// Every safety gate lives in mergeWorktreeIntoMain (dirty checkout, wrong branch,
// conflicts, the is-ancestor proof before any delete), so a merge that can't land
// leaves the branch and worktree where they are.
// ---- Organization tasks for prompted runs ---------------------
// With the composer's "Task" toggle on, a prompted run also opens a task in the
// organization so it shows up for everyone, not just this app's Agents tab. The
// task records provenance: the bots that launched and reported it, and the model,
// permission mode and reasoning strength it was given.

QString MainWindow::agentBotLabel(const QString &provider) const
{
    QString bot = provider.trimmed().toLower();
    if (bot.isEmpty())
        bot = QStringLiteral("agent");
    const QString machine = machineNodeName().trimmed().toLower();
    if (machine.isEmpty())
        return bot;
    return bot + QLatin1Char('@') + machine;
}

QString MainWindow::composerAgentStrength() const
{
    return QSettings()
        .value(kClaudeEffortSetting, QStringLiteral("high"))
        .toString()
        .trimmed()
        .toLower();
}

// Authenticate one organization-task write. A desktop that signed in with a
// password holds an account session token; the ordinary launch path is
// authenticateSilently(), which proves this install owns the account's key and
// produces no token at all — so without the signed fallback the Task toggle
// would silently do nothing for most users. `resource` is the task id for a
// proof that names one (empty for the collection). Returns false when this node
// can neither present a session nor sign for the account.
bool MainWindow::authenticateOrgTaskRequest(QUrl &url, QNetworkRequest &request,
                                            const QString &proofPrefix,
                                            const QString &resource) const
{
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (!sessionToken.isEmpty()) {
        request.setUrl(url);
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") +
                                 sessionToken.toUtf8());
        return true;
    }
    QString node, ts, sig;
    if (!signOrgTaskProof(proofPrefix, resource, &node, &ts, &sig))
        return false;
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("node"), node);
    query.addQueryItem(QStringLiteral("ts"), ts);
    query.addQueryItem(QStringLiteral("sig"), sig);
    url.setQuery(query);
    request.setUrl(url);
    return true;
}

bool MainWindow::signOrgTaskProof(const QString &proofPrefix,
                                  const QString &resource, QString *node,
                                  QString *ts, QString *sig) const
{
    const QString signer = accountOwner().trimmed().toLower();
    if (signer.isEmpty() || !hasOwnerSigningCapability(signer))
        return false;
    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QString canonical =
        proofPrefix + QLatin1Char('\n') + signer + QLatin1Char('\n');
    if (!resource.isEmpty())
        canonical += resource + QLatin1Char('\n');
    canonical += stamp;
    const QString signature = m_profileIdentity.signData(canonical.toUtf8());
    if (signature.isEmpty())
        return false;
    if (node)
        *node = signer;
    if (ts)
        *ts = stamp;
    if (sig)
        *sig = signature;
    return true;
}

void MainWindow::recordOrgTaskFields(int sessionId, const QString &taskId,
                                     const QString &finishedByBot,
                                     bool completeIfFinished)
{
    // Re-look-up rather than capturing the session: a network reply lands after
    // event-loop turns that can have rebuilt m_agentSessions (git-pump UAF
    // family).
    AgentSession *s = findAgentSession(sessionId);
    if (!s || !m_agentStore)
        return;
    if (!taskId.isEmpty())
        s->orgTaskId = taskId;
    if (!finishedByBot.isEmpty())
        s->finishedByBot = finishedByBot;
    m_agentStore->saveSession(*s);
    if (!taskId.isEmpty()) {
        syncOrgTaskAgentStatus(sessionId);
        refreshAgentTaskLinkUi(sessionId);
        attachOrgTaskWorkItemForSession(sessionId);
        s = findAgentSession(sessionId);
        if (completeIfFinished && s && AgentStatus::isTerminal(s->status))
            completeOrgTaskForSession(sessionId);
    }
}

void MainWindow::refreshAgentTaskLinkUi(int sessionId)
{
    if (m_selectedAgentSessionId != sessionId)
        return;
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    refreshAgentDetailMeta(sessionId);
    syncAgentViewTaskButton(sessionId);
}

void MainWindow::openOrgTaskForSession(const AgentSession &session)
{
    if (!session.orgTask || session.id <= 0 || !session.orgTaskId.isEmpty())
        return;
    postOrgTaskForSession(session, session.issueTitle, /*manual=*/false,
                          /*completeIfFinished=*/true);
}

bool MainWindow::postOrgTaskForSession(const AgentSession &session,
                                       const QString &title, bool manual,
                                       bool completeIfFinished)
{
    if (session.id <= 0 || !session.orgTaskId.isEmpty())
        return false;
    if (m_orgTaskOpenPending.contains(session.id))
        return false;
    if (!m_networkAccess)
        return false;

    const QString repository = session.owner + QLatin1Char('/') + session.name;
    const QString assigneeKind = agentIsClaudeProvider(session.provider)
                                     ? QStringLiteral("claude")
                                     : QStringLiteral("codex");
    QString details = session.prompt.trimmed();
    if (details.isEmpty() && session.issueNumber > 0)
        details = QStringLiteral("ForkMesh issue #%1: %2")
                      .arg(session.issueNumber)
                      .arg(session.issueTitle);

    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/tasks"));
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request;
    // No account session and no signing key: the run is simply local-only,
    // which is not an error worth interrupting the prompt for.
    if (!authenticateOrgTaskRequest(url, request, kOrgTaskOpenProof, QString()))
        return false;
    const QJsonObject body{
        {QStringLiteral("title"), title},
        {QStringLiteral("details"), details},
        {QStringLiteral("department"), QStringLiteral("engineering")},
        {QStringLiteral("destination"), QStringLiteral("agent")},
        {QStringLiteral("repository"), repository},
        {QStringLiteral("assigneeKind"), assigneeKind},
        {QStringLiteral("agent"),
         QJsonObject{
             {QStringLiteral("provider"), session.provider},
             {QStringLiteral("startedBy"), session.startedByBot},
             {QStringLiteral("model"), session.model},
             {QStringLiteral("mode"), session.mode},
             {QStringLiteral("strength"), session.strength},
             {QStringLiteral("sessionId"), QString::number(session.id)},
             {QStringLiteral("status"), session.status},
         }},
    };
    const int sessionId = session.id;
    m_orgTaskOpenPending.insert(sessionId);
    if (manual && m_selectedAgentSessionId == sessionId)
        syncAgentViewTaskButton(sessionId);
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, manual, completeIfFinished] {
        const QByteArray payload = reply->readAll();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        m_orgTaskOpenPending.remove(sessionId);
        if (status < 200 || status >= 300) {
            // Not an org member, signed out, relay down: the agent is already
            // running and nothing about it depends on the task existing.
            const QString failure = QStringLiteral(
                                        "Organization task not opened for agent "
                                        "session #%1 (relay replied %2).")
                                        .arg(sessionId)
                                        .arg(status);
            logSystem(failure);
            if (manual) {
                flashMessage(failure, true);
                if (m_selectedAgentSessionId == sessionId)
                    syncAgentViewTaskButton(sessionId);
            }
            return;
        }
        const QString taskId = QJsonDocument::fromJson(payload)
                                   .object()
                                   .value(QStringLiteral("task"))
                                   .toObject()
                                   .value(QStringLiteral("id"))
                                   .toString();
        if (taskId.isEmpty())
            return;
        if (manual) {
            if (AgentSession *s = findAgentSession(sessionId)) {
                s->orgTask = true;
                s->orgTaskAttached = false;
            }
        }
        recordOrgTaskFields(sessionId, taskId, QString(), completeIfFinished);
        announceOrgTaskOpened(sessionId);
        if (manual) {
            flashMessage(QStringLiteral("Created an organization task for agent "
                                        "session #%1.")
                             .arg(sessionId));
            if (m_organizationTaskStore && m_organizationTaskStore->loaded())
                refreshOrganizationTasks();
        }
    });
    return true;
}

// The board write is the one thing starting an agent does that the operator
// cannot see happen: the Agents tab shows the session, and the task lands on
// the board. Say it in the log, mirroring the line the failing branch above
// writes.
void MainWindow::announceOrgTaskOpened(int sessionId)
{
    if (sessionId <= 0)
        return;
    // The task title is the operator's own prompt text, and the log badges a
    // line by scanning it for words like "error" and "failed" — quoting a
    // prompt here would file this confirmation as an error and hand it to the
    // error-log repair teammate. The run's number is the handle that matters;
    // the title is already on the board.
    logSystem(QStringLiteral("Opened an organization task for agent session #%1.")
                  .arg(sessionId));
}

namespace {

QString redactedAgentText(const QString &text, int limit)
{
    static const QList<QRegularExpression> secrets = {
        QRegularExpression(QStringLiteral("-----BEGIN [A-Z ]*PRIVATE KEY-----.*?(?:-----END [A-Z ]*PRIVATE KEY-----|$)"),
                           QRegularExpression::DotMatchesEverythingOption),
        QRegularExpression(QStringLiteral("\\b(?:sk-ant-|sk-proj-|sk-|ghp_|gho_|ghu_|ghs_|github_pat_|xox[abprs]-|glpat-|AKIA|ASIA)[A-Za-z0-9_\\-]{8,}")),
        QRegularExpression(QStringLiteral("\\beyJ[A-Za-z0-9_\\-]{10,}\\.[A-Za-z0-9_\\-]{10,}\\.[A-Za-z0-9_\\-]{5,}")),
        QRegularExpression(QStringLiteral("\\b(password|passwd|secret|token|api[_-]?key)\\s*[:=]\\s*\\S+"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("\\b[A-Fa-f0-9]{32,}\\b")),
        QRegularExpression(QStringLiteral("\\b[A-Za-z0-9+/_\\-]{40,}={0,2}")),
    };
    QString out = text.simplified();
    for (const QRegularExpression &re : secrets)
        out.replace(re, QStringLiteral("[redacted]"));
    if (out.size() > limit)
        out = out.left(limit - 1) + QChar(0x2026);
    return out;
}

constexpr int kAgentProseLimit = 200;
constexpr int kAgentTitleLimit = 120;
constexpr int kAgentProseEveryMs = 20000;

} // namespace

void MainWindow::syncOfficeAgentProse()
{
    if (!m_networkAccess)
        return;
    bool queued = false;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (isExternalSession(session.id))
            continue;
        const QString status = session.status.trimmed().toLower();
        if (status != AgentStatus::Queued && status != AgentStatus::Running &&
            status != AgentStatus::Waiting)
            continue;
        if (session.orgTaskId.isEmpty() && !agentPresenceWanted(session))
            continue;
        const QString prose = redactedAgentText(
            m_lastAssistantText.value(session.id), kAgentProseLimit);
        if (prose.isEmpty() || m_officeProseSent.value(session.id) == qHash(prose))
            continue;
        m_orgTaskAgentStatusSent.insert(session.id, status);
        m_orgTaskAgentStatusPending.insert(session.id, status);
        queued = true;
    }
    if (queued && !m_orgTaskAgentStatusFlushQueued) {
        m_orgTaskAgentStatusFlushQueued = true;
        QTimer::singleShot(0, this, &MainWindow::flushOrgTaskAgentStatus);
    }
}

bool MainWindow::agentPresenceWanted(const AgentSession &session) const
{
    constexpr qint64 kAgentPresenceFinishedMs = 8LL * 60 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (session.deletedAtMs > 0 || now < m_agentPresencePausedUntilMs)
        return false;
    const QString status = session.status.trimmed().toLower();
    if (status == AgentStatus::Queued || status == AgentStatus::Running ||
        status == AgentStatus::Waiting)
        return true;
    return session.finishedAtMs > 0 &&
           now - session.finishedAtMs < kAgentPresenceFinishedMs;
}

void MainWindow::syncOrgTaskAgentStatus(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || isExternalSession(sessionId) || !m_networkAccess)
        return;
    if (session->orgTaskId.isEmpty() && !agentPresenceWanted(*session))
        return;
    if (!m_officeProseTimer) {
        m_officeProseTimer = new QTimer(this);
        m_officeProseTimer->setInterval(kAgentProseEveryMs);
        connect(m_officeProseTimer, &QTimer::timeout, this, &MainWindow::syncOfficeAgentProse);
        m_officeProseTimer->start();
    }
    const QString status = session->status.trimmed().toLower();
    if (status != AgentStatus::Queued && status != AgentStatus::Running &&
        status != AgentStatus::Waiting && status != AgentStatus::Success &&
        status != AgentStatus::Failed && status != AgentStatus::Stopped)
        return;
    if (m_orgTaskAgentStatusSent.value(sessionId) == status)
        return;
    m_orgTaskAgentStatusSent.insert(sessionId, status);
    m_orgTaskAgentStatusPending.insert(sessionId, status);
    // reloadAgents() walks every session, so the first reload after launch lands
    // here once per session — seventeen signed POSTs in the same millisecond,
    // most of which the relay answered 429 to. Coalesce the burst
    // into the single batched write below on the next event-loop turn.
    if (m_orgTaskAgentStatusFlushQueued)
        return;
    m_orgTaskAgentStatusFlushQueued = true;
    QTimer::singleShot(0, this, &MainWindow::flushOrgTaskAgentStatus);
}

void MainWindow::flushOrgTaskAgentStatus()
{
    m_orgTaskAgentStatusFlushQueued = false;
    const QHash<int, QString> pending = m_orgTaskAgentStatusPending;
    m_orgTaskAgentStatusPending.clear();
    if (pending.isEmpty())
        return;

    // Re-resolve each session: queuing happened turns ago and m_agentSessions
    // may have been rebuilt since (the git-pump UAF family).
    constexpr int kMaxPresencePerBatch = 40;
    QList<int> order = pending.keys();
    const auto rank = [this, &pending](int id) {
        const AgentSession *session = findAgentSession(id);
        if (!session || !session->orgTaskId.isEmpty())
            return 0;
        const QString status = pending.value(id);
        return status == AgentStatus::Queued || status == AgentStatus::Running ||
                       status == AgentStatus::Waiting
                   ? 1
                   : 2;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&rank](int a, int b) { return rank(a) < rank(b); });
    QJsonArray statuses;
    QList<QPair<int, QString>> sent;
    int presence = 0;
    for (const int id : std::as_const(order)) {
        const auto it = pending.constFind(id);
        const AgentSession *session = findAgentSession(it.key());
        const bool skipPresence = session && session->orgTaskId.isEmpty() &&
                                  presence >= kMaxPresencePerBatch;
        if (!session || isExternalSession(it.key()) || skipPresence ||
            (session->orgTaskId.isEmpty() && !agentPresenceWanted(*session))) {
            if (m_orgTaskAgentStatusSent.value(it.key()) == it.value())
                m_orgTaskAgentStatusSent.remove(it.key());
            continue;
        }
        if (session->orgTaskId.isEmpty())
            ++presence;
        QJsonObject agent{
            {QStringLiteral("provider"), session->provider},
            {QStringLiteral("startedBy"), session->startedByBot},
            {QStringLiteral("model"), session->model},
            {QStringLiteral("mode"), session->mode},
            {QStringLiteral("strength"), session->strength},
            {QStringLiteral("sessionId"), QString::number(session->id)},
        };
        if (session->orgTaskId.isEmpty() && session->startedAtMs > 0)
            agent.insert(QStringLiteral("startedAt"), session->startedAtMs);
        if (session->orgTaskId.isEmpty()) {
            const QString title = redactedAgentText(session->issueTitle, kAgentTitleLimit);
            if (!title.isEmpty())
                agent.insert(QStringLiteral("title"), title);
        }
        const QString prose = redactedAgentText(m_lastAssistantText.value(session->id),
                                                kAgentProseLimit);
        if (!prose.isEmpty()) {
            agent.insert(QStringLiteral("prose"), prose);
            m_officeProseSent.insert(session->id, qHash(prose));
        }
        statuses.append(QJsonObject{
            {QStringLiteral("task"), session->orgTaskId},
            {QStringLiteral("status"), it.value()},
            {QStringLiteral("agent"), agent},
        });
        sent.append({it.key(), it.value()});
    }
    if (statuses.isEmpty())
        return;

    // The batch proof names no task: the ids ride in the body and the relay
    // re-checks ownership for each one exactly as it does for a single write.
    // Signing it up front lets the same proof travel either way.
    QString node, ts, sig;
    const bool haveProof = signOrgTaskProof(kOrgTaskAgentStatusBatchProof,
                                            QString(), &node, &ts, &sig);

    // This desktop is already holding a socket to the relay so the board can
    // push to it. Sending the report back up that socket costs no request at
    // all — which is the point, since the rate limiting that started this
    // was the relay refusing this node's own HTTP writes. The
    // relay verifies the very same signature it would over HTTPS.
    if (haveProof && m_nodeEventSocket &&
        m_nodeEventSocket->sendSignedFrame(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("agent-status")},
            {QStringLiteral("node"), node},
            {QStringLiteral("ts"), ts},
            {QStringLiteral("sig"), sig},
            {QStringLiteral("statuses"), statuses},
        })) {
        m_orgTaskAgentStatusInFlight.append(sent);
        if (!m_orgTaskAgentStatusAckTimer) {
            m_orgTaskAgentStatusAckTimer = new QTimer(this);
            m_orgTaskAgentStatusAckTimer->setSingleShot(true);
            connect(m_orgTaskAgentStatusAckTimer, &QTimer::timeout, this,
                    &MainWindow::requeueOrgTaskAgentStatusInFlight);
        }
        m_orgTaskAgentStatusAckTimer->start(kOrgTaskAgentStatusAckTimeoutMs);
        return;
    }

    if (!haveProof || !m_networkAccess) {
        for (const QPair<int, QString> &entry : std::as_const(sent))
            if (m_orgTaskAgentStatusSent.value(entry.first) == entry.second)
                m_orgTaskAgentStatusSent.remove(entry.first);
        return;
    }
    // No socket, or a relay too old to accept the frame: the signed HTTPS
    // route, still one request for the whole fleet.
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/tasks/agent-status"));
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(url, request, kOrgTaskAgentStatusBatchProof,
                                    QString())) {
        for (const QPair<int, QString> &entry : std::as_const(sent))
            if (m_orgTaskAgentStatusSent.value(entry.first) == entry.second)
                m_orgTaskAgentStatusSent.remove(entry.first);
        return;
    }
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("statuses"), statuses}})
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, sent] {
        const int response =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        reply->deleteLater();
        applyOrgTaskAgentStatusResult(
            sent, response >= 200 && response < 300,
            QJsonDocument::fromJson(payload)
                .object()
                .value(QStringLiteral("results"))
                .toArray());
    });
}

void MainWindow::applyOrgTaskAgentStatusResult(
    const QList<QPair<int, QString>> &sent, bool ok,
    const QJsonArray &results)
{
    if (!ok) {
        bool presenceOnly = !sent.isEmpty();
        for (const QPair<int, QString> &entry : sent) {
            const AgentSession *session = findAgentSession(entry.first);
            if (session && !session->orgTaskId.isEmpty())
                presenceOnly = false;
            if (m_orgTaskAgentStatusSent.value(entry.first) == entry.second)
                m_orgTaskAgentStatusSent.remove(entry.first);
        }
        if (presenceOnly)
            m_agentPresencePausedUntilMs =
                QDateTime::currentMSecsSinceEpoch() + 30LL * 60 * 1000;
        return;
    }
    QHash<QString, bool> accepted;
    for (const QJsonValue &value : results)
        accepted.insert(
            value.toObject().value(QStringLiteral("task")).toString(),
            value.toObject().value(QStringLiteral("ok")).toBool());
    for (const QPair<int, QString> &entry : sent) {
        const AgentSession *session = findAgentSession(entry.first);
        if (session && !accepted.value(session->orgTaskId, true) &&
            m_orgTaskAgentStatusSent.value(entry.first) == entry.second)
            m_orgTaskAgentStatusSent.remove(entry.first);
    }
}

void MainWindow::onOrgTaskAgentStatusFrame(bool ok, const QJsonArray &results)
{
    if (m_orgTaskAgentStatusInFlight.isEmpty())
        return;
    const QList<QPair<int, QString>> batch =
        m_orgTaskAgentStatusInFlight.takeFirst();
    if (m_orgTaskAgentStatusAckTimer) {
        if (m_orgTaskAgentStatusInFlight.isEmpty())
            m_orgTaskAgentStatusAckTimer->stop();
        else
            m_orgTaskAgentStatusAckTimer->start(
                kOrgTaskAgentStatusAckTimeoutMs);
    }
    applyOrgTaskAgentStatusResult(batch, ok, results);
}

void MainWindow::requeueOrgTaskAgentStatusInFlight()
{
    const QList<QList<QPair<int, QString>>> stranded =
        m_orgTaskAgentStatusInFlight;
    m_orgTaskAgentStatusInFlight.clear();
    if (m_orgTaskAgentStatusAckTimer)
        m_orgTaskAgentStatusAckTimer->stop();
    for (const QList<QPair<int, QString>> &batch : stranded)
        for (const QPair<int, QString> &entry : batch)
            if (m_orgTaskAgentStatusSent.value(entry.first) == entry.second)
                m_orgTaskAgentStatusSent.remove(entry.first);
}

void MainWindow::completeOrgTaskForSession(int sessionId, const QString &followUp,
                                           bool evenIfAttached)
{
    const AgentSession *s = findAgentSession(sessionId);
    if (!s || s->orgTaskId.isEmpty() || isExternalSession(sessionId))
        return;
    if (s->orgTaskAttached && !evenIfAttached)
        return;
    if (!s->finishedByBot.isEmpty() && followUp.trimmed().isEmpty())
        return;
    if (followUp.trimmed().isEmpty() && s->status != AgentStatus::Success &&
        s->status != AgentStatus::Failed && s->status != AgentStatus::Stopped)
        return;
    if (!m_networkAccess)
        return;

    const AgentSession session = *s; // by value: the post below pumps the loop
    const QString finishedBy = agentBotLabel(session.provider);
    QString note = QStringLiteral("%1 finished this run with status %2.")
                       .arg(finishedBy)
                       .arg(session.status);
    if (!followUp.trimmed().isEmpty())
        note += QLatin1Char(' ') + followUp.trimmed();
    if (!session.lastError.trimmed().isEmpty())
        note += QStringLiteral(" Last error: %1").arg(session.lastError.trimmed());
    if (session.prNumber > 0)
        note += QStringLiteral(" Opened pull request #%1.").arg(session.prNumber);
    if (session.merged)
        note += QStringLiteral(" Merged into %1.").arg(agentMergeBase(session));
    if (!session.branchName.isEmpty())
        note += QStringLiteral(" Branch %1.").arg(session.branchName);
    QStringList stats;
    if (session.numTurns > 0)
        stats << QStringLiteral("%1 turns").arg(session.numTurns);
    const qint64 dur = agentEffectiveDurationMs(session);
    if (dur > 0)
        stats << QStringLiteral("%1s").arg(dur / 1000);
    if (session.costUsd > 0.0)
        stats << agentCostText(session.costUsd);
    const qint64 toks = sessionTokenTotal(session);
    if (toks > 0)
        stats << QStringLiteral("%1 tokens").arg(formatCount(toks));
    if (!stats.isEmpty())
        note += QStringLiteral(" Run summary: %1.")
                    .arg(stats.join(QString::fromUtf8(" \xC2\xB7 ")));

    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/tasks/") + session.orgTaskId +
                QStringLiteral("/complete"));
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(url, request, kOrgTaskCompleteProof,
                                    session.orgTaskId))
        return;
    const QJsonObject body{
        {QStringLiteral("completionNote"), note},
        {QStringLiteral("agent"),
         QJsonObject{
             {QStringLiteral("provider"), session.provider},
             {QStringLiteral("finishedBy"), finishedBy},
             {QStringLiteral("model"), session.model},
             {QStringLiteral("mode"), session.mode},
             {QStringLiteral("strength"), session.strength},
             {QStringLiteral("status"), session.status},
         }},
    };
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, finishedBy] {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                reply->deleteLater();
                if (status < 200 || status >= 300) {
                    logSystem(QStringLiteral(
                                  "Organization task for agent session #%1 not "
                                  "closed out (relay replied %2).")
                                  .arg(sessionId)
                                  .arg(status));
                    return;
                }
                recordOrgTaskFields(sessionId, QString(), finishedBy);
            });
}

bool MainWindow::mergeAgentBranchIntoBase(int repoIndex, const QString &branchArg,
                                          bool cleanUp,
                                          std::function<void(bool)> done,
                                          bool unattended)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return false;
    // Copy everything out of the lists before the bind: it pumps the GUI event
    // loop over blocking git reads, and a reload landing in that pump rebuilds
    // m_repositories/m_agentSessions — `branchArg` aliases a session field
    // (git-pump UAF family).
    const QString branch = branchArg;
    const QString localPath = m_repositories.at(repoIndex).localPath;
    const QString owner = m_repositories.at(repoIndex).owner;
    const QString name = m_repositories.at(repoIndex).name;
    if (branch.isEmpty())
        return false;
    if (!bindRepoDetailToRepo(repoIndex)) {
        setRepoDetailNotice(
            QStringLiteral("Couldn't open %1/%2 to merge %3 — try again from that "
                           "repository.")
                .arg(owner, name, branch),
            true);
        return false;
    }
    mergeWorktreeIntoMain(
        branch, worktreePathForBranch(localPath, branch, WorktreeLookup::Any),
        cleanUp, std::move(done), unattended);
    return true;
}

void MainWindow::maybeAutoMergeForSession(int sessionId)
{
    const AgentSession *s = findAgentSession(sessionId);
    if (!s || !(s->yolo || s->vibe) || s->merged || s->branchName.isEmpty() ||
        s->status != AgentStatus::Success || isExternalSession(sessionId))
        return;
    const int ri = repoIndexFor(s->owner, s->name);
    if (ri < 0)
        return;
    // Snapshot by value before anything below: the merge pumps the GUI event loop
    // over blocking git reads, and a reloadAgents() fired during the pump rebuilds
    // m_agentSessions — `s` would dangle (git-pump UAF family).
    const AgentSession session = *s;
    const QString label = session.vibe ? QStringLiteral("Vibe")
                                       : QStringLiteral("YOLO");
    if (m_agentStore)
        m_agentStore->appendLog(
            session,
            QStringLiteral("==> %1: merging %2 into the default branch.\n")
                .arg(label, session.branchName));
    std::function<void(bool)> afterMerge;
    if (session.vibe) {
        afterMerge = [this, owner = session.owner, name = session.name,
                      branch = session.branchName](bool landed) {
            if (!landed) {
                logSystem(QStringLiteral("Vibe: %1 was not merged into %2/%3; "
                                         "its worktree and branch were kept.")
                              .arg(branch, owner, name));
                return;
            }
            const int index = repoIndexFor(owner, name);
            if (index < 0 || index != m_repoDetailIndex) {
                logSystem(QStringLiteral("Vibe: merged %1, but %2/%3 was closed "
                                         "before its default branch was pushed.")
                              .arg(branch, owner, name));
                return;
            }
            logSystem(QStringLiteral("Vibe: merged %1 into %2/%3; pushing the "
                                     "default branch.")
                          .arg(branch, owner, name));
            pushCurrentRepoUpstream();
        };
    }
    if (!mergeAgentBranchIntoBase(ri, session.branchName,
                                  /*cleanUp=*/session.vibe, std::move(afterMerge),
                                  /*unattended=*/true)
        && m_agentStore)
        m_agentStore->appendLog(
            session,
            QStringLiteral("!! %1: %2/%3 could not be opened; merge %4 by hand.\n")
                .arg(label, session.owner, session.name, session.branchName));
}

QList<int> MainWindow::releaseTrackedWorktree(const QString &worktreePath)
{
    QList<int> released;
    for (auto it = m_streamWorktree.begin(); it != m_streamWorktree.end();) {
        if (!forkmesh::agentwt::samePath(it.value(), worktreePath)) {
            ++it;
            continue;
        }
        released.append(it.key());
        m_sessionWorkdirCache.remove(it.key()); // about to vanish — don't cache it
        retireStreamCwd(it.value());
        it = m_streamWorktree.erase(it);
    }
    return released;
}

void MainWindow::retrackWorktree(const QList<int> &sessionIds, const QString &worktreePath)
{
    if (worktreePath.trimmed().isEmpty())
        return;
    for (const int id : sessionIds) {
        if (findAgentSession(id)) // sessions already deleted stay gone
            m_streamWorktree.insert(id, worktreePath);
    }
}

void MainWindow::cleanupStreamWorktree(int sessionId)
{
    m_sessionWorkdirCache.remove(sessionId); // worktree about to vanish — don't cache it
    const QString wtPath = m_streamWorktree.take(sessionId);
    if (wtPath.isEmpty())
        return;
    retireStreamCwd(wtPath);
    // A run started without a worktree works in the repository's own checkout,
    // whose .git is a directory rather than a worktree's pointer file. That
    // folder is the operator's, never ours to remove.
    if (QFileInfo(wtPath + QStringLiteral("/.git")).isDir())
        return;
    if (m_agentPermissionController)
        m_agentPermissionController->clearWorktree(wtPath);
    QString repoPath;
    if (const AgentSession *s = findAgentSession(sessionId)) {
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri >= 0)
            repoPath = repoAgentGitDir(m_repositories.at(ri)); // mirror or checkout
    }
    QStringList agentRoots{forkmesh::agentwt::root(repoPath),
                           QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                               QStringLiteral("/forkmesh-worktrees")};
    if (forkmesh::vm::active())
        agentRoots << forkmesh::vm::worktreeRoot();
    QString agentRoot;
    for (const QString &root : std::as_const(agentRoots)) {
        if (!root.trimmed().isEmpty() &&
            forkmesh::safedelete::refusal(wtPath, root,
                                          forkmesh::safedelete::Allow::OwnWorktree)
                .isEmpty()) {
            agentRoot = root;
            break;
        }
    }
    if (agentRoot.isEmpty()) {
        logSystem(QStringLiteral("Agent worktree %1 was kept: it is not in an agent "
                                 "worktree folder this app created.")
                      .arg(wtPath));
        return;
    }
    QThread *worker = QThread::create([wtPath, repoPath, agentRoot]() {
        if (!repoPath.isEmpty()) {
            QProcess::execute(QStringLiteral("git"),
                              {QStringLiteral("-C"), repoPath, QStringLiteral("worktree"),
                               QStringLiteral("remove"), QStringLiteral("--force"), wtPath});
        }
        forkmesh::safedelete::removeTree(wtPath, agentRoot, nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
        forkmesh::agentscratch::removeAll(wtPath);
        forkmesh::agentscratch::sweepOrphans(agentRoot);
        if (!repoPath.isEmpty()) {
            QProcess::execute(QStringLiteral("git"),
                              {QStringLiteral("-C"), repoPath, QStringLiteral("worktree"),
                               QStringLiteral("prune")});
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    if (QThread *prev = m_worktreeTeardown.value(sessionId))
        prev->disconnect(this); // an older teardown for this id is being superseded
    m_worktreeTeardown[sessionId] = worker;
    connect(worker, &QThread::finished, this, [this, sessionId, worker] {
        if (m_worktreeTeardown.value(sessionId) == worker)
            m_worktreeTeardown.remove(sessionId);
    });
    worker->start();
}

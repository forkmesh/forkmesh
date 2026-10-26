
#include "MainWindow.h"
#include "AgentLogText.h"
#include "ClaudeTranscriptTools.h"
#include "MainWindowInternal.h"
#include "AccountCapability.h"
#include "ForkMeshPackageRegistry.h"
#include "RepoStatsStore.h"
#include "AgentCliLocator.h"
#include "AgentCredentialEnvironment.h"
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
#include "WorktreeInclude.h"
#include "FlowLayout.h"
#include "KebabHeaderView.h"
#include "CodexAppServerSession.h"
#include "CrashHandler.h"
#include "NodeEventSocket.h"
#include "UsageLimitCalendar.h"

#include <QActionGroup>
#include <QFutureWatcher>
#include <QtConcurrent>
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
#include <QPointer>
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

constexpr int kAgentExternalStopSettleMs = 3000;
constexpr int kExitAfterSigterm = 128 + 15;
constexpr int kExitAfterSighup = 128 + 1;

const QString kExternalClaudeFolderName = QStringLiteral("External Claude Code");
const QString kExternalClaudeMemberPrefix = QStringLiteral("claude:");

void appendBoundedStreamRaw(QString &buf, const QString &piece)
{
    constexpr qsizetype kMax = 400000;
    constexpr qsizetype kKeep = 300000;
    if (buf.size() + piece.size() > kMax && piece.size() >= kKeep) {
        buf = piece.right(kKeep);
        return;
    }
    buf.append(piece);
    if (buf.size() > kMax)
        buf.remove(0, buf.size() - kKeep);
}

struct ExternalTranscriptRead {
    bool full = false;
    bool redraw = false;
    qint64 newOffset = 0;
    qint64 addedTokens = 0;
    QList<QJsonObject> events;
    QString raw;
};

} // namespace

void MainWindow::startCliTranscript(AgentSession &session, const Issue &issue,
                                    const QString &repoPath,
                                    const QString &customPrompt,
                                    bool switchToTab,
                                    const RepositoryRecord *repository)
{
    if (!m_agentStore)
        return;
    const int sid = session.id;
    if (!session.directCheckout && session.issueNumber == 0) {
        const QString worktreeRoot = forkmesh::vm::active()
                                         ? forkmesh::vm::worktreeRoot()
                                         : forkmesh::agentwt::root(repoPath);
        const auto existing = forkmesh::agentwt::existingWorkspace(worktreeRoot, sid);
        if (!existing.path.isEmpty() &&
            (session.branchName != existing.branch ||
             session.worktreeName != QFileInfo(existing.path).fileName())) {
            session.branchName = existing.branch;
            session.worktreeName = QFileInfo(existing.path).fileName();
            m_agentStore->saveSession(session);
            m_agentStore->appendLog(
                session, QStringLiteral("==> Recovered existing workspace %1.\n")
                             .arg(session.worktreeName));
        }
    }
    // The queue routes here on the provider, so the provider names the CLI.
    // Sessions switched to another agent before the runtime followed the
    // provider still carry the old runtime and would relaunch that agent.
    session.runtimeId = agentRuntimeIdForLegacyProvider(session.provider);
    const bool codex = session.runtimeId == AgentRuntimeIds::CodexAppServer;
    if (!codex && session.provider == QLatin1String("claude-code"))
        reconcileClaudeAccountLogins();
    const AgentAccountProfile runAccount = activeAgentAccount(session.provider);
    const QStringList runAccountEnv = activeAgentAccountEnv(session.provider);
    m_streamAccountId[sid] = runAccount.id;
    forkmesh::agentguard::Config guardrails =
        cliTranscriptGuardrails(session, repoPath, repository);

    resolveCliTranscriptBase(session, repoPath);
    if (session.prompt.isEmpty() && !customPrompt.trimmed().isEmpty())
        session.prompt = customPrompt.trimmed();

    const QString lead =
        customPrompt.trimmed().isEmpty()
            ? QStringLiteral("Resolve the following ForkMesh issue.\n\n%1")
                  .arg(issueContextPrompt(issue))
            : customPrompt.trimmed();
    const QString customPreamble =
        QSettings().value(kAgentPromptPreambleSetting).toString().trimmed();
    QString prompt = customPreamble.isEmpty()
                         ? lead
                         : customPreamble + QStringLiteral("\n\n") + lead;
    prompt += QStringLiteral("\n\n") +
              (session.directCheckout
                   ? AgentRunner::directCheckoutInstruction()
                   : AgentRunner::commitChangesInstruction());
    prompt += QStringLiteral("\n\n") + AgentRunner::toolUseInstruction();
    const QString guardrailInstruction =
        forkmesh::agentguard::promptInstruction(guardrails);
    if (!guardrailInstruction.isEmpty())
        prompt += QStringLiteral("\n\n") + guardrailInstruction;
    const QString originalTaskPrompt = prompt;

    const bool resuming = !m_streamEvents.value(sid).isEmpty();
    if (!resuming) {
        m_streamEvents[sid].clear();
        m_streamRaw[sid].clear();
        m_streamFiles[sid].clear();
        m_agentStore->clearEvents(session);
        // The rows on screen no longer stand for the (now empty) stream, so the
        // next show must replay rather than append its tail to them.
        if (m_renderedTranscriptSession == sid) {
            m_renderedTranscriptSession = -1;
            m_renderedTranscriptCount = -1;
        }
    }
    const QString steer = takeAgentSteerMessageForLaunch(sid);
    // An explicit selector change must start a fresh native conversation. If
    // the user switches Claude -> Codex -> Claude, the old Claude session id is
    // still in the event log, but resuming it would silently discard everything
    // Codex did between those turns. The transcript handoff below is the source
    // of truth for the replacement conversation instead.
    const bool explicitProviderHandoff =
        !session.handoffFromProvider.isEmpty() &&
        session.handoffFromProvider != session.provider;
    // A model the provider rejected may never have recorded the task in its
    // native thread, so its replacement starts over from the saved transcript.
    const QString rejectedModel = m_agentModelHandoffs.take(sid);
    const bool modelHandoff =
        resuming && !rejectedModel.isEmpty() && !explicitProviderHandoff;
    QString resumeId =
        resuming && !explicitProviderHandoff && !modelHandoff
            ? (codex ? lastCodexThreadId(sid) : lastClaudeSessionId(sid))
            : QString();
    QString incompatibleResumeReason;
    if (!resumeId.isEmpty() &&
        !session.canResumeWithRuntime(
            AgentRuntimeRegistry::descriptor(session.runtimeId),
            &incompatibleResumeReason)) {
        // Keep the worktree and visible transcript intact. A compatible new
        // conversation receives the bounded saved transcript below instead of
        // sending an unsafe resume request to an incompatible runtime.
        resumeId.clear();
    }
    QString directWorkdir = repoPath;
    if (session.directCheckout && session.claudeImported()) {
        const QString repoRoot = QDir(repoPath).absolutePath();
        const QString cwd = QDir(session.claudeImportCwd).absolutePath();
        if (cwd.startsWith(repoRoot + QLatin1Char('/')) && QFileInfo(cwd).isDir())
            directWorkdir = cwd;
    }
    QString claudeImportPath;
    if (!codex && session.claudeImported() && resumeId == session.claudeImportId) {
        claudeImportPath = session.claudeImportPath;
        session.claudeImportBytes = -1;
    }
    QList<QJsonObject> events = m_streamEvents.value(sid);
    QString handoffAccount;
    if (resuming && resumeId.isEmpty()) {
        const QString ownerId =
            forkmesh::agents::resumeConversationAccountId(events, codex);
        if (!ownerId.isEmpty() && ownerId != runAccount.id) {
            const QString label =
                agentAccountLabelFor(session.provider, ownerId);
            handoffAccount =
                label.isEmpty() ? QStringLiteral("another account") : label;
        }
    }
    const QString handoffFrom =
        modelHandoff
            ? rejectedModel
        : explicitProviderHandoff
            ? cliProviderLabel(session.handoffFromProvider)
            : (resuming && resumeId.isEmpty() && handoffAccount.isEmpty()
                   ? (codex ? (forkmesh::agents::claudeResumeSessionId(events).isEmpty()
                                   ? QString()
                                   : QStringLiteral("Claude Code"))
                            : (forkmesh::agents::codexResumeThreadId(events).isEmpty()
                                   ? QString()
                                   : QStringLiteral("Codex")))
                   : QString());
    events = QList<QJsonObject>();
    QString handoffTranscript;
    if ((!handoffFrom.isEmpty() || !handoffAccount.isEmpty() ||
         !incompatibleResumeReason.isEmpty()) &&
        m_agentStore) {
        handoffTranscript = m_agentStore->readLog(session).trimmed();
        constexpr int kMaxHandoffTranscriptChars = 48 * 1024;
        if (handoffTranscript.size() > kMaxHandoffTranscriptChars)
            handoffTranscript =
                QStringLiteral("[Earlier transcript omitted]\n") +
                handoffTranscript.right(kMaxHandoffTranscriptChars);
    }
    const QString transcriptContext =
        handoffTranscript.isEmpty()
            ? QString()
            : QStringLiteral("\n\nSaved conversation transcript/log (oldest "
                             "omitted if necessary):\n---\n%1\n---")
                  .arg(handoffTranscript);
    if (!resumeId.isEmpty()) {
        if (steer.isEmpty()) {
            prompt = continueWhereLeftOffPrompt();
            applyTranscriptEvent(
                sid, QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_user")},
                                 {QStringLiteral("text"), prompt}});
        } else {
            prompt = steer; // already shown in the transcript by the composer
        }
    } else {
        if (!incompatibleResumeReason.isEmpty()) {
            prompt = QStringLiteral(
                         "The saved native conversation cannot be resumed "
                         "safely by this runtime (%1). Continue from the "
                         "existing worktree and saved transcript instead. "
                         "Do not repeat completed work.\n\nOriginal task:\n%2%3")
                         .arg(incompatibleResumeReason, originalTaskPrompt,
                              transcriptContext);
            applyTranscriptEvent(
                sid,
                QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("_local_notice")},
                    {QStringLiteral("text"),
                     QStringLiteral(
                         "The saved runtime protocol is incompatible with this "
                         "runtime. ForkMesh preserved the worktree and transcript "
                         "and started a safe handoff instead.")}});
        } else if (modelHandoff) {
            prompt = QStringLiteral(
                         "You are taking over this session from model %1, which "
                         "the provider rejected before it could run. Its saved "
                         "transcript is included below. Work may already be in "
                         "progress on the current branch — read what is there "
                         "before changing anything, then carry on with the "
                         "task.\n\nOriginal task:\n%2%3")
                         .arg(rejectedModel, originalTaskPrompt,
                              transcriptContext);
            applyTranscriptEvent(
                sid,
                QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("_local_notice")},
                    {QStringLiteral("text"),
                     QStringLiteral(
                         "Starting a fresh %1 conversation instead of resuming "
                         "the one %2 couldn't run. The original task, saved "
                         "transcript, and branch were handed over as context.")
                         .arg(codex ? QStringLiteral("Codex")
                                    : QStringLiteral("Claude Code"),
                              rejectedModel)}});
        } else if (!handoffFrom.isEmpty()) {
            prompt = QStringLiteral(
                         "You are taking over this session from %1, whose "
                         "native conversation cannot be resumed by a different "
                         "agent. Its saved transcript is included below. Work is "
                         "already in progress on the current branch — read what "
                         "is there before changing anything, then "
                         "carry on from that point.\n\nOriginal task:\n%2%3")
                         .arg(handoffFrom, originalTaskPrompt,
                              transcriptContext);
            applyTranscriptEvent(
                sid,
                QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("_local_notice")},
                    {QStringLiteral("text"),
                     QStringLiteral(
                         "Continuing with %1. A %2 conversation can't be resumed "
                         "by another agent, so the original task, saved transcript, "
                         "and branch were handed over as context.")
                         .arg(codex ? QStringLiteral("Codex")
                                    : QStringLiteral("Claude Code"),
                              handoffFrom)}});
        } else if (!handoffAccount.isEmpty()) {
            prompt = QStringLiteral(
                         "You are taking over this session from a different %1 "
                         "account, whose conversation this login cannot open. "
                         "Work is already in progress on the current branch — "
                         "read what is there before changing anything, then "
                         "carry on from that point.\n\nOriginal task:\n%2%3")
                         .arg(codex ? QStringLiteral("Codex")
                                    : QStringLiteral("Claude Code"),
                              originalTaskPrompt, transcriptContext);
            applyTranscriptEvent(
                sid,
                QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("_local_notice")},
                    {QStringLiteral("text"),
                     QStringLiteral(
                         "Continuing as %1. The earlier turns belong to %2, and "
                         "one account can't open another's conversation, so the "
                         "original task was re-sent with the branch as its "
                         "context.")
                         .arg(runAccount.label.trimmed().isEmpty()
                                  ? QStringLiteral("the selected account")
                                  : runAccount.label.trimmed(),
                              handoffAccount)}});
        }
        if (!steer.isEmpty()) {
            prompt +=
                QStringLiteral("\n\nAdditional user instruction:\n%1\n").arg(steer);
        }
    }
    // A native resume sends only the latest turn, not originalTaskPrompt, so
    // repeat the active boundary on that path as well. Enforcement does not
    // depend on the prompt; this keeps the provider from wasting denied calls.
    if (!resumeId.isEmpty() && !guardrailInstruction.isEmpty())
        prompt += QStringLiteral("\n\n") + guardrailInstruction;
    QString resumeFallbackPrompt;
    if (!resumeId.isEmpty()) {
        resumeFallbackPrompt =
            QStringLiteral(
                "The previous %1 conversation could not be resumed. Continue the "
                "same work from the current branch and repository state.\n\n"
                "Original task:\n%2\n\nLatest user instruction:\n%3")
                .arg(codex ? QStringLiteral("Codex thread")
                           : QStringLiteral("Claude Code session"),
                     originalTaskPrompt,
                     steer.isEmpty() ? continueWhereLeftOffPrompt() : steer);
    }
    if (!claudeImportPath.isEmpty() && m_agentStore) {
        QString saved = m_agentStore->readLog(session).trimmed();
        constexpr int kMaxImportedTranscriptChars = 48 * 1024;
        if (saved.size() > kMaxImportedTranscriptChars)
            saved = QStringLiteral("[Earlier transcript omitted]\n") +
                    saved.right(kMaxImportedTranscriptChars);
        resumeFallbackPrompt =
            QStringLiteral(
                "This conversation was started in Claude Code outside ForkMesh and "
                "could not be resumed natively. Its saved transcript is below; carry "
                "on from where it left off.\n---\n%1\n---\n\nLatest user "
                "instruction:\n%2")
                .arg(saved, steer.isEmpty() ? continueWhereLeftOffPrompt() : steer);
    }
    m_streamSessionInfo[sid] = session;
    m_agentCliTasks.remove(sid);
    m_agentResultAwaitingCliTasks.remove(sid);
    if (ClaudeStreamSession *old = m_streamSessions.take(sid))
        old->deleteLater();
    if (CodexAppServerSession *old = m_codexStreams.take(sid))
        old->deleteLater();
    if (!resuming)
        applyTranscriptEvent(sid, QJsonObject{
                                      {QStringLiteral("type"), QStringLiteral("_local_user")},
                                      {QStringLiteral("text"), prompt}});
    const QStringList providerCredentialValues =
        localProviderCredentialValues();
    auto onEvent = [this, sid, guardrails](const QJsonObject &ev) {
        handleCliRuntimeEvent(sid, ev, guardrails);
    };
    auto onRaw =
        [this, sid, providerCredentialValues](
            const QString &rawLine) {
        const QString line = redactProviderCredentials(
            rawLine, providerCredentialValues);
        noteAgentActivity(sid, line.size()); // pulse the list's night-rider light
        appendBoundedStreamRaw(m_streamRaw[sid], line + QLatin1String("\n\n"));
        if (sid == m_selectedAgentSessionId)
            appendAgentRawLog(line + QStringLiteral("\n\n"));
    };
    auto onStderr =
        [this, sid, providerCredentialValues](
            const QString &rawText) {
        const QString text = redactProviderCredentials(
            rawText, providerCredentialValues);
        if (text.isEmpty())
            return;
        noteAgentActivity(sid, text.size());
        QString piece = QLatin1String("[stderr] ") + text;
        if (!piece.endsWith(QLatin1Char('\n')))
            piece += QLatin1Char('\n');
        appendBoundedStreamRaw(m_streamRaw[sid], piece);
        if (sid == m_selectedAgentSessionId)
            appendAgentRawLog(QStringLiteral("[stderr] ") + text);
    };
    auto onFinished = [this, sid, codex](int exitCode) {
        finishCliTranscriptRun(sid, codex, exitCode);
    };
    AgentRuntimeSession *runtime =
        AgentRuntimeRegistry::create(session.runtimeId, this);
    if (codex) {
        auto *stream = qobject_cast<CodexAppServerSession *>(runtime);
        if (!stream) {
            delete runtime;
            onFinished(125);
            return;
        }
        m_codexStreams.insert(sid, stream);
        connect(runtime, &AgentRuntimeSession::event, this, onEvent);
        connect(runtime, &AgentRuntimeSession::rawLine, this, onRaw);
        connect(runtime, &AgentRuntimeSession::stderrText, this, onStderr);
        connect(runtime, &AgentRuntimeSession::finished, this, onFinished);
        connect(stream, &CodexAppServerSession::modelsListed, this,
                [this](const QJsonArray &models) {
                    applyListedCodexModels(models);
                });
        connect(stream, &CodexAppServerSession::toolCallOutput, this,
                [this, sid](const QString &tool, const QJsonObject &input,
                            const QString &output) {
                    reportCodexAccessFailure(
                        sid, QJsonObject{{QStringLiteral("tool"), tool},
                                         {QStringLiteral("input"), input},
                                         {QStringLiteral("output"), output}});
                });
    } else {
        auto *stream = qobject_cast<ClaudeStreamSession *>(runtime);
        if (!stream) {
            delete runtime;
            onFinished(125);
            return;
        }
        m_streamSessions.insert(sid, stream);
        stream->setFinishBarrier([this](std::function<void()> completion) {
            if (!m_agentStore) {
                completion();
                return;
            }
            m_agentStore->whenTranscriptWritesDrained(
                this, std::move(completion));
        });
        connect(runtime, &AgentRuntimeSession::event, this, onEvent);
        connect(runtime, &AgentRuntimeSession::rawLine, this, onRaw);
        connect(runtime, &AgentRuntimeSession::stderrText, this, onStderr);
        connect(runtime, &AgentRuntimeSession::finished, this, onFinished);
    }

    // showAgentSession() and reloadAgents() below rebuild m_agentSessions, and
    // processAgentQueue passes `session` as a reference into that list, so
    // everything read from it after them is copied here first.
    const QString branchName = session.branchName;
    const QString baseRef = session.baseRef;
    const bool directCheckout = session.directCheckout;
    const QString baseBranch = session.baseBranch;
    const int issueNumber = session.issueNumber;
    const QString worktreeName =
        session.worktreeName.isEmpty()
            ? forkmesh::agentwt::dirName(sid, session.issueTitle)
            : session.worktreeName;
    QString selectedModel = session.model;
    if (selectedModel.isEmpty())
        selectedModel = QSettings()
                            .value(codex ? kCodexModelSetting : kClaudeCodeModelSetting)
                            .toString()
                            .trimmed();
    if (codex)
        selectedModel = codexChatGptModelId(selectedModel);
    const QString launchProvider = codex ? kCodexProvider : QStringLiteral("claude-code");
    if (!agentModelMatchesProvider(launchProvider, selectedModel))
        selectedModel.clear();
    const QString sessionMode =
        session.mode.isEmpty()
            ? QSettings()
                  .value(kAgentModeSetting,
                         QSettings().value(kClaudeAutoModeSetting, true).toBool()
                             ? kClaudeAutoModeLabel
                             : kAgentAskModeLabel)
                  .toString()
                  .trimmed()
            : session.mode;
    const QString sessionStrength =
        session.strength.isEmpty() ? composerAgentStrength() : session.strength;
    session.model = selectedModel;
    session.mode = sessionMode;
    session.strength = sessionStrength;
    // Once this new native conversation has been prepared, its own session or
    // thread id carries future turns. Do not keep forcing fresh conversations
    // on ordinary follow-ups under the newly selected provider.
    if (explicitProviderHandoff)
        session.handoffFromProvider.clear();

    const qint64 runStartMs = QDateTime::currentMSecsSinceEpoch();
    session.beginRun(runStartMs); // bank the previous turn before restamping
    session.status = AgentStatus::Running;
    session.startedAtMs = runStartMs;
    session.mergeCandidateHead.clear();
    m_agentStore->saveSession(session);
    const QString startupNotice =
        session.directCheckout
            ? QStringLiteral("==> Working directly in %1 (no branch or worktree).")
                  .arg(repoPath)
            : QString::fromUtf8(
                  "==> Preparing an isolated worktree for branch %1\xE2\x80\xA6")
                  .arg(branchName);
    m_agentStore->appendLog(
        session, QLatin1Char('\n') + startupNotice + QLatin1Char('\n'));
    applyTranscriptEvent(
        sid, QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_notice")},
                         {QStringLiteral("text"), startupNotice}});

    const bool startupQuiet =
        m_startupQuietAgentSessions.remove(sid) || m_agentQuietResume;
    // The run is starting, which is the moment "listen to new agents" is
    // about. A follow-up turn counts - to the ear it is a run starting again -
    // but the quiet resumes the app does by itself at launch do not: nobody
    // started those, and a whole fleet striking up at login is not wanted.
    if (!startupQuiet)
        autoListenToNewRun(sid);
    if (!startupQuiet) {
        m_terminalSessionId = sid;
        if (switchToTab && m_selectedAgentSessionId != sid)
            switchToAgentsTab(sid);
        showAgentSession(sid); // renders the buffered turn + selects the surface
        setAgentRawOutputMode(false); // a fresh run opens on its transcript
    }
    reloadAgents();

    const bool autoMode =
        sessionMode.isEmpty()
            ? QSettings().value(kClaudeAutoModeSetting, true).toBool()
            : agentModeSkipsPermissions(sessionMode);
    const QString routeTask = lead;
    CliRuntimeLaunchPlan plan;
    plan.sessionId = sid;
    plan.prompt = prompt;
    plan.resumeFallbackPrompt = resumeFallbackPrompt;
    plan.autoMode = autoMode;
    plan.branchName = directCheckout ? baseBranch : branchName;
    plan.resumeId = resumeId;
    plan.selectedModel = selectedModel;
    plan.routeTask = routeTask;
    plan.codex = codex;
    plan.sessionMode = sessionMode;
    plan.sessionStrength = sessionStrength;
    plan.runAccountEnv = runAccountEnv;
    plan.guardrails = guardrails;
    plan.claudeImportPath = claudeImportPath;
    plan.claudeConfigDir = runAccount.configDir.isEmpty()
                               ? agentAccountDefaultConfigDir(QStringLiteral("claude-code"))
                               : runAccount.configDir;
    auto launch = [this, plan](const QString &workdir) {
        launchCliRuntime(plan, workdir);
    };
    if (directCheckout) {
        // The operator's own checkout (a .git directory, not a worktree's .git
        // file). A mirror-only repository has none to work in.
        if (!QFileInfo(repoPath + QStringLiteral("/.git")).isDir()) {
            failCliTranscriptLaunch(
                sid, QStringLiteral("This repository has no local checkout to "
                                    "run a no-worktree agent in."));
            return;
        }
        if (forkmesh::vm::active() && !forkmesh::vm::pathIsShared(repoPath)) {
            failCliTranscriptLaunch(sid, forkmesh::vm::pathError(repoPath));
            return;
        }
        // Tracked like a worktree so the run's cwd reads as ForkMesh's own;
        // cleanupStreamWorktree() refuses to delete a primary checkout.
        m_streamWorktree[sid] = directWorkdir;
        launch(directWorkdir);
        return;
    }
    startCliTranscriptWorktree(sid, repoPath, worktreeName, issueNumber,
                               branchName, baseRef, resumeId, launch);
}

// The repository's directory guardrails for a CLI run: the explicit repository
// when the caller has one, otherwise the session's own repository.
forkmesh::agentguard::Config MainWindow::cliTranscriptGuardrails(
    const AgentSession &session, const QString &repoPath,
    const RepositoryRecord *repository)
{
    forkmesh::agentguard::Config guardrails;
    const RepositoryRecord *guardedRepo = repository;
    if (!guardedRepo) {
        const int guardrailRepoIndex =
            repoIndexFor(session.owner, session.name);
        if (guardrailRepoIndex >= 0 &&
            guardrailRepoIndex < m_repositories.size())
            guardedRepo = &m_repositories.at(guardrailRepoIndex);
    }
    if (guardedRepo) {
        guardrails.enabled = guardedRepo->agentGuardrailsEnabled;
        guardrails.allowedDirectories =
            guardedRepo->agentAllowedDirectories;
    }
    guardrails.provider = session.provider;
    guardrails.repositoryRoot = repoPath;
    applyProjectAgentAccess(guardrails);
    return guardrails;
}

// Pins the commit and branch a first run forks its worktree from; later runs
// reuse the persisted baseRef and skip this entirely.
// code-health: allow(blocking-wait) bounded: only a session's first launch (empty
// persisted baseRef) runs this one local `git rev-parse` call, 8 s cap.
// processAgentQueue and the ad-hoc launchers count the run as started the moment
// startCliTranscript returns, so the lookup cannot move behind a callback.
void MainWindow::resolveCliTranscriptBase(AgentSession &session,
                                          const QString &repoPath)
{
    if (session.baseRef.isEmpty()) {
        QProcess git;
        git.setWorkingDirectory(repoPath);
        git.start(QStringLiteral("git"),
                  {QStringLiteral("rev-parse"), QStringLiteral("HEAD"),
                   QStringLiteral("--abbrev-ref"), QStringLiteral("HEAD")});
        QStringList lines;
        if (git.waitForFinished(8000) && git.exitCode() == 0)
            lines = QString::fromUtf8(git.readAllStandardOutput())
                        .split(QLatin1Char('\n'));
        session.baseRef = lines.value(0).trimmed();
        const QString b = lines.value(1).trimmed();
        if (!b.isEmpty() && b != QLatin1String("HEAD"))
            session.baseBranch = b;
    }
    if (session.baseBranch.isEmpty())
        session.baseBranch = session.baseRef;
}

void MainWindow::handleCliRuntimeEvent(
    int sid, const QJsonObject &ev,
    const forkmesh::agentguard::Config &guardrails)
{
    // applyTranscriptEvent is the common security boundary for every
    // provider.  Redacting here as well doubled a deep traversal of giant
    // tool-result events without adding protection.
    auditAgentGuardrailEvent(sid, ev, guardrails);
    if (ev.value(QStringLiteral("type")).toString() ==
            AgentRuntimeEventTypes::PermissionRequested &&
        m_agentPermissionController) {
        AgentRuntimeEvent request;
        QString error;
        if (!AgentRuntimeEvent::fromJson(ev, &request, &error)) {
            noteAgentSessionNotice(
                sid,
                QStringLiteral("Malformed runtime permission request: %1")
                    .arg(error),
                /*error=*/true);
            return;
        }
        AgentPermissionResult result =
            m_agentPermissionController->handleRequest(
                request, QString::number(sid), sessionWorkdir(sid));
        if (result.status == AgentPermissionStatus::Prompt &&
            result.request.capability ==
                AgentHostCapabilities::ProcessExecute &&
            AgentToolPolicy::deniesProcessExecution(
                AgentToolPolicy::disabledFamiliesFromOptions(
                    agentRunLaunchOptions(sid, true)))) {
            // The operator switched the Shell family off in Tool
            // efficiency; Codex cannot disable its shell itself, so the
            // host answers every command approval with a denial.
            result = m_agentPermissionController->resolve(
                QString::number(sid), request.requestId,
                AgentGrantLifetime::Once, AgentCapabilityDecision::Deny);
            noteAgentSessionNotice(
                sid,
                QStringLiteral("Shell command denied: the Shell tool family "
                               "is switched off in Tool efficiency or outside the "
                               "global tool allowlist."),
                /*error=*/false);
        }
        if (result.status == AgentPermissionStatus::Prompt &&
            result.request.capability ==
                AgentHostCapabilities::ProcessExecute) {
            // Project settings > Agents: local commands run, blocked
            // destinations are refused, the rest is asked below.
            if (const std::optional<bool> allowed = projectDecisionForCommand(
                    sid, request.data.value(QStringLiteral("command")).toString()))
                result = m_agentPermissionController->resolve(
                    QString::number(sid), request.requestId,
                    AgentGrantLifetime::Once,
                    *allowed ? AgentCapabilityDecision::Allow
                             : AgentCapabilityDecision::Deny);
        }
        if (result.status == AgentPermissionStatus::Resolved) {
            AgentRuntimeSession *live = m_codexStreams.value(sid);
            if (!live)
                live = m_streamSessions.value(sid);
            if (!live || !live->sendCommand(result.response))
                noteAgentSessionNotice(
                    sid,
                    QStringLiteral(
                        "The runtime permission response could not be delivered."),
                    /*error=*/true);
            if (!result.error.isEmpty())
                noteAgentSessionNotice(sid, result.error, /*error=*/true);
            return;
        }
        if (result.status == AgentPermissionStatus::Invalid) {
            noteAgentSessionNotice(sid, result.error, /*error=*/true);
            return;
        }
        QTimer::singleShot(0, this, [this] { ringLocalControlAgentOpsBell(); });

        QString question =
            request.data.value(QStringLiteral("description"))
                .toString()
                .left(1024)
                .trimmed();
        if (question.trimmed().isEmpty()) {
            question = QStringLiteral("Allow %1 for %2?")
                           .arg(result.request.capability,
                                result.request.resource);
        }
        const QJsonObject block{
            {QStringLiteral("type"), QStringLiteral("tool_use")},
            {QStringLiteral("id"), request.requestId},
            {QStringLiteral("name"), QStringLiteral("AskUserQuestion")},
            {QStringLiteral("input"),
             QJsonObject{{QStringLiteral("questions"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("question"), question},
                              {QStringLiteral("header"),
                               QStringLiteral("Permission")},
                              {QStringLiteral("options"),
                               QJsonArray{
                                   QJsonObject{
                                       {QStringLiteral("label"),
                                        QStringLiteral("Allow once")},
                                       {QStringLiteral("description"),
                                        QStringLiteral(
                                            "Approve only this operation")}},
                                   QJsonObject{
                                       {QStringLiteral("label"),
                                        QStringLiteral("Allow for session")},
                                       {QStringLiteral("description"),
                                        QStringLiteral(
                                            "Approve matching operations until this session ends")}},
                                   QJsonObject{
                                       {QStringLiteral("label"),
                                        QStringLiteral("Allow for worktree")},
                                       {QStringLiteral("description"),
                                        QStringLiteral(
                                            "Approve matching operations in this worktree")}},
                                   QJsonObject{
                                       {QStringLiteral("label"),
                                        QStringLiteral("Always allow")},
                                       {QStringLiteral("description"),
                                        QStringLiteral(
                                            "Save this exact permission on this device")}},
                                   QJsonObject{
                                       {QStringLiteral("label"),
                                        QStringLiteral("Deny")},
                                       {QStringLiteral("description"),
                                        QStringLiteral(
                                            "Reject this operation")}}}},
                              {QStringLiteral("multiSelect"), false}}}}}}};
        applyTranscriptEvent(
            sid,
            QJsonObject{
                {QStringLiteral("type"), QStringLiteral("assistant")},
                {QStringLiteral("session_id"), QString::number(sid)},
                {QStringLiteral("message"),
                 QJsonObject{
                     {QStringLiteral("role"),
                      QStringLiteral("assistant")},
                     {QStringLiteral("content"), QJsonArray{block}}}}});
        return;
    }
    applyTranscriptEvent(sid, ev);
    if (m_streamSessions.contains(sid))
        handleClaudeControlEvent(sid, ev);
}

bool MainWindow::desktopStopping() const
{
    return m_closingDown || forkmesh::terminationSignalReceived();
}

bool MainWindow::keepCliRunForNextStart(int sid, bool codex, int exitCode)
{
    AgentSession *as = findAgentSession(sid);
    if (!as || m_agentCompletionChecks.contains(sid) ||
        (as->status != AgentStatus::Running &&
         as->status != AgentStatus::Waiting))
        return false;
    if (m_agentPermissionController)
        m_agentPermissionController->clearSession(QString::number(sid));
    m_agentBackgroundWork.remove(sid);
    m_agentCliTasks.remove(sid);
    m_agentResultAwaitingCliTasks.remove(sid);
    m_agentRelaunchAttempts.remove(sid);
    as->lastError.clear();
    as->finishedAtMs = 0;
    if (m_agentStore)
        m_agentStore->saveSession(*as);
    noteAgentSessionNotice(
        sid,
        QStringLiteral("ForkMesh is stopping, so %1 stopped (exit %2). This "
                       "session continues when ForkMesh starts again, with or "
                       "without a window.")
            .arg(codex ? QStringLiteral("Codex app-server")
                       : QStringLiteral("Claude Code"))
            .arg(exitCode),
        /*error=*/false);
    if (codex) {
        if (CodexAppServerSession *done = m_codexStreams.take(sid))
            done->deleteLater();
    } else if (ClaudeStreamSession *done = m_streamSessions.take(sid)) {
        done->deleteLater();
    }
    return true;
}

void MainWindow::finishCliTranscriptRun(int sid, bool codex, int exitCode)
{
    if (desktopStopping() && keepCliRunForNextStart(sid, codex, exitCode)) {
        m_agentExitsSettling.remove(sid);
        return;
    }
    const AgentSession *exiting = findAgentSession(sid);
    if ((exitCode == kExitAfterSigterm || exitCode == kExitAfterSighup) && exiting &&
        (exiting->status == AgentStatus::Running ||
         exiting->status == AgentStatus::Waiting) &&
        !m_agentExitsSettling.contains(sid)) {
        m_agentExitsSettling.insert(sid);
        QObject *const exited =
            codex ? static_cast<QObject *>(m_codexStreams.value(sid))
                  : static_cast<QObject *>(m_streamSessions.value(sid));
        const QPointer<QObject> runtime = exited;
        QTimer::singleShot(kAgentExternalStopSettleMs, this,
                           [this, sid, codex, exitCode, exited, runtime] {
            const QObject *current =
                codex ? static_cast<QObject *>(m_codexStreams.value(sid))
                      : static_cast<QObject *>(m_streamSessions.value(sid));
            if ((exited && !runtime) || current != exited) {
                m_agentExitsSettling.remove(sid);
                return;
            }
            finishCliTranscriptRun(sid, codex, exitCode);
        });
        return;
    }
    m_agentExitsSettling.remove(sid);
    if (m_agentPermissionController)
        m_agentPermissionController->clearSession(QString::number(sid));
    // Whatever the run left going stops being ours to watch the moment the
    // CLI is gone: its children are reparented away and drop out of the
    // process tree we poll. Say so instead of quietly calling the run done.
    const QString stranded = agentBackgroundWorkNote(sid);
    m_agentBackgroundWork.remove(sid);
    applyAgentBackgroundWork();
    m_agentCliTasks.remove(sid);
    const bool awaitedCliTasks = m_agentResultAwaitingCliTasks.remove(sid);
    bool backupQueued = false;
    if (AgentSession *as = findAgentSession(sid)) {
        const bool completedResult =
            m_agentCompletionChecks.contains(sid) || awaitedCliTasks;
        if (completedResult) {
            m_agentCompletionChecks.remove(sid);
            if (as->status == AgentStatus::Running) {
                as->status = AgentStatus::Success;
                as->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
                as->lastError.clear();
                m_agentStore->saveSession(*as);
                updateAgentStatusCell(sid);
                notifyAgentDone(sid);
            }
        } else if (as->status == AgentStatus::Running ||
            as->status == AgentStatus::Waiting) {
            QString failure = as->lastError;
            if (m_agentStore)
                failure += QLatin1Char('\n') +
                           m_agentStore->readLog(*as).right(16 * 1024);
            failure += QLatin1Char('\n') +
                       m_streamRaw.value(sid).right(16 * 1024);
            backupQueued = queueTeammateBackupModel(sid, failure);
            if (!backupQueued)
                applyCliExitWithoutResult(sid, codex, exitCode);
        }
    }
    if (backupQueued) {
        if (codex) {
            if (CodexAppServerSession *done = m_codexStreams.take(sid))
                done->deleteLater();
        } else if (ClaudeStreamSession *done = m_streamSessions.take(sid)) {
            done->deleteLater();
        }
        reloadAgents();
        if (sid == m_selectedAgentSessionId)
            showAgentSession(sid);
        processAgentQueue();
        maybeStartQueuedRebuild();
        return;
    }
    if (!stranded.isEmpty())
        noteAgentSessionNotice(
            sid,
            QStringLiteral("The agent CLI exited with %1. Those processes "
                           "were left to the system — this session can no "
                           "longer follow them.")
                .arg(stranded),
            /*error=*/true);
    if (const AgentSession *fin = findAgentSession(sid);
        fin && (fin->yolo || fin->vibe)) {
        const bool vibe = fin->vibe;
        maybeCreatePullForStreamSession(sid, [this, sid, vibe] {
            maybeAutoMergeForSession(sid);
            completeOrgTaskForSession(sid);
            if (!vibe)
                cleanupStreamWorktree(sid);
        });
    } else {
        completeOrgTaskForSession(sid);
    }
    if (codex) {
        if (CodexAppServerSession *done = m_codexStreams.take(sid))
            done->deleteLater();
    } else if (ClaudeStreamSession *done = m_streamSessions.take(sid)) {
        done->deleteLater();
    }
    reloadAgents();
    if (sid == m_selectedAgentSessionId)
        showAgentSession(sid);
    looperOnSessionFinished(sid);
    processAgentQueue(); // pick the re-queued session back up
    maybeStartQueuedRebuild();
}

// Codex app-server reported its live model list: cache it and fold it into
// every Codex model picker that is currently showing.
void MainWindow::applyListedCodexModels(const QJsonArray &models)
{
    if (models.isEmpty())
        return;
    QSettings().setValue(
        kCodexModelsCacheSetting,
        QJsonDocument(models).toJson(QJsonDocument::Compact));
    if (m_quickAddAgentProvider && m_quickAddClaudeModel &&
        agentIsCodexProvider(
            m_quickAddAgentProvider->currentData().toString())) {
        mergeLiveCodexModels(m_quickAddClaudeModel, models);
        const QString selected = codexChatGptModelId(
            selectedModelComboValue(m_quickAddClaudeModel));
        QSettings().setValue(kCodexModelSetting, selected);
        if (m_codexModelEdit)
            m_codexModelEdit->setText(selected);
    }
    refreshAgentModelSelectors();
    if (m_branchFixAgentCombo && m_branchFixModelCombo &&
        agentIsCodexProvider(
            m_branchFixAgentCombo->currentData().toString()))
        mergeLiveCodexModels(m_branchFixModelCombo, models);
    if (m_actionFixAgentCombo && m_actionFixModelCombo &&
        agentIsCodexProvider(
            m_actionFixAgentCombo->currentData().toString()))
        mergeLiveCodexModels(m_actionFixModelCombo, models);
    if (m_issueAgentProvider && m_issueAgentModel &&
        agentIsCodexProvider(
            m_issueAgentProvider->currentData().toString()))
        mergeLiveCodexModels(m_issueAgentModel, models);
}

void MainWindow::launchCliRuntime(const CliRuntimeLaunchPlan &plan,
                                  const QString &workdir)
{
    const int sid = plan.sessionId;
    const QString &prompt = plan.prompt;
    const QString &resumeFallbackPrompt = plan.resumeFallbackPrompt;
    const bool autoMode = plan.autoMode;
    const QString &branchName = plan.branchName;
    const QString &resumeId = plan.resumeId;
    const QString &selectedModel = plan.selectedModel;
    const QString &routeTask = plan.routeTask;
    const bool codex = plan.codex;
    const QString &sessionMode = plan.sessionMode;
    const QString &sessionStrength = plan.sessionStrength;
    const QStringList &runAccountEnv = plan.runAccountEnv;
    const forkmesh::agentguard::Config &guardrails = plan.guardrails;
    if (codex) {
        CodexAppServerSession *live = m_codexStreams.value(sid);
        if (!live)
            return;
        if (AgentSession *as = findAgentSession(sid))
            m_agentStore->appendLog(
                *as,
                QStringLiteral("\n==> Running Codex app-server transcript on "
                               "branch %1 in %2\n")
                    .arg(branchName, workdir));
        const QString mode =
            sessionMode.isEmpty()
                ? (autoMode ? kClaudeAutoModeLabel
                            : kAgentAskModeLabel)
                : sessionMode;
        const QString effort = sessionStrength;
        if (AgentSession *as = findAgentSession(sid))
            m_agentStore->appendLog(
                *as, QStringLiteral("==> %1\n")
                         .arg(AgentRunner::launchIdentityInstruction(
                             kCodexProvider, selectedModel, mode, effort)));
        QStringList codexEnv = forkmesh::agentenv::codexCredentialVariables();
        codexEnv << runAccountEnv;
        codexEnv << agentIntakeEnv(sid);
        const QString scratch = forkmesh::vm::active()
                                    ? QString()
                                    : forkmesh::agentscratch::folderFor(workdir);
        codexEnv << forkmesh::agentscratch::environment(scratch, workdir);
        int jailMb = 0;
        if (QSettings().value(kAgentJailSetting, false).toBool() ||
            guardrails.enforced()) {
            if (QSettings().value(kAgentJailSetting, false).toBool())
                jailMb = agentJailMemoryMb();
            const QString jailDir =
                forkmesh::vm::active()
                    ? forkmesh::vm::worktreeRoot() +
                          QStringLiteral("/jails/s%1").arg(sid)
                    : (scratch.isEmpty() ? AgentJail::sessionJailDir(sid) : scratch);
            codexEnv << AgentJail::envEntries(jailDir);
        }
        AgentRuntimeLaunch runtimeLaunch;
        runtimeLaunch.sessionId = QString::number(sid);
        runtimeLaunch.workingDirectory = workdir;
        runtimeLaunch.environment = codexEnv;
        runtimeLaunch.initialPrompt = prompt;
        runtimeLaunch.resumeId = resumeId;
        runtimeLaunch.model = selectedModel;
        runtimeLaunch.permissionMode = mode;
        runtimeLaunch.effort = effort;
        runtimeLaunch.memoryLimitMb = jailMb;
        runtimeLaunch.resumeFallbackPrompt = resumeFallbackPrompt;
        runtimeLaunch.options = agentRunLaunchOptions(sid, true);
        noteAgentRunToolConfig(sid, runtimeLaunch.options);
        live->setGuardrails(guardrails);
        live->startRuntime(runtimeLaunch);
        return;
    }

    ClaudeStreamSession *live = m_streamSessions.value(sid);
    if (!live)
        return; // session was stopped or deleted while the worktree was building
    QStringList env = forkmesh::agentenv::claudeCredentialVariables();
    env << runAccountEnv;
    if (!forkmesh::vm::active()) {
        if (ClaudeIdeBridge *bridge = ensureIdeBridge()) {
            bridge->setRepositoryGuardrails(
                guardrails.enabled, guardrails.allowedDirectories);
            if (bridge->start(workdir))
                env << bridge->env();
        }
    }
    if (AgentSession *as = findAgentSession(sid))
        m_agentStore->appendLog(
            *as, QStringLiteral("\n==> Running Claude Code (stream-json transcript) "
                                "on branch %1 in %2\n")
                     .arg(branchName, workdir));
    auto begin = [this, sid, live, workdir, env, prompt, autoMode, resumeId,
                  resumeFallbackPrompt, sessionMode, sessionStrength, guardrails,
                  importPath = plan.claudeImportPath,
                  configDir = plan.claudeConfigDir](const QString &chosenModel) {
        if (m_streamSessions.value(sid) != live)
            return;
        if (!importPath.isEmpty() && !resumeId.isEmpty()) {
            QString why;
            if (!ClaudeSessionImport::stage(importPath, configDir, workdir, resumeId, &why))
                applyTranscriptEvent(
                    sid, QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_notice")},
                                     {QStringLiteral("level"), QStringLiteral("warning")},
                                     {QStringLiteral("text"),
                                      QStringLiteral("Could not hand the Claude Code "
                                                     "conversation to this run: %1.")
                                          .arg(why)}});
        }
        const QString effort = sessionStrength;
        if (AgentSession *as = findAgentSession(sid)) {
            as->model = chosenModel;
            m_agentStore->saveSession(*as);
        }
        const QString fallback =
            QSettings().value(kClaudeFallbackModelSetting, false).toBool()
                ? QStringLiteral("opus,sonnet")
                : QString();
        QStringList launchEnv = env;
        if (!QSettings().value(kClaudeThinkingSetting, true).toBool())
            launchEnv << QStringLiteral("MAX_THINKING_TOKENS=0");
        const QString scratch = forkmesh::vm::active()
                                    ? QString()
                                    : forkmesh::agentscratch::folderFor(workdir);
        launchEnv << forkmesh::agentscratch::environment(scratch, workdir);
        int jailMb = 0;
        if (QSettings().value(kAgentJailSetting, false).toBool() ||
            guardrails.enforced()) {
            if (QSettings().value(kAgentJailSetting, false).toBool())
                jailMb = agentJailMemoryMb();
            const QString jailDir =
                forkmesh::vm::active()
                    ? forkmesh::vm::worktreeRoot() +
                          QStringLiteral("/jails/s%1").arg(sid)
                    : (scratch.isEmpty() ? AgentJail::sessionJailDir(sid) : scratch);
            launchEnv << AgentJail::envEntries(jailDir);
        }
        if (AgentSession *as = findAgentSession(sid))
            m_agentStore->appendLog(
                *as, QStringLiteral("==> %1\n")
                         .arg(AgentRunner::launchIdentityInstruction(
                             QStringLiteral("claude-code"), chosenModel,
                             sessionMode, effort)));
        // Every label reaches the CLI, not just Auto: "Plan" launches
        // read-only and "Edit" edits-but-asks, where all three non-Auto
        // labels used to collapse into the same ask-for-everything run.
        const QString permissionMode =
            sessionMode.isEmpty()
                ? agentPermissionModeId(autoMode ? kClaudeAutoModeLabel
                                                 : kAgentAskModeLabel)
                : agentPermissionModeId(sessionMode);
        AgentRuntimeLaunch runtimeLaunch;
        runtimeLaunch.sessionId = QString::number(sid);
        runtimeLaunch.workingDirectory = workdir;
        launchEnv << agentIntakeEnv(sid);
        runtimeLaunch.environment = launchEnv;
        runtimeLaunch.initialPrompt = prompt;
        runtimeLaunch.resumeId = resumeId;
        runtimeLaunch.model = chosenModel;
        runtimeLaunch.permissionMode = permissionMode;
        runtimeLaunch.effort = effort;
        if (!fallback.isEmpty())
            runtimeLaunch.fallbackModels.append(fallback);
        runtimeLaunch.memoryLimitMb = jailMb;
        runtimeLaunch.resumeFallbackPrompt = resumeFallbackPrompt;
        runtimeLaunch.options = agentRunLaunchOptions(sid, false);
        if (!importPath.isEmpty())
            runtimeLaunch.options.insert(QStringLiteral("forkSession"), true);
        runtimeLaunch.options.insert(QStringLiteral("hostControl"), true);
        if (const AgentSession *named = findAgentSession(sid)) {
            const QString title = claudeInitialSessionTitle(*named);
            if (!title.isEmpty())
                runtimeLaunch.options.insert(QStringLiteral("sessionTitle"), title);
        }
        noteAgentRunToolConfig(sid, runtimeLaunch.options);
        const QStringList gateEnv = agentNetworkGateEnv(sid);
        QString claudeSettings;
        if (!gateEnv.isEmpty()) {
            runtimeLaunch.environment += gateEnv;
            claudeSettings = forkmesh::agentnet::claudeHookSettings(agentRunGatesCommands(sid));
        }
        claudeSettings = forkmesh::agentrules::claudeSettingsWithAllowRules(
            forkmesh::agentrules::claudeSettingsWithReadGuard(claudeSettings),
            agentRunRules(sid).allowedToolRules);
        if (!claudeSettings.isEmpty())
            runtimeLaunch.options.insert(QStringLiteral("claudeSettings"), claudeSettings);
        addAgentRunMcpConfig(sid, runtimeLaunch.options);
        live->setGuardrails(guardrails);
        live->startRuntime(runtimeLaunch);
    };
    if (selectedModel == kClaudeAutoModelId)
        resolveAutoClaudeModel(sid, routeTask, workdir, live,
                               std::move(begin), guardrails);
    else
        begin(selectedModel);
}

void MainWindow::failCliTranscriptLaunch(int sid, const QString &why)
{
    if (CodexAppServerSession *live = m_codexStreams.take(sid))
        live->deleteLater();
    if (ClaudeStreamSession *live = m_streamSessions.take(sid))
        live->deleteLater();
    if (AgentSession *as = findAgentSession(sid)) {
        as->status = AgentStatus::Failed;
        as->lastError = why;
        as->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_agentStore->appendLog(*as, QStringLiteral("!! %1\n").arg(why));
        m_agentStore->saveSession(*as);
        scheduleAgentSessionsPush();
    }
    const QJsonObject failureNotice{
        {QStringLiteral("type"), QStringLiteral("_local_notice")},
        {QStringLiteral("level"), QStringLiteral("error")},
        {QStringLiteral("text"), why}};
    applyTranscriptEvent(sid, failureNotice);
    const bool failureNoticeRendered =
        m_renderedTranscriptSession == sid &&
        m_renderedTranscriptCount == m_streamEvents.value(sid).size();
    completeOrgTaskForSession(sid);
    reloadAgents();
    if (sid == m_selectedAgentSessionId) {
        showAgentSession(sid);
        renderTranscriptForSession(sid);
        if (!failureNoticeRendered && m_agentTranscript) {
            m_agentTranscript->handleEvent(failureNotice);
            m_agentTranscript->jumpToBottom();
        }
    }
    looperOnSessionFinished(sid);
    processAgentQueue();
    maybeStartQueuedRebuild();
}

void MainWindow::copyWorktreeIncludes(
    int sid, const QString &repoPath, const QString &wtPath,
    const std::function<void(const QString &)> &launch)
{
    namespace include = forkmesh::worktreeinclude;
    if (!QFileInfo(QDir(repoPath).filePath(QLatin1String(include::kFileName))).isFile()) {
        launch(wtPath);
        return;
    }
    auto *watcher = new QFutureWatcher<include::Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, sid, watcher, wtPath, launch] {
                const include::Result result = watcher->result();
                watcher->deleteLater();
                if (AgentSession *as = findAgentSession(sid);
                    as && (result.copied > 0 || !result.errors.isEmpty())) {
                    QString line = QStringLiteral("== .worktreeinclude: copied %1 file%2")
                                       .arg(result.copied)
                                       .arg(result.copied == 1 ? QString()
                                                               : QStringLiteral("s"));
                    if (!result.errors.isEmpty())
                        line += QStringLiteral("; could not copy %1")
                                    .arg(result.errors.join(QStringLiteral(", ")));
                    m_agentStore->appendLog(*as, line + QLatin1Char('\n'));
                }
                if (!m_streamSessions.value(sid) && !m_codexStreams.value(sid))
                    return;
                launch(wtPath);
            });
    watcher->setFuture(QtConcurrent::run(
        [repoPath, wtPath] { return include::apply(repoPath, wtPath); }));
}

void MainWindow::startCliTranscriptWorktree(
    int sid, const QString &repoPath, const QString &worktreeName,
    int issueNumber, const QString &branchName, const QString &baseRef,
    const QString &resumeId, std::function<void(const QString &)> launch)
{
    auto failLaunch = [this, sid](const QString &why) {
        failCliTranscriptLaunch(sid, why);
    };
    if (baseRef.isEmpty()) {
        failLaunch(QStringLiteral(
            "Could not resolve a base commit to fork the agent's worktree "
            "from — does the repository have a commit yet?"));
        return;
    }
    if (forkmesh::vm::active() && !forkmesh::vm::pathIsShared(repoPath)) {
        failLaunch(forkmesh::vm::pathError(repoPath));
        return;
    }
    const QString wtRoot = forkmesh::vm::active()
                               ? forkmesh::vm::worktreeRoot()
                               : forkmesh::agentwt::root(repoPath);
    forkmesh::agentwt::ensureRoot(wtRoot);
    const QString wtPath = QDir(wtRoot).filePath(worktreeName);
    const QString legacyPath =
        (forkmesh::vm::active()
             ? forkmesh::vm::worktreeRoot()
             : QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                   QStringLiteral("/forkmesh-worktrees")) +
        QStringLiteral("/issue-%1-s%2").arg(issueNumber).arg(sid);
    const QString script = forkmesh::agentwt::addScript(
        wtPath, branchName, baseRef, !resumeId.isEmpty());
    auto runAdd = std::make_shared<std::function<void(int)>>();
    auto firstFailure = std::make_shared<QString>();
    *runAdd = [this, sid, wtPath, repoPath, script, launch, failLaunch,
               runAdd, firstFailure](int attemptsLeft) {
        auto *add = new QProcess(this);
        add->setWorkingDirectory(repoPath);
        connect(add, &QProcess::finished, this,
                [this, sid, add, wtPath, repoPath, script, launch, failLaunch,
                 runAdd, firstFailure, attemptsLeft](int, QProcess::ExitStatus) {
                    const QString out = QString::fromUtf8(
                                            add->readAllStandardOutput())
                                            .trimmed();
                    const QString err = QString::fromUtf8(
                                            add->readAllStandardError())
                                            .trimmed();
                    QStringList diagnostic;
                    if (!out.isEmpty())
                        diagnostic << out;
                    if (!err.isEmpty())
                        diagnostic << err;
                    add->deleteLater();
                    if (QDir(wtPath).exists()) {
                        m_streamWorktree[sid] = wtPath;
                        m_taskWorkItemSessionWorktree.insert(sid, wtPath);
                        attachOrgTaskWorkItemForSession(sid);
                        copyWorktreeIncludes(sid, repoPath, wtPath, launch);
                        return;
                    }
                    if (!m_streamSessions.value(sid) && !m_codexStreams.value(sid))
                        return;
                    const QString failure = diagnostic.join(QLatin1Char('\n'));
                    if (firstFailure->isEmpty())
                        *firstFailure = failure;
                    else if (!failure.isEmpty() && failure != *firstFailure)
                        diagnostic.prepend(
                            QStringLiteral("first attempt: %1\nlast attempt:")
                                .arg(*firstFailure));
                    if (attemptsLeft > 0) {
                        QTimer::singleShot(400, this,
                                           [runAdd, attemptsLeft] {
                                               (*runAdd)(attemptsLeft - 1);
                                           });
                        return;
                    }
                    failLaunch(
                        QStringLiteral("Could not create the agent's worktree "
                                       "at %1%2")
                            .arg(wtPath, diagnostic.isEmpty()
                                             ? QStringLiteral(".")
                                             : QStringLiteral(": %1").arg(
                                                   diagnostic.join(QLatin1Char('\n')))));
                });
        add->start(QStringLiteral("bash"), {QStringLiteral("-lc"), script});
    };
    auto worktreeOnSessionBranch = [&branchName](const QString &path) -> bool {
        QFile dotGit(path + QStringLiteral("/.git"));
        if (!dotGit.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        QString gitDir = QString::fromUtf8(dotGit.readLine()).trimmed();
        if (!gitDir.startsWith(QLatin1String("gitdir:")))
            return false;
        gitDir = gitDir.mid(7).trimmed();
        if (QDir::isRelativePath(gitDir))
            gitDir = QDir(path).absoluteFilePath(gitDir);
        QFile head(gitDir + QStringLiteral("/HEAD"));
        if (!head.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        return QString::fromUtf8(head.readLine()).trimmed() ==
               QStringLiteral("ref: refs/heads/") + branchName;
    };
    if (QThread *teardown = m_worktreeTeardown.value(sid);
        teardown && teardown->isRunning()) {
        connect(teardown, &QThread::finished, this, [runAdd] { (*runAdd)(3); });
    } else if (worktreeOnSessionBranch(wtPath)) {
        m_streamWorktree[sid] = wtPath;
        launch(wtPath);
    } else if (wtPath != legacyPath && worktreeOnSessionBranch(legacyPath)) {
        m_streamWorktree[sid] = legacyPath;
        launch(legacyPath);
    } else {
        (*runAdd)(3);
    }
}

QString MainWindow::sessionWorkdir(int sessionId)
{
    if (m_streamWorktree.contains(sessionId))
        return m_streamWorktree.value(sessionId);
    if (const AgentSession *s = findAgentSession(sessionId)) {
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri >= 0) {
            const QString repoLocal = m_repositories.at(ri).localPath;
            const QString wt =
                cachedSessionWorktree(sessionId, repoLocal, s->branchName);
            if (!wt.isEmpty() && QDir(wt).exists())
                return wt;
            return repoLocal;
        }
    }
    return QString();
}

QString MainWindow::cachedSessionWorktree(int sessionId, const QString &repoLocal,
                                          const QString &branch)
{
    if (repoLocal.isEmpty() || branch.trimmed().isEmpty())
        return QString();
    auto live = m_streamWorktree.constFind(sessionId);
    if (live != m_streamWorktree.constEnd())
        return *live;
    auto cached = m_sessionWorkdirCache.constFind(sessionId);
    if (cached != m_sessionWorkdirCache.constEnd()
        && (cached->isEmpty() || QDir(*cached).exists()))
        return *cached;
    const QString wt = worktreePathForBranch(repoLocal, branch);
    m_sessionWorkdirCache.insert(sessionId, wt);
    return wt;
}

QString MainWindow::cachedSessionWorktreeForUi(int sessionId,
                                                const QString &repoLocal,
                                                const QString &branch)
{
    if (repoLocal.isEmpty() || branch.trimmed().isEmpty())
        return QString();
    const auto live = m_streamWorktree.constFind(sessionId);
    if (live != m_streamWorktree.constEnd())
        return *live;
    const auto cached = m_sessionWorkdirCache.constFind(sessionId);
    if (cached != m_sessionWorkdirCache.constEnd() &&
        (cached->isEmpty() || QDir(*cached).exists()))
        return *cached;
    if (m_sessionWorkdirUiReads.contains(sessionId))
        return QString();

    m_sessionWorkdirUiReads.insert(sessionId);
    runOffThread<QString>(
        [repoLocal, branch] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("resolve selected agent worktree"),
                forkmesh::ActionTelemetry::Execution::Worker);
            return backgroundWorktrees(repoLocal).value(branch);
        },
        [this, sessionId, repoLocal, branch](QString worktree) {
            m_sessionWorkdirUiReads.remove(sessionId);
            const AgentSession *session = findAgentSession(sessionId);
            if (!session || session->branchName != branch)
                return;
            const int repoIndex = repoIndexFor(session->owner, session->name);
            if (repoIndex < 0 ||
                m_repositories.at(repoIndex).localPath != repoLocal)
                return;
            m_sessionWorkdirCache.insert(sessionId, worktree);
            if (sessionId == m_selectedAgentSessionId) {
                refreshAgentDetailMeta(sessionId);
                updateAgentFilesTabState(sessionId);
            }
        });
    return QString();
}

bool MainWindow::isStreamTranscriptSession(int sessionId) const
{
    return m_streamSessions.contains(sessionId) || m_codexStreams.contains(sessionId) ||
           m_streamEvents.contains(sessionId);
}

void MainWindow::stopErroredStreamTransport(int sessionId)
{
#ifdef FORKMESH_WINDOW_TESTS
    m_testLiveTransportSessions.remove(sessionId);
#endif
    if (ClaudeStreamSession *stream = m_streamSessions.take(sessionId)) {
        stream->stop();
        stream->deleteLater();
    }
    if (CodexAppServerSession *codex = m_codexStreams.take(sessionId)) {
        codex->interrupt();
        QTimer::singleShot(150, codex, [codex] {
            codex->stop();
            codex->deleteLater();
        });
    }
}

void MainWindow::stopStreamSession(int sessionId, bool refreshUi,
                                   const QString &reason)
{
    ClaudeStreamSession *stream = m_streamSessions.take(sessionId);
    CodexAppServerSession *codex = m_codexStreams.take(sessionId);
    if (!stream && !codex)
        return;
    if (stream) {
        stream->stop();
        stream->deleteLater();
    }
    if (codex) {
        codex->interrupt();
        QTimer::singleShot(150, codex, [codex] {
            codex->stop();
            codex->deleteLater();
        });
    }

    const QString stopReason = reason.trimmed().isEmpty()
                                   ? QStringLiteral("Stop requested by user.")
                                   : reason.trimmed();
    QString &raw = m_streamRaw[sessionId];
    raw += QStringLiteral("\n==> %1\n").arg(stopReason);
    if (sessionId == m_selectedAgentSessionId)
        appendAgentRawLog(QStringLiteral("\n==> %1\n").arg(stopReason));

    if (AgentSession *as = findAgentSession(sessionId);
        as && (as->status == AgentStatus::Running ||
               as->status == AgentStatus::Waiting)) {
        as->status = AgentStatus::Stopped;
        as->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        if (m_agentStore)
            m_agentStore->saveSession(*as);
        scheduleAgentSessionsPush();
    }

    if (refreshUi) {
        // A stop changes a status, not a worktree: repaint from what is loaded.
        // A rescan would re-read every agent's merge state and diff in its own
        // worktree, which is git work nobody asked for (and lit every one of them
        // up in the activity window).
        reloadAgents(AgentReloadScan::FromCache);
        if (sessionId == m_selectedAgentSessionId)
            showAgentSession(sessionId);
        updateAgentActionState();
    }
}

QList<int> MainWindow::stoppableAgentSessionIds() const
{
    QList<int> ids;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.merged || isExternalSession(session.id))
            continue;
        if (session.status == AgentStatus::Running ||
            session.status == AgentStatus::Waiting ||
            session.status == AgentStatus::Queued)
            ids << session.id;
    }
    return ids;
}

void MainWindow::stopAllRunningAgents()
{
    const QList<int> ids = stoppableAgentSessionIds();
    if (ids.isEmpty()) {
        flashMessage(QStringLiteral("No agents are running."));
        return;
    }
    m_agentQueue.clear();
    stopAgentSessions(ids);
    flashMessage(QStringLiteral("Stopped %1 agent session%2.")
                     .arg(ids.size())
                     .arg(ids.size() == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::stopAgentSessions(const QList<int> &ids)
{
    for (const int sessionId : std::as_const(ids)) {
        m_agentQueue.removeAll(sessionId);
        if (AgentRunner *runner = runnerForSession(sessionId))
            runner->stop();
        stopStreamSession(sessionId, /*refreshUi=*/false);
        if (AgentSession *as = findAgentSession(sessionId);
            as && m_agentStore &&
            (as->status == AgentStatus::Queued ||
             as->status == AgentStatus::Running ||
             as->status == AgentStatus::Waiting)) {
            as->status = AgentStatus::Stopped;
            as->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
            m_agentStore->saveSession(*as);
        }
    }
    scheduleAgentSessionsPush();
    // A stop changes a status, not a worktree: repaint from what is loaded.
    // A rescan would re-read every agent's merge state and diff in its own
    // worktree, which is git work nobody asked for (and lit every one of them
    // up in the activity window).
    reloadAgents(AgentReloadScan::FromCache);
    if (m_selectedAgentSessionId > 0)
        showAgentSession(m_selectedAgentSessionId);
    updateAgentActionState();
}

bool MainWindow::isStoppableAgentSession(int sessionId) const
{
    const AgentSession *session = nullptr;
    for (const AgentSession &candidate : std::as_const(m_agentSessions)) {
        if (candidate.id == sessionId) {
            session = &candidate;
            break;
        }
    }
    if (!session || session->merged)
        return false;
    return session->status == AgentStatus::Running ||
           session->status == AgentStatus::Waiting ||
           session->status == AgentStatus::Queued;
}

qint64 MainWindow::agentSessionLastLiveMs(int sessionId) const
{
    qint64 liveAt = m_scannerStates.value(sessionId).lastActivityMs;
    for (const AgentSession &session : std::as_const(m_agentSessions))
        if (session.id == sessionId) {
            liveAt = qMax(liveAt, session.startedAtMs);
            break;
        }
    return liveAt;
}

bool MainWindow::agentSessionWorkInFlight(int sessionId) const
{
    if (sessionId <= 0)
        return false;
    const AgentSession *session = nullptr;
    for (const AgentSession &candidate : std::as_const(m_agentSessions))
        if (candidate.id == sessionId) {
            session = &candidate;
            break;
        }
    if (session && session->merged)
        return false;
    const QString status = session ? session->status
                                   : m_streamSessionInfo.value(sessionId).status;
    if (status != AgentStatus::Running && status != AgentStatus::Queued)
        return false;
    if (isExternalSession(sessionId))
        return true;
    if (m_agentQueue.contains(sessionId))
        return true;
    if (runnerForSession(sessionId)) // headless AgentRunner mid-run
        return true;
    ClaudeStreamSession *claude = m_streamSessions.value(sessionId);
    CodexAppServerSession *codex = m_codexStreams.value(sessionId);
    const bool claudeLive = claude && claude->running();
    const bool codexLive = codex && codex->running() && codex->turnActive();
    if (!claudeLive && !codexLive)
        return false;
    const qint64 liveAt = agentSessionLastLiveMs(sessionId);
    return liveAt > 0 &&
           (QDateTime::currentMSecsSinceEpoch() - liveAt) < kAgentSilentStaleMs;
}

bool MainWindow::stopAgentSessionById(int sessionId)
{
    if (sessionId <= 0 || !isStoppableAgentSession(sessionId))
        return false;
    if (isExternalSession(sessionId)) {
        stopExternalSession(sessionId);
        return true;
    }
    m_agentQueue.removeAll(sessionId);
    if (AgentRunner *runner = runnerForSession(sessionId))
        runner->stop();
    stopStreamSession(sessionId, /*refreshUi=*/false);
    if (AgentSession *as = findAgentSession(sessionId);
        as && (as->status == AgentStatus::Queued ||
               as->status == AgentStatus::Running ||
               as->status == AgentStatus::Waiting)) {
        as->status = AgentStatus::Stopped;
        as->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        if (m_agentStore)
            m_agentStore->saveSession(*as);
    }
    scheduleAgentSessionsPush();
    // A stop changes a status, not a worktree: repaint from what is loaded.
    // A rescan would re-read every agent's merge state and diff in its own
    // worktree, which is git work nobody asked for (and lit every one of them
    // up in the activity window).
    reloadAgents(AgentReloadScan::FromCache);
    if (sessionId == m_selectedAgentSessionId)
        showAgentSession(sessionId);
    updateAgentActionState();
    return true;
}

void MainWindow::setRepoAgentAllowedDirectories(const QStringList &directories)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    RepositoryRecord &repo = m_repositories[m_repoDetailIndex];
    const QStringList next =
        forkmesh::agentguard::normalizeDirectories(directories);
    if (next != repo.agentAllowedDirectories) {
        const bool narrowed = forkmesh::agentguard::policyNarrowed(
            repo.agentAllowedDirectories, next);
        repo.agentAllowedDirectories = next;
        saveRepositories();
        const QString owner = repo.owner;
        const QString name = repo.name;
        if (repo.agentGuardrailsEnabled && narrowed) {
            const int stopped =
                stopRepositoryAgentsForGuardrailChange(owner, name);
            if (stopped > 0)
                flashMessage(
                    QStringLiteral(
                        "Stopped %1 active agent process%2 so the narrower "
                        "repository boundary takes effect immediately.")
                        .arg(stopped)
                        .arg(stopped == 1 ? QString() : QStringLiteral("s")));
        }
    }
    scheduleAgentGuardrailRefresh();
    refreshRepoSettings();
}

int MainWindow::stopRepositoryAgentsForGuardrailChange(
    const QString &owner, const QString &name)
{
    QList<int> sessions;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.owner == owner && session.name == name &&
            !isExternalSession(session.id) &&
            (session.status == AgentStatus::Running ||
             session.status == AgentStatus::Waiting))
            sessions.append(session.id);
    }

    int stopped = 0;
    for (int sessionId : std::as_const(sessions))
        if (stopAgentSessionById(sessionId))
            ++stopped;

    for (QDialog *dialog : findChildren<QDialog *>()) {
        // A closed terminal lingers hidden until its deferred delete runs;
        // it is no longer an agent process and must not be counted again.
        if (!dialog->isVisible())
            continue;
        if (dialog->property("forkmeshRepoOwner").toString() == owner &&
            dialog->property("forkmeshRepoName").toString() == name) {
            dialog->close();
            ++stopped;
        }
    }
    if (stopped > 0 && m_agentGuardrailLedger)
        m_agentGuardrailLedger->recordAgentsStopped(
            owner + QLatin1Char('/') + name, stopped);
    return stopped;
}

QList<int> MainWindow::startableAgentSessionIds() const
{
    QList<int> ids;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.merged || isExternalSession(session.id))
            continue;
        if (runnerForSession(session.id)) // still winding down from a stop
            continue;
        if (session.status == AgentStatus::Stopped ||
            session.status == AgentStatus::Failed)
            ids << session.id;
    }
    return ids;
}

// Which one-click control the end of the row offers while the pointer is on it.
// Deliberately the same population Stop all / Start all act on, so
// the per-row button never does something the pair above the list would not:
// merged, provenance-only and externally started sessions stay untouchable,
// and a session that finished cleanly is continued from its detail pane rather
// than by a stray click on the list.
int MainWindow::agentRunToggleMode(const AgentSession &session) const
{
    if (session.id <= 0 || session.merged || session.associationOnly)
        return AgentRunToggleNone;
    if (isStoppableAgentSession(session.id))
        return AgentRunToggleStop;
    if (isExternalSession(session.id)) // watched from outside; nothing to resume
        return AgentRunToggleNone;
    if (runnerForSession(session.id)) // still winding down from a stop
        return AgentRunToggleNone;
    if (session.status == AgentStatus::Stopped ||
        session.status == AgentStatus::Failed)
        return AgentRunToggleStart;
    return AgentRunToggleNone;
}

void MainWindow::toggleAgentSessionRun(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    const int mode = agentRunToggleMode(*session);
    if (mode == AgentRunToggleStop) {
        if (stopAgentSessionById(sessionId))
            flashMessage(QStringLiteral("Agent #%1 stopped.").arg(sessionId));
        return;
    }
    if (mode != AgentRunToggleStart)
        return;
    continueAgentSession(sessionId);
    // The queue may have been full, in which case the click parked it rather
    // than starting it — say which of the two happened.
    const AgentSession *resumed = findAgentSession(sessionId);
    if (resumed && resumed->status == AgentStatus::Queued)
        flashMessage(QStringLiteral("Agent #%1 queued — it starts when a run "
                                    "slot frees up.")
                         .arg(sessionId));
    else
        flashMessage(QStringLiteral("Agent #%1 started.").arg(sessionId));
}

void MainWindow::startAllStoppedAgents()
{
    const QList<int> ids = startableAgentSessionIds();
    if (ids.isEmpty()) {
        flashMessage(QStringLiteral("No stopped agents to start."));
        return;
    }
    startAgentSessions(ids);
    flashMessage(QStringLiteral("Started %1 agent session%2.")
                     .arg(ids.size())
                     .arg(ids.size() == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::startAgentSessions(const QList<int> &ids)
{
    for (const int sessionId : std::as_const(ids))
        continueAgentSession(sessionId, /*deferRefresh=*/true);
    scheduleAgentSessionsPush();
    reloadAgents();
    if (m_selectedAgentSessionId > 0)
        showAgentSession(m_selectedAgentSessionId);
    processAgentQueue();
    updateAgentActionState();
}

QList<int> MainWindow::updatableAgentSessionIds() const
{
    QList<int> ids;
    QSet<QString> seen;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.merged || session.associationOnly ||
            isExternalSession(session.id))
            continue;
        if (session.branchName.isEmpty() ||
            session.branchName == agentMergeBase(session))
            continue;
        const int repoIndex = repoIndexFor(session.owner, session.name);
        if (repoIndex < 0 || m_repositories.at(repoIndex).localPath.isEmpty())
            continue;
        const QString key = m_repositories.at(repoIndex).localPath +
                            QLatin1Char('\n') + session.branchName;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        ids << session.id;
    }
    return ids;
}

void MainWindow::updateAllAgentWorktreesFromMain()
{
    if (m_updateAllAgentWorktreesRunning) {
        flashMessage(QStringLiteral("Agent branches are already being updated."));
        return;
    }
    const QList<int> ids = updatableAgentSessionIds();
    if (ids.isEmpty()) {
        flashMessage(QStringLiteral("No agent branches to update."));
        return;
    }
    QList<AgentWorktreeUpdateJob> jobs;
    int unresolved = 0;
    for (const int sessionId : std::as_const(ids)) {
        const AgentSession *session = findAgentSession(sessionId);
        const int repoIndex =
            session ? repoIndexFor(session->owner, session->name) : -1;
        if (repoIndex < 0) {
            ++unresolved;
            continue;
        }
        AgentWorktreeUpdateJob job;
        job.repoPath = m_repositories.at(repoIndex).localPath;
        job.branch = session->branchName;
        job.base = agentMergeBase(*session);
        jobs << job;
    }
    m_updateAllAgentWorktreesRunning = true;
    const int total = ids.size();
    runOffThread<AgentWorktreeUpdateSummary>(
        [jobs, unresolved] {
            return updateAgentWorktreesFromBase(jobs, unresolved);
        },
        [this, total](AgentWorktreeUpdateSummary summary) {
            applyAgentWorktreesUpdate(total, summary);
        });
}

MainWindow::AgentWorktreeUpdateSummary MainWindow::updateAgentWorktreesFromBase(
    const QList<AgentWorktreeUpdateJob> &jobs, int unresolved)
{
    AgentWorktreeUpdateSummary summary;
    summary.skipped = unresolved;
    for (const AgentWorktreeUpdateJob &job : jobs) {
        const QString worktree =
            readWorktreePathForBranch(job.repoPath, job.branch);
        if (worktree.isEmpty() || !QDir(worktree).exists()) {
            ++summary.skipped;
            continue;
        }
        if (!runGitCapture(worktree,
                           {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                            QStringLiteral("--quiet"),
                            QStringLiteral("refs/heads/%1").arg(job.base)},
                           nullptr, nullptr)) {
            ++summary.skipped;
            continue;
        }
        if (runGitCapture(worktree,
                          {QStringLiteral("merge-base"),
                           QStringLiteral("--is-ancestor"), job.base,
                           QStringLiteral("HEAD")},
                          nullptr, nullptr)) {
            ++summary.current;
            continue;
        }
        const WorktreeMergeReport report =
            mergeBaseIntoLinkedWorktree(worktree, job.branch, job.base);
        summary.logs << report.logs;
        switch (report.status) {
        case WorktreeMergeReport::Merged:
            ++summary.updated;
            break;
        case WorktreeMergeReport::Busy:
            ++summary.busy;
            break;
        case WorktreeMergeReport::Conflicted:
        case WorktreeMergeReport::Failed:
            ++summary.conflicted;
            break;
        }
    }
    return summary;
}

void MainWindow::applyAgentWorktreesUpdate(
    int total, const AgentWorktreeUpdateSummary &summary)
{
    m_updateAllAgentWorktreesRunning = false;
    for (const QString &line : summary.logs)
        logSystem(line);
    const int updated = summary.updated;
    QStringList notes;
    if (summary.current > 0)
        notes << QStringLiteral("%1 already current").arg(summary.current);
    if (summary.busy > 0)
        notes << QStringLiteral("%1 mid-merge").arg(summary.busy);
    if (summary.conflicted > 0)
        notes << QStringLiteral("%1 conflicting").arg(summary.conflicted);
    if (summary.skipped > 0)
        notes << QStringLiteral("%1 without a worktree").arg(summary.skipped);
    flashMessage(
        QStringLiteral("Updated %1 agent branch%2%3.")
            .arg(updated)
            .arg(updated == 1 ? QString() : QStringLiteral("es"),
                 notes.isEmpty() ? QString()
                                 : QStringLiteral(" (%1 skipped: %2)")
                                       .arg(total - updated)
                                       .arg(notes.join(QStringLiteral(", ")))),
        /*isError=*/false, QString(),
        forkmesh::kLocalOperationNoticeKind);
    // The one button that goes back to Git for the whole list, so it measures
    // every row rather than only the ones whose branch tip moved: dropping the
    // signatures is what defeats that skip.
    m_agentDiffSig.clear();
    m_agentDiffRefreshPending = true;
    reloadAgents(AgentReloadScan::Rescan);
    if (m_selectedAgentSessionId > 0 &&
        findAgentSession(m_selectedAgentSessionId))
        refreshAgentFilesPanel(m_selectedAgentSessionId);
    loadWorktreesPanel();
    updateAgentActionState();
}

QSet<QString> MainWindow::ownStreamCwds() const
{
    QSet<QString> out;
    for (const QString &p : m_streamWorktree)
        out.insert(QDir(p).absolutePath());
    return out;
}

void MainWindow::retireStreamCwd(const QString &path)
{
    if (path.trimmed().isEmpty() || QFileInfo(path + QStringLiteral("/.git")).isDir())
        return;
    m_retiredStreamCwds.insert(QDir(path).absolutePath());
}

int MainWindow::externalTempIdFor(const QString &uuid)
{
    auto it = m_externalTempId.constFind(uuid);
    if (it != m_externalTempId.constEnd())
        return it.value();
    const int id = m_nextExternalTempId--; // -1000, -1001, … (all <= kExternalIdBase)
    m_externalTempId.insert(uuid, id);
    return id;
}

void MainWindow::scanExternalClaudeSessions()
{
    if (m_externalClaudeScanInFlight)
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    if (repo.localPath.isEmpty())
        return;
    constexpr qint64 kActiveWindowMs = 90 * 1000; // written within 90s == "running"
    const QString repoKey = repo.owner + QLatin1Char('/') + repo.name;
    const QSet<QString> streamCwds = ownStreamCwds() | m_retiredStreamCwds;
    QStringList accountDirs;
    for (const AgentAccountProfile &profile :
         agentAccountProfiles(QStringLiteral("claude-code")))
        if (!profile.configDir.trimmed().isEmpty())
            accountDirs.append(profile.configDir);
    const quint64 generation = ++m_externalClaudeScanGeneration;
    m_externalClaudeScanInFlight = true;
    runOffThread<QList<ExternalClaudeSession>>(
        [repoPath = repo.localPath, streamCwds, accountDirs,
         activeWindowMs = kActiveWindowMs]() {
            return ClaudeSessionScan::scan(repoPath, streamCwds,
                                           activeWindowMs, accountDirs);
        },
        [this, generation, repoKey](QList<ExternalClaudeSession> sessions) {
            if (generation != m_externalClaudeScanGeneration)
                return;
            m_externalClaudeScanInFlight = false;
            if (m_repoDetailIndex < 0 ||
                m_repoDetailIndex >= m_repositories.size())
                return;
            const RepositoryRecord &current =
                m_repositories.at(m_repoDetailIndex);
            if (current.owner + QLatin1Char('/') + current.name != repoKey)
                return;
            if (QSettings().value(kExcludeExternalClaudeSetting, true).toBool())
                return;
            const QHash<QString, int> imported = importedClaudeSessionAgents();
            if (!imported.isEmpty())
                sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                              [&imported](const ExternalClaudeSession &s) {
                                                  return imported.contains(s.uuid);
                                              }),
                               sessions.end());
            m_externalClaude = std::move(sessions);
            for (const ExternalClaudeSession &ext :
                 std::as_const(m_externalClaude)) {
                const int id = m_externalTempId.value(ext.uuid, 0);
                if (id != 0 && m_externalSurfaced.contains(id)) {
                    ExternalClaudeSession &saved = m_externalSurfaced[id];
                    saved.lastActivityMs = ext.lastActivityMs;
                    saved.path = ext.path;
                    if (!ext.title.isEmpty())
                        saved.title = ext.title;
                }
            }
            applyExternalClaudeScan();
        });
}

void MainWindow::applyExternalClaudeScan()
{
    for (const ExternalClaudeSession &ext : std::as_const(m_externalClaude)) {
        registerExternalSession(ext.uuid);
    }

    QStringList sig;
    for (const ExternalClaudeSession &e : std::as_const(m_externalClaude))
        sig << e.uuid;
    sig.sort();
    QStringList surfSig;
    for (auto it = m_externalSurfaced.constBegin();
         it != m_externalSurfaced.constEnd(); ++it)
        surfSig << QStringLiteral("%1:%2")
                       .arg(it.key())
                       .arg(externalIsLive(it.value().uuid));
    surfSig.sort();
    const QString combined = sig.join(QLatin1Char(',')) + QLatin1Char('|') +
                             surfSig.join(QLatin1Char(','));
    if (combined != m_externalSig) {
        m_externalSig = combined;
        injectExternalSessions();
        refreshAgentTable();
        updateAgentsTabIndicator();
        notifyLocalControlAgentsChanged();
    }

    if (isExternalSession(m_selectedAgentSessionId) &&
        m_externalSurfaced.contains(m_selectedAgentSessionId))
        renderExternalTranscript(m_selectedAgentSessionId, /*full=*/false);
    scheduleSessionsPageRefresh();
}

bool MainWindow::externalIsLive(const QString &uuid) const
{
    if (m_externalStopped.contains(uuid))
        return false;
    for (const ExternalClaudeSession &e : m_externalClaude)
        if (e.uuid == uuid)
            return true;
    for (const ClaudePeerSession &peer : m_claudePeerSessions)
        if (peer.sessionId == uuid)
            return true;
    return false;
}

void MainWindow::injectExternalSessions()
{
    m_agentSessions.erase(std::remove_if(m_agentSessions.begin(), m_agentSessions.end(),
                                         [](const AgentSession &s) {
                                             return s.id <= kExternalIdBase;
                                         }),
                          m_agentSessions.end());
    for (auto it = m_externalSurfaced.constBegin(); it != m_externalSurfaced.constEnd();
         ++it) {
        const int id = it.key();
        const ExternalClaudeSession &ext = it.value();
        const QString repoKey = m_externalSurfacedRepo.value(id);
        const int slash = repoKey.indexOf(QLatin1Char('/'));
        AgentSession s;
        s.id = id;
        s.owner = repoKey.left(slash);
        s.name = repoKey.mid(slash + 1);
        s.provider = QStringLiteral("claude-code");
        s.issueNumber = 0;
        s.issueTitle = ext.title.isEmpty() ? QStringLiteral("Claude Code (external)")
                                           : ext.title;
        s.branchName = ext.gitBranch;
        s.status = externalIsLive(ext.uuid) ? AgentStatus::Running : AgentStatus::Success;
        s.createdAtMs = ext.lastActivityMs;
        m_agentSessions.append(s);
    }
    // Cloud rows live in the same band and were dropped with it above.
    injectCloudAgentSessions();
}

int MainWindow::registerExternalSession(const QString &uuid)
{
    const ExternalClaudeSession *found = nullptr;
    for (const ExternalClaudeSession &e : std::as_const(m_externalClaude))
        if (e.uuid == uuid) {
            found = &e;
            break;
        }
    if (!found || m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return 0;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (!m_externalTempId.contains(uuid)) {
        const ClaudePeerSession *peer = ClaudePeerMessaging::find(m_claudePeerSessions, {}, uuid);
        for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it) {
            if (it->kind == QLatin1String("claude") && !m_externalSurfaced.contains(it.key()) &&
                ((peer && peer->pid == it->pid) || (!it->transcript.isEmpty() && it->transcript == found->path))) {
                m_externalTempId.insert(uuid, it.key());
                break;
            }
        }
    }
    const int id = externalTempIdFor(uuid);
    if (!m_externalSurfaced.contains(id)) {
        m_externalSurfaced.insert(id, *found);
        m_externalSurfacedRepo.insert(id, repo.owner + QLatin1Char('/') + repo.name);
        m_externalSig.clear(); // a new row — force a rebuild
        fileExternalSessionInFolder(AgentFolders::repoKey(repo.owner, repo.name),
                                    uuid);
    } else {
        m_externalSurfaced[id] = *found; // refresh metadata in place
    }
    return id;
}

void MainWindow::fileExternalSessionInFolder(const QString &repoKey,
                                             const QString &uuid)
{
    if (uuid.isEmpty())
        return;
    const QString member = kExternalClaudeMemberPrefix + uuid;
    if (!m_agentFolders.folderOf(repoKey, member).isEmpty())
        return;
    const QString folderId =
        m_agentFolders.findOrCreate(repoKey, kExternalClaudeFolderName);
    if (folderId.isEmpty())
        return;
    QSet<QString> surfaced;
    for (auto it = m_externalSurfaced.cbegin(); it != m_externalSurfaced.cend();
         ++it)
        if (m_externalSurfacedRepo.value(it.key()) == repoKey &&
            !it->uuid.isEmpty())
            surfaced.insert(kExternalClaudeMemberPrefix + it->uuid);
    for (const QString &stale : m_agentFolders.members(repoKey, folderId))
        if (stale.startsWith(kExternalClaudeMemberPrefix) &&
            !surfaced.contains(stale))
            m_agentFolders.assign(repoKey, stale, QString());
    m_agentFolders.assign(repoKey, member, folderId);
    if (m_agentFoldersSaveQueued)
        return;
    m_agentFoldersSaveQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_agentFoldersSaveQueued = false;
        m_agentFolders.save();
    });
}

void MainWindow::surfaceExternalSession(const QString &uuid)
{
    const int id = registerExternalSession(uuid);
    if (id == 0)
        return;
    m_externalReadOffset.remove(id); // fresh render on select
    injectExternalSessions();        // so findAgentSession(id) resolves before the switch
    switchToAgentsTab(id);           // shows the Agents tab + renders the detail
    if (m_agentTable) {
        for (int r = 0; r < m_agentTable->rowCount(); ++r) {
            QTableWidgetItem *cell = m_agentTable->item(r, 0);
            if (cell && cell->data(Qt::UserRole).toInt() == id) {
                m_agentTable->selectRow(r);
                break;
            }
        }
    }
}

void MainWindow::unsurfaceExternalSession(int sessionId)
{
    if (!isExternalSession(sessionId))
        return;
    m_externalSurfaced.remove(sessionId);
    m_externalSurfacedRepo.remove(sessionId);
    m_externalReadOffset.remove(sessionId);
    m_externalTranscriptFullQueued.remove(sessionId);
    if (m_externalTranscriptRenderSession == sessionId) {
        ++m_externalTranscriptRenderGeneration;
        m_externalTranscriptRenderSession = -1;
        m_externalTranscriptRenderEvents.clear();
        if (m_agentTranscript)
            m_agentTranscript->setBulkPopulate(false);
    }
    m_externalSig.clear();
    if (m_selectedAgentSessionId == sessionId)
        m_selectedAgentSessionId = -1;
    updateQuickAddTargetAgentLabel();
    reloadAgents();
}

void MainWindow::killExternalSessionPids(const QList<qint64> &pids)
{
#ifdef Q_OS_WIN
    // Windows has no SIGTERM equivalent for an unrelated console process.
    // Open only the termination right and close every native handle promptly.
    for (const qint64 pid : pids) {
        if (pid <= 0 || pid > std::numeric_limits<DWORD>::max())
            continue;
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
                                     static_cast<DWORD>(pid));
        if (!process)
            continue;
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
#else
    for (const qint64 pid : pids)
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    QTimer::singleShot(4000, this, [pids] {
        for (const qint64 pid : pids)
            if (::kill(static_cast<pid_t>(pid), 0) == 0)
                ::kill(static_cast<pid_t>(pid), SIGKILL);
    });
#endif
}

void MainWindow::stopExternalSession(int sessionId)
{
    auto it = m_externalSurfaced.constFind(sessionId);
    if (it == m_externalSurfaced.constEnd())
        return;
    const ExternalClaudeSession ext = it.value();
    const QList<qint64> pids = ClaudeSessionScan::findSessionPids(ext.uuid, ext.cwd);
    if (pids.isEmpty()) {
        flashMessage(QStringLiteral("Couldn't find the external Claude Code process "
                                    "to stop — it may have already exited."),
                     /*error=*/true);
        return;
    }
    if (QMessageBox::warning(
            this, QStringLiteral("Stop external Claude Code"),
            QStringLiteral("Stop the Claude Code session running in\n%1?\n\n"
                           "This ends a process ForkMesh didn't start.").arg(ext.cwd),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    killExternalSessionPids(pids);
    m_externalStopped.insert(ext.uuid);
    m_externalSig.clear();
    flashMessage(QStringLiteral("Stopping external Claude Code session…"));
    // Nothing in git moved: repaint, do not rescan every agent's worktree.
    reloadAgents(AgentReloadScan::FromCache);
    updateAgentActionState();
}

void MainWindow::deleteExternalSession(int sessionId)
{
    auto it = m_externalSurfaced.constFind(sessionId);
    if (it == m_externalSurfaced.constEnd())
        return;
    const ExternalClaudeSession ext = it.value();
    if (externalIsLive(ext.uuid)) {
        const QList<qint64> pids = ClaudeSessionScan::findSessionPids(ext.uuid, ext.cwd);
        if (!pids.isEmpty()) {
            if (QMessageBox::warning(
                    this, QStringLiteral("Delete external Claude Code session"),
                    QStringLiteral("This Claude Code session is still running in\n%1.\n\n"
                                   "Deleting it will stop that process completely "
                                   "(ForkMesh didn't start it). Continue?").arg(ext.cwd),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            killExternalSessionPids(pids);
            m_externalStopped.insert(ext.uuid);
        }
    }
    unsurfaceExternalSession(sessionId);
}

void MainWindow::onExternalClaudeTick()
{
    if (!m_agentStore)
        return;
    syncImportedClaudeSessions();
    if (QSettings().value(kExcludeExternalClaudeSetting, true).toBool()) {
        if (m_externalClaudeScanInFlight) {
            ++m_externalClaudeScanGeneration;
            m_externalClaudeScanInFlight = false;
        }
        if (!m_externalClaude.isEmpty() || !m_externalSurfaced.isEmpty()) {
            m_externalClaude.clear();
            m_externalSurfaced.clear();
            m_externalSurfacedRepo.clear();
            m_externalReadOffset.clear();
            m_externalSig.clear();
            if (isExternalSession(m_selectedAgentSessionId))
                m_selectedAgentSessionId = -1;
            updateQuickAddTargetAgentLabel();
            injectExternalSessions();
            refreshAgentTable();
            updateAgentsTabIndicator();
        }
        return;
    }
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        if (m_externalClaudeScanInFlight) {
            ++m_externalClaudeScanGeneration;
            m_externalClaudeScanInFlight = false;
        }
        if (!m_externalClaude.isEmpty()) {
            m_externalClaude.clear();
            updateAgentsTabIndicator();
        }
        return;
    }
    scanExternalClaudeSessions();
}

void MainWindow::renderExternalTranscript(int sessionId, bool full)
{
    if (!m_agentTranscript)
        return;
    auto it = m_externalSurfaced.constFind(sessionId);
    if (it == m_externalSurfaced.constEnd())
        return;
    const ExternalClaudeSession ext = it.value();

    if (m_externalTranscriptReads.contains(sessionId)) {
        if (full && !m_externalTranscriptReadFull.value(sessionId))
            m_externalTranscriptFullQueued.insert(sessionId);
        return;
    }
    if (m_externalTranscriptRenderSession == sessionId) {
        if (full && !m_externalTranscriptRenderFull)
            m_externalTranscriptFullQueued.insert(sessionId);
        return;
    }

    const bool redraw = full && m_renderedExternalSession == sessionId &&
                        m_renderedCloudSession != sessionId;
    if (full) {
        ++m_agentTranscriptRenderGeneration;
        m_agentTranscriptRenderSession = -1;
        ++m_externalTranscriptRenderGeneration;
        m_externalTranscriptRenderSession = -1;
        m_externalTranscriptRenderEvents.clear();
        if (!redraw) {
            m_agentTranscript->clear();
            m_agentTranscript->setBulkPopulate(false);
        }
        m_renderedTranscriptSession = -1;
        m_renderedExternalSession = sessionId;
    }

    const qint64 offset = full ? -1 : m_externalReadOffset.value(sessionId, 0);
    const QStringList providerCredentialValues =
        localProviderCredentialValues();
    m_externalTranscriptReads.insert(sessionId);
    m_externalTranscriptReadFull.insert(sessionId, full);
    runOffThread<ExternalTranscriptRead>(
        [ext, offset, full, redraw, providerCredentialValues] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("read external agent transcript"),
                forkmesh::ActionTelemetry::Execution::Worker);
            ExternalTranscriptRead result;
            result.full = full;
            result.redraw = redraw;
            const qint64 start =
                full ? ClaudeSessionScan::tailStartOffset(ext.path, 400 * 1024)
                     : offset;
            QList<QJsonObject> unsafeEvents =
                ClaudeSessionScan::readEvents(ext.path, start,
                                              &result.newOffset);
            result.events.reserve(unsafeEvents.size());
            forkmesh::transcripttools::OutputTokenCounter outputTokens;
            for (const QJsonObject &unsafeEvent : std::as_const(unsafeEvents)) {
                const QJsonObject event =
                    redactProviderCredentials(QJsonValue(unsafeEvent),
                                              providerCredentialValues)
                        .toObject();
                if (event.value(QStringLiteral("type")).toString() ==
                    QLatin1String("assistant")) {
                    result.addedTokens += outputTokens.add(
                        event.value(QStringLiteral("message")).toObject());
                }
                result.events.append(event);
            }
            if (full) {
                QFile file(ext.path);
                if (file.open(QIODevice::ReadOnly)) {
                    file.seek(start);
                    result.raw =
                        redactProviderCredentials(
                            QString::fromUtf8(file.readAll()),
                            providerCredentialValues)
                            .split(QLatin1Char('\n'), Qt::SkipEmptyParts)
                            .join(QStringLiteral("\n\n"));
                }
            }
            return result;
        },
        [this, sessionId, path = ext.path](ExternalTranscriptRead result) {
            m_externalTranscriptReads.remove(sessionId);
            m_externalTranscriptReadFull.remove(sessionId);
            const auto current = m_externalSurfaced.constFind(sessionId);
            if (current == m_externalSurfaced.constEnd() ||
                current->path != path) {
                m_externalTranscriptFullQueued.remove(sessionId);
                return;
            }
            m_externalReadOffset[sessionId] = result.newOffset;
            if (!result.events.isEmpty())
                noteAgentActivity(sessionId, result.events.size() * 200);
            if (sessionId != m_selectedAgentSessionId ||
                !isExternalSession(sessionId)) {
                m_externalTranscriptFullQueued.remove(sessionId);
                return;
            }

            const quint64 generation = ++m_externalTranscriptRenderGeneration;
            m_externalTranscriptRenderSession = sessionId;
            m_externalTranscriptRenderEvents = std::move(result.events);
            m_externalTranscriptRenderCursor = 0;
            m_externalTranscriptRenderFull = result.full;
            m_externalTranscriptRenderTokens = result.addedTokens;
            m_externalTranscriptRenderRaw = std::move(result.raw);
            if (result.full)
                m_agentTranscript->setBatchHoldsFrame(result.redraw, false);
            m_agentTranscript->setBulkPopulate(result.full);
            if (result.redraw)
                m_agentTranscript->clear();
            else if (result.full)
                m_agentTranscript->jumpToBottom();
            renderExternalTranscriptSlice(generation);
        });
}

void MainWindow::renderExternalTranscriptSlice(quint64 generation)
{
    if (!m_agentTranscript ||
        generation != m_externalTranscriptRenderGeneration)
        return;
    const int sessionId = m_externalTranscriptRenderSession;
    if (sessionId != m_selectedAgentSessionId || !isExternalSession(sessionId)) {
        m_agentTranscript->setBulkPopulate(false);
        m_externalTranscriptRenderSession = -1;
        m_externalTranscriptRenderEvents.clear();
        return;
    }

    QElapsedTimer budget;
    budget.start();
    bool handled = false;
    while (m_externalTranscriptRenderCursor <
               m_externalTranscriptRenderEvents.size() &&
           (!handled || budget.elapsed() < 8)) {
        const QJsonObject &event = m_externalTranscriptRenderEvents.at(
            m_externalTranscriptRenderCursor++);
        if (event.value(QStringLiteral("isSidechain")).toBool() ||
            event.value(QStringLiteral("isMeta")).toBool()) {
            handled = true;
            continue;
        }
        if (event.value(QStringLiteral("type")).toString() ==
            QLatin1String("user")) {
            const QJsonValue content =
                event.value(QStringLiteral("message")).toObject()
                    .value(QStringLiteral("content"));
            if (content.isString()) {
                if (!content.toString().trimmed().isEmpty() &&
                    !AgentLogText::isInjectedPrompt(content.toString()))
                    m_agentTranscript->addUserTurn(
                        content.toString(),
                        ClaudeTranscriptView::eventTime(event));
            } else {
                for (const QJsonValue &blockValue : content.toArray()) {
                    const QJsonObject block = blockValue.toObject();
                    if (block.value(QStringLiteral("type")).toString() ==
                        QLatin1String("text")) {
                        const QString text =
                            block.value(QStringLiteral("text")).toString();
                        if (!text.trimmed().isEmpty() &&
                            !AgentLogText::isInjectedPrompt(text))
                            m_agentTranscript->addUserTurn(
                                text, ClaudeTranscriptView::eventTime(event));
                    }
                }
            }
        }
        m_agentTranscript->handleEvent(event);
        handled = true;
    }
    if (m_externalTranscriptRenderCursor <
        m_externalTranscriptRenderEvents.size()) {
        QTimer::singleShot(
            0, this,
            [this, generation] { renderExternalTranscriptSlice(generation); });
        return;
    }

    const bool full = m_externalTranscriptRenderFull;
    const qint64 addedTokens = m_externalTranscriptRenderTokens;
    const QString raw = std::move(m_externalTranscriptRenderRaw);
    m_agentTranscript->setBulkPopulate(false);
    m_renderedExternalSession = sessionId;
    m_externalTranscriptRenderSession = -1;
    m_externalTranscriptRenderEvents.clear();
    if (full)
        m_sessionTokens[sessionId] =
            qMax(m_sessionTokens.value(sessionId), addedTokens);
    else if (addedTokens > 0)
        m_sessionTokens[sessionId] += addedTokens;
    if (full || addedTokens > 0)
        updateAgentTokenCell(sessionId);
    if (full && m_agentLog)
        setAgentLogText(sessionId, raw);
    if (full)
        reapplyTranscriptSearch();

    if (m_externalTranscriptFullQueued.remove(sessionId))
        renderExternalTranscript(sessionId, /*full=*/true);
}

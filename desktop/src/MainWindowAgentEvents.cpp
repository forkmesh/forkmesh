
#include "MainWindow.h"
#include "AgentActivityFeed.h"
#include "AgentLogText.h"

#include "AgentSonifier.h"
#include "AgentAnnouncer.h"
#include "MainWindowInternal.h"
#include "AccountCapability.h"
#include "ForkMeshPackageRegistry.h"
#include "RepoStatsStore.h"
#include "AgentCliLocator.h"
#include "AgentJail.h"
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

QString agentUsageWindowLabel(const QString &windowKey)
{
    if (windowKey == QLatin1String("5h"))
        return QStringLiteral("5-hour");
    if (windowKey == QLatin1String("weekly"))
        return QStringLiteral("weekly");
    if (windowKey == QLatin1String("fable"))
        return QStringLiteral("Fable weekly");
    return windowKey;
}

QString agentUsageLimitCountdownText(const AgentSession &session)
{
    const QString providerKey =
        agentIsClaudeProvider(session.provider)
            ? QStringLiteral("claude")
            : (agentIsCodexProvider(session.provider)
                   ? QStringLiteral("codex")
                   : QString());
    if (providerKey.isEmpty())
        return QString();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSettings settings;
    QString bestLabel;
    qint64 bestRemaining = 0;

    auto considerWindow = [&](const QString &windowKey) {
        const QString exhaustedKey = usageExhaustedSetting(providerKey, windowKey);
        if (exhaustedKey.isEmpty() || !settings.value(exhaustedKey).toBool())
            return;
        const QString resetKey = usageResetSetting(providerKey, windowKey);
        const qint64 resetAt = settings.value(resetKey).toLongLong();
        if (resetAt <= 0)
            return;
        const qint64 remaining = resetAt - now;
        if (remaining <= 0)
            return;
        if (bestRemaining == 0 || remaining < bestRemaining) {
            bestRemaining = remaining;
            bestLabel = agentUsageWindowLabel(windowKey);
        }
    };

    considerWindow(QStringLiteral("5h"));
    considerWindow(QStringLiteral("weekly"));
    if (providerKey == QLatin1String("claude"))
        considerWindow(QStringLiteral("fable"));

    if (bestRemaining <= 0)
        return QString();
    return QStringLiteral("%1 usage limit reached · resets in %2")
        .arg(bestLabel, humanizeRemaining(bestRemaining));
}

QString agentSuccessOutcomeText(const AgentSession &session)
{
    QStringList parts;
    if (session.numTurns > 0)
        parts << QStringLiteral("%1 turns").arg(session.numTurns);
    if (session.durationMs > 0)
        parts << QStringLiteral("%1s").arg(session.durationMs / 1000);
    if (session.totalTokens > 0)
        parts << QStringLiteral("%1 tokens")
                     .arg(formatCount(session.totalTokens));
    if (session.costUsd > 0.0)
        parts << agentCostText(session.costUsd);
    if (parts.isEmpty())
        return QStringLiteral("Done");
    return QStringLiteral("Done · %1").arg(parts.join(QStringLiteral(" · ")));
}

QString agentStatusPillText(const AgentSession &session)
{
    const QString flat = agentStatusBadgeText(session);
    const int at = flat.indexOf(QLatin1String("is: "));
    if (at < 0)
        return flat;
    QString wrapped = flat;
    wrapped.replace(at, 4, QStringLiteral("is:\n"));
    return wrapped;
}

constexpr int kMaxAgentRelaunchAttempts = 2;

// Degrees the fireball's head travels in one 60 ms frame: quiet agents idle at
// 11 and busy ones top out at 24.  Always positive, so the orbit only ever
// carries the head forward.
qreal agentSpinStepDegrees(double meter)
{
    return 11.0 + 13.0 * qBound(0.0, meter, 1.0);
}

// Share of the gap to the live activity meter one frame closes.  At 60 ms a
// frame that eases onto a new speed over about a third of a second: quick
// enough to read as "this one just got busy", slow enough that the change of
// speed is never itself a jolt.
constexpr double kAgentSpinMeterEase = 0.18;

// Frames of orbit one animation tick is worth.  Clamped at the top so a GUI
// thread that stalled resumes with a fast sweep rather than a jump across the
// dial, and at the bottom so back-to-back repaints still creep forward.
constexpr qreal kAgentSpinMinFrames = 0.25;

constexpr qreal kAgentSpinMaxFrames = 2.0;

// Sessions start spread around the dial so a wall of running agents does not
// pulse in lockstep.
qreal agentSpinSeedDegrees(const AgentSession &session)
{
    return std::fmod(qreal(session.id % 29) * 7.0, 360.0);
}

// The little state disc stamped over the pill's avatar: the same glyph the
// session list draws on its rows, so "what is this agent doing" reads the same
// in both places.
QPair<QString, QColor> agentStatusPillBadge(const AgentSession &session)
{
    if (session.merged || session.status == AgentStatus::Success)
        return {QStringLiteral("check-circle"), QColor("#3fb950")};
    if (session.status == AgentStatus::Running)
        return {QStringLiteral("sync"), agentRunningFireballColor()};
    if (session.status == AgentStatus::Waiting)
        return {QStringLiteral("hand"), QColor("#d29922")};
    if (session.status == AgentStatus::Queued)
        return {QStringLiteral("history"), Theme::remapColor(QColor("#8b949e"))};
    if (session.status == AgentStatus::Failed)
        return {QStringLiteral("x"), QColor("#f85149")};
    if (session.status == AgentStatus::Stopped)
        return {QStringLiteral("stop"), QColor("#f85149")};
    return {QString(), QColor()};
}

QIcon agentStatusPillIcon(const AgentSession &session,
                          qreal activityAngle = 0.0)
{
    const QIcon avatar = agentStatusModelIcon(session);
    const QPair<QString, QColor> badge = agentStatusPillBadge(session);
    if (badge.first.isEmpty())
        return avatar;

    constexpr int kBadgePx = 20;
    QPixmap out = crispIconPixmap(kAgentStatusPillIconPx, kAgentStatusPillIconPx,
                                  iconDevicePixelRatio());
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawPixmap(QRect(0, 0, kAgentStatusPillIconPx, kAgentStatusPillIconPx),
                avatar.pixmap(kAgentStatusPillIconPx, kAgentStatusPillIconPx));
    const QRect badgeRect(kAgentStatusPillIconPx - kBadgePx,
                          kAgentStatusPillIconPx - kBadgePx, kBadgePx, kBadgePx);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::remapColor(QColor("#0d1117")));
    p.drawEllipse(badgeRect.adjusted(-2, -2, 0, 0));
    if (!session.merged && session.status == AgentStatus::Running) {
        p.drawPixmap(badgeRect.topLeft(),
                     agentRunningFireballPixmap(kBadgePx, activityAngle));
    } else {
        themedOcticon(badge.first, badge.second, kBadgePx).paint(&p, badgeRect);
    }
    p.end();
    return QIcon(out);
}

QString agentStatusBadgeTone(const AgentSession &session)
{
    if (session.merged || session.status == AgentStatus::Success)
        return QStringLiteral("success");
    if (session.status == AgentStatus::Failed ||
        session.status == AgentStatus::Stopped)
        return QStringLiteral("failure");
    if (session.status == AgentStatus::Running ||
        session.status == AgentStatus::Waiting ||
        session.status == AgentStatus::Queued)
        return QStringLiteral("pending");
    return QStringLiteral("neutral");
}

QString agentStatusBadgeToolTip(const AgentSession &session)
{
    QStringList details;
    details << agentStatusBadgeText(session);
    if (session.awaitingStart())
        details << (session.heldBy.isEmpty()
                        ? QStringLiteral("Paused: this agent waits until you start it. "
                                         "Right-click it and choose Start now, or "
                                         "press Start.")
                        : QStringLiteral("Paused: %1 created this agent, and it waits "
                                         "until you start it. Right-click it and "
                                         "choose Start now, or press Start.")
                              .arg(session.heldBy));
    if (session.finishingBackgroundWork())
        details << QStringLiteral("The turn is over, but %1 background task%2 "
                                  "started here %3 still %4: %5")
                       .arg(session.backgroundProcesses)
                       .arg(session.backgroundProcesses == 1
                                ? QString()
                                : QStringLiteral("s"),
                            session.backgroundProcesses == 1
                                ? QStringLiteral("is")
                                : QStringLiteral("are"),
                            session.backgroundProcessesBusy
                                ? QStringLiteral("working")
                                : QStringLiteral("open"),
                            session.backgroundProcessSummary);
    if (session.merged)
        details << QStringLiteral("Merged into %1").arg(agentMergeBase(session));
    if (session.status == AgentStatus::Success &&
        (session.numTurns > 0 || session.durationMs > 0 ||
         session.totalTokens > 0 || session.costUsd > 0.0))
        details << QStringLiteral("Brief stats: %1")
                       .arg(agentSuccessOutcomeText(session).mid(
                           QStringLiteral("Done · ").size()));
    else if (session.status == AgentStatus::Failed &&
             !session.lastError.trimmed().isEmpty())
        details << QStringLiteral("Failure reason: %1").arg(session.lastError.trimmed());
    if (const QString usageLine = agentUsageLimitCountdownText(session);
        !usageLine.isEmpty())
        details << usageLine;
    details << QStringLiteral("Hover or click to show session details.");
    return details.join(QLatin1Char('\n'));
}

// Events only a working run produces: a model turn, the tool results fed back
// into one, the token-by-token deltas of either. Bookkeeping a finished run can
// still trail (usage totals, rate-limit notices), the local echoes the window
// writes itself (_local_user, _local_notice) and `result` — the thing that ends
// a turn — are deliberately not here. See the retraction in
// applyTranscriptEvent(): anything in this set arriving after a session was
// called done proves the claim wrong.
static bool agentEventShowsWorkInFlight(const QString &type)
{
    return type == QLatin1String("assistant") || type == QLatin1String("user") ||
           type == QLatin1String("stream_event") ||
           type == QLatin1String("control_request") ||
           type == QLatin1String("_codex_agent_delta") ||
           type == QLatin1String("_codex_agent_complete") ||
           type == QLatin1String("_codex_reasoning_complete") ||
           type == QLatin1String("_codex_tool_delta");
}

static constexpr qint64 kScannerIdleMs = 1500;

// The first seconds are polled tightly so a run whose tree is already empty
// lands on Success immediately; after that the poll relaxes, because scanning
// /proc four times a second for the length of a build is pure GUI-thread cost.
static constexpr int kAgentCompletionFastPolls = 20; // 20 * 250ms = 5s

static constexpr int kAgentCompletionFastPollMs = 250;

static constexpr int kAgentCompletionSlowPollMs = 1000;

// A tree that has not consumed a single CPU tick for this long is a straggler
// (a parked dev server, an orphaned sleep), not work in progress: stop it and
// finish the run, which is what this check has always done at a flat 10s.
static constexpr qint64 kAgentCompletionIdleGraceMs = 8000;

// Work that really is running gets to run, but not forever.
static constexpr qint64 kAgentCompletionMaxWaitMs = 15 * 60 * 1000;

// How long the stop signal gets to take effect before we complete anyway.
static constexpr qint64 kAgentCompletionKillGraceMs = 10000;

static constexpr int kAgentCliTaskSettleMs = 30000;

} // namespace

bool MainWindow::approveAgentGuardrailDirectory(int sessionId,
                                                const QString &directory)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session || directory.trimmed().isEmpty())
        return false;
    const int repositoryIndex = repoIndexFor(session->owner, session->name);
    if (repositoryIndex < 0 || repositoryIndex >= m_repositories.size())
        return false;

    RepositoryRecord &repository = m_repositories[repositoryIndex];
    if (!repository.agentGuardrailsEnabled)
        return false;
    const QStringList expanded = forkmesh::agentguard::normalizeDirectories(
        repository.agentAllowedDirectories + QStringList{directory});
    if (!expanded.contains(directory) &&
        !forkmesh::agentguard::pathAllowed(
            m_streamWorktree.value(sessionId), expanded, directory))
        return false;

    const bool changed = expanded != repository.agentAllowedDirectories;
    const QString scope =
        directory == QLatin1String(".")
            ? QStringLiteral("the whole repository")
            : QStringLiteral("directory '%1'").arg(directory);
    if (!changed) {
        noteAgentSessionNotice(
            sessionId,
            QStringLiteral("Agent access to %1 is already allowed; the "
                           "repository boundary was not changed.")
                .arg(scope));
        return true;
    }

    repository.agentAllowedDirectories = expanded;
    saveRepositories();
    if (m_repoDetailIndex == repositoryIndex)
        refreshRepoSettings();

    const bool live = agentSessionHasLiveTransport(sessionId);
    noteAgentSessionNotice(
        sessionId,
        live ? QStringLiteral(
                   "Approved %1 for this repository. ForkMesh is restarting "
                   "the agent so the expanded operating-system boundary takes "
                   "effect, then it will retry the blocked work.")
                   .arg(scope)
             : QStringLiteral(
                   "Approved %1 for this repository. The expanded boundary "
                   "will apply the next time an agent starts.")
                   .arg(scope));
    flashMessage(QStringLiteral("Approved agent access to %1.").arg(scope));
    if (!live)
        return true;

    queueAgentSteerMessage(
        sessionId,
        QStringLiteral(
            "The repository owner approved access to %1. Retry the blocked "
            "operation and continue the task.")
            .arg(scope));
    stopStreamSession(
        sessionId, /*refreshUi=*/false,
        QStringLiteral("Restarting to apply approved guardrail access."));

    // Landlock policies are intentionally monotonic. Give the old provider's
    // terminate/kill window time to finish before reusing its worktree, then
    // resume the same native conversation under the newly expanded policy.
    QTimer::singleShot(1700, this, [this, sessionId] {
        AgentSession *stopped = findAgentSession(sessionId);
        if (!stopped || stopped->status != AgentStatus::Stopped ||
            agentSessionHasLiveTransport(sessionId))
            return;
        continueAgentSession(sessionId);
    });
    return true;
}

void MainWindow::showAgentGuardrailAlert(
    int sessionId, const QString &tool, const QString &path,
    const QString &directory)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    const int repositoryIndex = repoIndexFor(session->owner, session->name);
    if (repositoryIndex < 0 || repositoryIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repository = m_repositories.at(repositoryIndex);
    if (!repository.agentGuardrailsEnabled)
        return;
    const QString worktree = m_streamWorktree.value(sessionId);
    if (forkmesh::agentguard::pathAllowed(
            worktree, repository.agentAllowedDirectories, path))
        return; // another approval covered this queued/replayed attempt

    auto *alert = new QMessageBox(QMessageBox::Warning,
                                  QStringLiteral("Agent access blocked"),
                                  QString(), QMessageBox::NoButton, this);
    alert->setObjectName(QStringLiteral("agentGuardrailBoundaryAlert"));
    alert->setAttribute(Qt::WA_DeleteOnClose);
    alert->setWindowModality(Qt::ApplicationModal);
    alert->setTextFormat(Qt::PlainText);
    alert->setText(QStringLiteral(
        "An agent tried to access a path outside this repository's boundary."));
    const QString actor = tool.trimmed().isEmpty()
                              ? QStringLiteral("The agent")
                              : tool.trimmed();
    QString explanation =
        QStringLiteral("%1 requested access to:\n%2\n\nForkMesh blocked the "
                       "tool call.")
            .arg(actor, path.left(1000));
    if (directory.isEmpty()) {
        explanation += QStringLiteral(
            "\n\nThis path resolves outside the repository, so a repository "
            "guardrail cannot approve it. Deny leaves the boundary unchanged.");
    } else {
        const QString scope =
            directory == QLatin1String(".")
                ? QStringLiteral("the whole repository")
                : QStringLiteral("'%1'").arg(directory);
        explanation += QStringLiteral(
            "\n\nApprove adds %1 to this repository's allowlist and restarts "
            "the agent under the expanded boundary. Deny leaves it blocked.")
                           .arg(scope);
    }
    alert->setInformativeText(explanation);
    alert->setDetailedText(
        QStringLiteral("Currently allowed: %1")
            .arg(forkmesh::agentguard::policyDescription(
                repository.agentAllowedDirectories)));

    QPushButton *approve = alert->addButton(QStringLiteral("Approve"),
                                            QMessageBox::AcceptRole);
    QPushButton *deny = alert->addButton(QStringLiteral("Deny"),
                                         QMessageBox::RejectRole);
    approve->setObjectName(QStringLiteral("agentGuardrailApproveButton"));
    deny->setObjectName(QStringLiteral("agentGuardrailDenyButton"));
    approve->setEnabled(!directory.isEmpty());
    if (directory.isEmpty())
        approve->setToolTip(
            QStringLiteral("Only directories inside this repository can be "
                           "added to its guardrail policy."));
    alert->setDefaultButton(deny);
    alert->setEscapeButton(deny);

    QPointer<QMessageBox> guard(alert);
    const QString repositoryKey =
        repository.owner + QLatin1Char('/') + repository.name;
    connect(alert, &QMessageBox::finished, this,
            [this, guard, approve, sessionId, path, directory,
             repositoryKey](int) {
                if (!guard)
                    return;
                if (guard->clickedButton() == approve) {
                    const bool approved =
                        approveAgentGuardrailDirectory(sessionId, directory);
                    if (approved && m_agentGuardrailLedger)
                        m_agentGuardrailLedger->recordDecision(repositoryKey,
                                                               true);
                    if (!approved)
                        noteAgentSessionNotice(
                            sessionId,
                            QStringLiteral(
                                "Could not approve guardrail access to %1; the "
                                "repository boundary remains unchanged.")
                                .arg(path.left(300)),
                            /*error=*/true);
                    return;
                }
                if (m_agentGuardrailLedger)
                    m_agentGuardrailLedger->recordDecision(repositoryKey, false);
                noteAgentSessionNotice(
                    sessionId,
                    QStringLiteral("Denied agent access to %1. The repository "
                                   "boundary was not changed.")
                        .arg(path.left(300)));
            });
    alert->open();
    alert->raise();
    alert->activateWindow();
}

void MainWindow::auditAgentGuardrailEvent(
    int sessionId, const QJsonObject &event,
    const forkmesh::agentguard::Config &guardrails)
{
    if (!guardrails.enabled ||
        event.value(QStringLiteral("type")).toString() !=
            QLatin1String("assistant"))
        return;
    const QString worktree = m_streamWorktree.value(sessionId);
    if (worktree.isEmpty())
        return;
    const QJsonArray content = event.value(QStringLiteral("message"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() !=
            QLatin1String("tool_use"))
            continue;
        const QString tool = block.value(QStringLiteral("name")).toString();
        const QString path = forkmesh::agentguard::toolViolation(
            tool, block.value(QStringLiteral("input")).toObject(), worktree,
            guardrails.allowedDirectories);
        if (path.isEmpty())
            continue;
        const QString notice =
            QStringLiteral(
                "Repository guardrail blocked %1 from accessing %2. "
                "Allowed directories: %3.")
                .arg(tool.isEmpty() ? QStringLiteral("a tool call") : tool,
                     path.left(300),
                     forkmesh::agentguard::policyDescription(
                         guardrails.allowedDirectories));
        applyTranscriptEvent(
            sessionId,
            QJsonObject{
                {QStringLiteral("type"), QStringLiteral("_local_notice")},
                {QStringLiteral("level"), QStringLiteral("warning")},
                {QStringLiteral("text"), notice}});
        const QString toolUseId = block.value(QStringLiteral("id")).toString();
        const QString alertKey =
            QStringLiteral("%1:%2")
                .arg(sessionId)
                .arg(toolUseId.isEmpty()
                         ? tool + QLatin1Char(':') + path
                         : toolUseId);
        if (m_agentGuardrailAlertedToolUses.contains(alertKey))
            continue;
        m_agentGuardrailAlertedToolUses.insert(alertKey);
        const QString directory = forkmesh::agentguard::approvalDirectory(
            worktree, path);
        if (m_agentGuardrailLedger) {
            if (const AgentSession *session = findAgentSession(sessionId))
                m_agentGuardrailLedger->recordBlocked(
                    session->owner + QLatin1Char('/') + session->name, tool,
                    path, directory.isEmpty());
        }
        QTimer::singleShot(
            0, this,
            [this, sessionId, tool, path, directory] {
                showAgentGuardrailAlert(sessionId, tool, path, directory);
            });
    }
}

QString MainWindow::agentTaskProgress(int sessionId)
{
    auto tracker = m_agentTaskTrackers.find(sessionId);
    if (tracker == m_agentTaskTrackers.end()) {
        const auto events = m_streamEvents.constFind(sessionId);
        if (events == m_streamEvents.constEnd())
            return {};
        tracker = m_agentTaskTrackers.insert(sessionId, {});
        for (const QJsonObject &event : *events)
            feedAgentTaskTracker(*tracker, event);
    }
    return forkmesh::agenttasks::progressText(*tracker);
}

void MainWindow::applyTranscriptEvent(int sessionId, const QJsonObject &event)
{
    // Agent output is untrusted data. Strip provider credentials before it can
    // enter a UI buffer, the persistent transcript store, an owner-sealed sync
    // snapshot, or a copyable raw-output surface. This is intentionally at the
    // common ingestion point so future stream providers cannot bypass it.
    QJsonObject ev =
        redactProviderCredentials(
            QJsonValue(event), localProviderCredentialValues())
            .toObject();
    if (const QString accountId = m_streamAccountId.value(sessionId);
        !accountId.isEmpty() &&
        (!ev.value(QStringLiteral("session_id")).toString().isEmpty() ||
         !ev.value(QStringLiteral("thread_id")).toString().isEmpty())) {
        ev.insert(forkmesh::agents::resumeAccountKey(), accountId);
    }
    const QString type = ev.value(QStringLiteral("type")).toString();
    if (auto tracker = m_agentTaskTrackers.find(sessionId);
        tracker != m_agentTaskTrackers.end())
        feedAgentTaskTracker(*tracker, ev);
    // The speaker rows caption their bubble with when it was said; the CLIs'
    // stream events carry no time, so stamp them as they arrive (and persist).
    if ((type == QLatin1String("_local_user") || type == QLatin1String("assistant") ||
         type == QLatin1String("_codex_agent_delta") ||
         type == QLatin1String("_codex_agent_complete") ||
         (type == QLatin1String("stream_event") &&
          ev.value(QStringLiteral("event")).toObject().value(QStringLiteral("type")) ==
              QLatin1String("message_start"))) &&
        !ev.contains(QStringLiteral("_ts")))
        ev.insert(QStringLiteral("_ts"),
                  double(QDateTime::currentMSecsSinceEpoch()));
    m_streamEvents[sessionId].append(ev);
    if (m_sessionsUi)
        noteSessionsPageEvent(sessionId, ev);
    // Every run's events pass through here; the sonifier ignores the ones
    // nobody switched Listen on for.
    if (m_agentSonifier)
        m_agentSonifier->handleEvent(sessionId, ev);
    if (m_agentCommentator && !m_agentAnnouncerClosing)
        m_agentCommentator->handleEvent(sessionId, ev,
                                        QDateTime::currentMSecsSinceEpoch());
    forkmesh::agentfeed::publishAgentEvent(sessionId, ev);
    recordAgentToolEfficiency(sessionId, ev);
    if (const auto streamInfo = m_streamSessionInfo.constFind(sessionId);
        m_agentStore && streamInfo != m_streamSessionInfo.constEnd()) {
        m_agentStore->appendEvent(*streamInfo, ev);
        const QString etype = ev.value(QStringLiteral("type")).toString();
        QString logText;
        if (etype == QLatin1String("_local_user")) {
            const QString t = ev.value(QStringLiteral("text")).toString().trimmed();
            if (!t.isEmpty())
                logText = QStringLiteral("\n> ") + t + QLatin1Char('\n');
        } else if (etype == QLatin1String("_codex_agent_complete")) {
            const QString text = ev.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty())
                logText = QLatin1Char('\n') + text + QLatin1Char('\n');
        } else if (etype == QLatin1String("assistant")) {
            logText = AgentLogText::assistantContent(
                ev.value(QStringLiteral("message")).toObject());
        } else if (etype == QLatin1String("user")) {
            logText = AgentLogText::userEvent(ev);
        }
        if (!logText.isEmpty())
            m_agentStore->appendLog(*streamInfo, logText);
    }

    // A credit-exhausted transport may have events already queued when its
    // replacement is prepared. Keep the diagnostic event in storage above,
    // but do not let that old transport change the replacement's state.
    if (m_teammateBackupTransportStops.contains(sessionId))
        return;

    // A run that was called done and is still working was not done. Every path
    // that opens a turn funnels through markAgentSessionRunning(), but a CLI
    // that keeps going on its own after its `result` — a hook that answers it,
    // a provider that reopens the turn, a tool result that lands late — goes
    // through none of them, and the badge would keep saying "done" while the
    // transcript filled in underneath it. Take the claim back here, before the
    // handlers below read the status: only the next completion may finish this
    // run, and notifyAgentDone() will announce it again because
    // markAgentSessionRunning() clears the once-per-run mark. Restricted to a
    // session whose own transport is still attached, so a stored or relayed
    // event can never resurrect a run that really did end.
    if (agentEventShowsWorkInFlight(type) && !isExternalSession(sessionId) &&
        agentSessionHasLiveTransport(sessionId)) {
        if (const AgentSession *live = findAgentSession(sessionId);
            live && !live->merged && live->status == AgentStatus::Success)
            markAgentSessionRunning(sessionId);
    }
    if (agentEventShowsWorkInFlight(type) &&
        forkmesh::transcripttools::parentToolUseId(ev).isEmpty())
        m_agentResultAwaitingCliTasks.remove(sessionId);
    if (forkmesh::agentclitasks::isTaskEvent(ev) && !isExternalSession(sessionId))
        trackAgentCliTask(sessionId, ev);

    if (const forkmesh::transcripttools::TokenStep step = m_sessionTokenMeter[sessionId].add(ev);
        step.in > 0 || step.out > 0) {
        QPair<qint64, qint64> &flow = m_sessionTokenFlow[sessionId];
        flow.first += step.in;
        flow.second += step.out;
        m_debugTokensInTotal += step.in;
        m_debugTokensOutTotal += step.out;
    }
    if (ev.value(QStringLiteral("type")).toString() == QLatin1String("assistant")) {
        const QJsonObject message = ev.value(QStringLiteral("message")).toObject();
        const qint64 out = m_sessionOutputTokens[sessionId].add(message);
        if (out > 0) {
            m_sessionTokens[sessionId] += out;
            if (AgentSession *as = findAgentSession(sessionId))
                as->totalTokens = static_cast<int>(
                    qMin<qint64>(m_sessionTokens.value(sessionId), 2'000'000'000));
            updateAgentTokenCell(sessionId);
        }
        const QJsonArray content = ev.value(QStringLiteral("message")).toObject()
                                       .value(QStringLiteral("content")).toArray();
        QString assistantText;
        bool askedQuestion = false;
        bool needsPermission = false;
        for (const QJsonValue &bv : content) {
            const QJsonObject b = bv.toObject();
            const QString btype = b.value(QStringLiteral("type")).toString();
            if (btype == QLatin1String("text"))
                assistantText += b.value(QStringLiteral("text")).toString();
            if (btype != QLatin1String("tool_use"))
                continue;
            if (!isExternalSession(sessionId))
                recordAgentToolShortcutUses(sessionId, b);
            const QString name = b.value(QStringLiteral("name")).toString();
            if (name == QLatin1String("AskUserQuestion")) {
                askedQuestion = true;
                needsPermission =
                    b.value(QStringLiteral("id")).toString().contains(
                        QStringLiteral("approval"), Qt::CaseInsensitive);
            }
            if (name == QLatin1String("Edit") || name == QLatin1String("Write") ||
                name == QLatin1String("MultiEdit") ||
                name == QLatin1String("NotebookEdit") ||
                name == QLatin1String("FileChange") || name == QLatin1String("Bash"))
                ++m_agentPendingFileTools[sessionId];
            if (name == QLatin1String("Edit") || name == QLatin1String("Write")
                || name == QLatin1String("MultiEdit") || name == QLatin1String("NotebookEdit")) {
                const QString p = b.value(QStringLiteral("input")).toObject()
                                      .value(QStringLiteral("file_path")).toString();
                if (!p.isEmpty() && !m_streamFiles[sessionId].contains(p))
                    m_streamFiles[sessionId].append(p);
            } else if (name == QLatin1String("FileChange")) {
                const QJsonArray changes = b.value(QStringLiteral("input")).toObject()
                                               .value(QStringLiteral("changes")).toArray();
                for (const QJsonValue &change : changes) {
                    const QString p =
                        change.toObject().value(QStringLiteral("path")).toString();
                    if (!p.isEmpty() && !m_streamFiles[sessionId].contains(p))
                        m_streamFiles[sessionId].append(p);
                }
            }
        }
        if (!assistantText.trimmed().isEmpty()) {
            m_lastAssistantText[sessionId] = assistantText.trimmed();
            relayTeammateAssistantText(sessionId, assistantText);
        }
        applyGenieTaskTitle(sessionId, assistantText);
        if (!askedQuestion) {
            QStringList inlineOptions;
            askedQuestion = ClaudeTranscriptView::parseInlineChoices(assistantText, inlineOptions);
        }
        if (askedQuestion)
            notifyAgentWaiting(sessionId, needsPermission);
    }

    // Both CLIs report a finished tool as a user turn carrying tool_result
    // blocks; once an editing tool's result is in, its change is on disk.
    if (type == QLatin1String("user") && m_agentPendingFileTools.contains(sessionId)) {
        const QJsonArray content = ev.value(QStringLiteral("message")).toObject()
                                       .value(QStringLiteral("content")).toArray();
        int results = 0;
        for (const QJsonValue &bv : content)
            if (bv.toObject().value(QStringLiteral("type")).toString() ==
                QLatin1String("tool_result"))
                ++results;
        if (results > 0) {
            int &pending = m_agentPendingFileTools[sessionId];
            pending -= results;
            if (pending <= 0)
                m_agentPendingFileTools.remove(sessionId);
            scheduleAgentLiveDiffRefresh(sessionId);
        }
    } else if (type == QLatin1String("result")) {
        m_agentPendingFileTools.remove(sessionId);
    }

    if (type == QLatin1String("_codex_agent_complete")) {
        const QString text = ev.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) {
            m_lastAssistantText[sessionId] = text;
            relayTeammateAssistantText(sessionId, text);
            QStringList inlineOptions;
            if (ClaudeTranscriptView::parseInlineChoices(text, inlineOptions))
                notifyAgentWaiting(sessionId, false);
        }
    } else if (type == QLatin1String("_codex_usage")) {
        if (AgentSession *as = findAgentSession(sessionId)) {
            as->promptTokens = static_cast<int>(qMin<qint64>(
                static_cast<qint64>(ev.value(QStringLiteral("input_tokens")).toDouble()),
                2'000'000'000));
            as->completionTokens = static_cast<int>(qMin<qint64>(
                static_cast<qint64>(ev.value(QStringLiteral("output_tokens")).toDouble()),
                2'000'000'000));
            as->totalTokens = static_cast<int>(qMin<qint64>(
                static_cast<qint64>(ev.value(QStringLiteral("total_tokens")).toDouble()),
                2'000'000'000));
            as->contextTokens = as->totalTokens;
            const int contextWindow = static_cast<int>(qMin<qint64>(
                static_cast<qint64>(ev.value(QStringLiteral("context_window")).toDouble()),
                2'000'000'000));
            if (contextWindow > 0)
                as->contextWindow = contextWindow;
            m_sessionTokens[sessionId] = as->totalTokens;
            m_sessionTokenFlow[sessionId] = {as->promptTokens, as->completionTokens};
            if (m_agentStore && !isExternalSession(sessionId))
                scheduleAgentSessionSave(sessionId); // per-token-batch event
            updateAgentTokenCell(sessionId);
        }
    } else if (type == QLatin1String("_codex_rate_limits") ||
               type == QLatin1String("rate_limit_event")) {
        QJsonObject limits = ev.value(QStringLiteral("rateLimits")).toObject();
        if (limits.isEmpty())
            limits = ev.value(QStringLiteral("codex_rate_limits")).toObject();
        QJsonObject limitsById =
            ev.value(QStringLiteral("rateLimitsByLimitId")).toObject();
        if (limitsById.isEmpty())
            limitsById =
                ev.value(QStringLiteral("rate_limits_by_limit_id")).toObject();
        if (!limits.isEmpty() || !limitsById.isEmpty()) {
            m_pendingCodexRateLimits = limits;
            m_pendingCodexRateLimitsByLimitId = limitsById;
            if (!m_codexRateLimitsApplyScheduled) {
                m_codexRateLimitsApplyScheduled = true;
                QTimer::singleShot(2000, this, [this] {
                    m_codexRateLimitsApplyScheduled = false;
                    applyCodexRateLimits(
                        std::exchange(m_pendingCodexRateLimits, {}),
                        std::exchange(m_pendingCodexRateLimitsByLimitId, {}));
                });
            }
        }
        // Claude Code reports one window per event, under its own key. Reading
        // it here rather than in the transcript view means a run that hits the
        // limit while another session is on screen still starts the countdown.
        const QJsonObject claudeLimit =
            ev.value(QStringLiteral("rate_limit_info")).toObject();
        if (!claudeLimit.isEmpty())
            applyClaudeRateLimitEvent(claudeLimit);
    }

    // The CLI is asking to use a tool while in manual mode: the agent is blocked
    // on the user's approval — surface it (see notifyAgentWaiting). A plain
    // `result` (the turn finishing normally, e.g. the agent reporting "Done") is
    // NOT a wait: it's handled below as a successful completion. Only genuine
    // input-required states — a permission prompt here, or an AskUserQuestion
    // multiple-choice handled above — mark the session "Waiting".
    if (type == QLatin1String("control_request")) {
        const QJsonObject request = ev.value(QStringLiteral("request")).toObject();
        notifyAgentWaiting(
            sessionId,
            request.value(QStringLiteral("subtype")).toString() ==
                    QLatin1String("can_use_tool") &&
                request.value(QStringLiteral("tool_name")).toString() !=
                    QLatin1String("AskUserQuestion"));
    }

    if (type == QLatin1String("system")
        && ev.value(QStringLiteral("subtype")).toString() == QLatin1String("init"))
        markAgentSessionRunning(sessionId);

    if (type == QLatin1String("result")) {
        bool erroredResult = false;
        QString resultFailure;
        if (AgentSession *as = findAgentSession(sessionId)) {
            bool awaitSubprocesses = false;
            QString cliTasksNotice;
            const int turns = ev.value(QStringLiteral("num_turns")).toInt();
            const qint64 dur = static_cast<qint64>(
                ev.value(QStringLiteral("duration_ms")).toDouble());
            const double cost = ev.value(QStringLiteral("total_cost_usd")).toDouble();
            if (turns > 0)
                as->numTurns = turns;
            if (dur > 0)
                as->durationMs = dur;
            if (cost > 0)
                as->costUsd = cost;
            const bool userStopped = as->status == AgentStatus::Stopped;
            const bool activeRun = as->status == AgentStatus::Running ||
                                   as->status == AgentStatus::Waiting;
            if (activeRun && ClaudeTranscriptView::resultIsError(ev)) {
                erroredResult = true;
                m_agentCompletionChecks.remove(sessionId);
                m_agentResultAwaitingCliTasks.remove(sessionId);
                m_agentBackgroundWork.remove(sessionId);
                as->status = AgentStatus::Failed;
                as->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
                as->lastError = ClaudeTranscriptView::failureReason(ev);
                resultFailure = as->lastError;
            } else if (as->status == AgentStatus::Running) {
                // A clean `result` says the CLI has finished its response, but
                // not necessarily that the build/test children it launched have
                // exited. Keep this turn visibly Working until the process tree
                // drains; completeAgentSessionWhenSubprocessesExit() then makes
                // the Success transition atomically. A new turn clears this
                // pending check in markAgentSessionRunning().
                const auto tasks = m_agentCliTasks.constFind(sessionId);
                if (tasks != m_agentCliTasks.constEnd() && tasks->openBackground() > 0) {
                    m_agentResultAwaitingCliTasks.insert(sessionId);
                    cliTasksNotice = forkmesh::agentclitasks::waitingNotice(*tasks);
                } else {
                    m_agentCompletionChecks.insert(sessionId);
                    awaitSubprocesses = true;
                }
            } else if (userStopped) {
                m_agentResultAwaitingCliTasks.remove(sessionId);
                m_agentCompletionChecks.remove(sessionId);
                m_agentBackgroundWork.remove(sessionId);
            }
            applyAgentBackgroundWork();
            if (m_agentStore && !isExternalSession(sessionId))
                m_agentStore->saveSession(*as);
            updateAgentCostCell(sessionId);
            updateAgentRunSummaryCells(sessionId); // fill the Turns/Time columns
            if (awaitSubprocesses)
                completeAgentSessionWhenSubprocessesExit(sessionId);
            else
                updateAgentStatusCell(sessionId);
            if (!cliTasksNotice.isEmpty())
                noteAgentSessionNotice(sessionId, cliTasksNotice);
        }
        if (erroredResult) {
            QString fallbackFailure = resultFailure;
            if (m_agentStore)
                if (const AgentSession *session = findAgentSession(sessionId))
                    fallbackFailure += QLatin1Char('\n') +
                                       m_agentStore->readLog(*session).right(16 * 1024);
            fallbackFailure += QLatin1Char('\n') +
                               m_streamRaw.value(sessionId).right(16 * 1024);
            if (queueTeammateBackupModel(sessionId, fallbackFailure)) {
                // stopErroredStreamTransport detaches the process from its
                // transport, so it has no finished() callback to pump the
                // queued replacement. Reserve the slot until the stop grace
                // has elapsed, then continue the same session on its backup.
                m_agentQueue.removeAll(sessionId);
                m_teammateBackupTransportStops.insert(sessionId);
                stopErroredStreamTransport(sessionId);
                QTimer::singleShot(2000, this, [this, sessionId] {
                    if (!m_teammateBackupTransportStops.contains(sessionId))
                        return;
                    m_teammateBackupTransportStops.remove(sessionId);
                    if (AgentSession *queued = findAgentSession(sessionId);
                        queued && queued->status == AgentStatus::Queued &&
                        !m_agentQueue.contains(sessionId)) {
                        m_agentQueue.append(sessionId);
                    }
                    reloadAgents();
                    if (sessionId == m_selectedAgentSessionId)
                        showAgentSession(sessionId);
                    processAgentQueue();
                    maybeStartQueuedRebuild();
                });
                reloadAgents();
                if (sessionId == m_selectedAgentSessionId)
                    showAgentSession(sessionId);
            } else {
                stopErroredStreamTransport(sessionId);
                holdAgentForUsageLimit(sessionId);
            }
        }
        if (m_codexStreams.contains(sessionId) &&
            !ev.value(QStringLiteral("is_error")).toBool()) {
            if (const AgentSession *cs = findAgentSession(sessionId);
                cs && (cs->yolo || cs->vibe))
                maybeCreatePullForStreamSession(sessionId);
            looperOnSessionFinished(sessionId);
        }
    }

    if (sessionId == m_selectedAgentSessionId && m_agentTranscript) {
        if (m_agentTranscriptRenderSession == sessionId)
            return;
        const int rendered = m_renderedTranscriptSession == sessionId
                                 ? m_renderedTranscriptCount
                                 : -1;
        const QList<QJsonObject> events = m_streamEvents.value(sessionId);
        const int have = events.size();
        constexpr int kLiveCatchUpEvents = 32;
        if (QApplication::activeModalWidget() || rendered == have) {
        } else if (rendered >= 0 && rendered < have &&
                   have - rendered <= kLiveCatchUpEvents) {
            bool streamingOnly = true;
            for (int i = rendered; i < have; ++i) {
                const QJsonObject &pending = events.at(i);
                const QString pendingType =
                    pending.value(QStringLiteral("type")).toString();
                if (pendingType == QLatin1String("_local_user"))
                    m_agentTranscript->addUserTurn(
                        pending.value(QStringLiteral("text")).toString(),
                        ClaudeTranscriptView::eventTime(pending));
                else
                    m_agentTranscript->handleEvent(pending);
                streamingOnly = streamingOnly &&
                    (pendingType == QLatin1String("stream_event") ||
                     pendingType == QLatin1String("_codex_agent_delta") ||
                     pendingType == QLatin1String("_codex_tool_delta") ||
                     pendingType == QLatin1String("_codex_usage") ||
                     pendingType == QLatin1String("_codex_rate_limits"));
            }
            if (!streamingOnly)
                refreshAgentFilesPanel(sessionId);
            m_renderedTranscriptCount = have;
        } else {
            if (!renderTranscriptTail(sessionId))
                renderTranscriptForSession(sessionId);
            refreshAgentFilesPanel(sessionId);
        }
    }
}

QString MainWindow::lastClaudeSessionId(int sessionId) const
{
    return forkmesh::agents::claudeResumeSessionId(
        m_streamEvents.value(sessionId),
        activeAgentAccount(QStringLiteral("claude-code")).id);
}

QString MainWindow::lastCodexThreadId(int sessionId) const
{
    return forkmesh::agents::codexResumeThreadId(
        m_streamEvents.value(sessionId),
        activeAgentAccount(kCodexProvider).id);
}

QString MainWindow::agentCliConversationId(int sessionId)
{
    if (isExternalSession(sessionId))
        return m_externalSurfaced.value(sessionId).uuid;
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return QString();
    if (agentIsCodexProvider(session->provider))
        return lastCodexThreadId(sessionId);
    if (session->provider == QLatin1String("claude-code"))
        return lastClaudeSessionId(sessionId);
    return QString();
}

MainWindow::AgentTerminalHandoff MainWindow::agentTerminalHandoff(int sessionId)
{
    AgentTerminalHandoff plan;
    const AgentSession *live = findAgentSession(sessionId);
    if (!live) {
        plan.blocker = QStringLiteral("Select a session first.");
        plan.code = QStringLiteral("unknown_session");
        return plan;
    }
    const AgentSession session = *live;
    if (session.cloud) {
        plan.blocker = QStringLiteral(
            "This agent runs on %1, so its CLI conversation is not on this "
            "machine to open in a terminal.")
                           .arg(session.cloudNode.isEmpty()
                                    ? QStringLiteral("a cloud node")
                                    : session.cloudNode);
        plan.code = QStringLiteral("cloud_session");
        return plan;
    }
    const bool external = isExternalSession(sessionId);
    const QString provider =
        external ? QStringLiteral("claude-code") : session.provider;
    const bool codex = agentIsCodexProvider(provider);
    if (!codex && provider != QLatin1String("claude-code")) {
        plan.blocker = QStringLiteral("%1 sessions run against the API — there is "
                                      "no CLI conversation to continue in a "
                                      "terminal.")
                           .arg(agentProviderName(provider));
        plan.code = QStringLiteral("unsupported_provider");
        return plan;
    }
    const int repoIdx =
        external ? -1 : repoIndexFor(session.owner, session.name);
    bool guardrailsActive = false;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.owner == session.owner && repo.name == session.name &&
            repo.agentGuardrailsEnabled) {
            guardrailsActive = true;
            break;
        }
    }
    if (guardrailsActive) {
        plan.blocker = QStringLiteral(
            "This repository's agent guardrails are active, so the session "
            "cannot be handed to an unguarded system terminal. Continue it "
            "in ForkMesh or turn guardrails off in Repository Settings.");
        plan.code = QStringLiteral("guardrails_on");
        return plan;
    }
    plan.program = forkmesh::agentcli::executable(
        codex ? QStringLiteral("codex") : QStringLiteral("claude"));
    if (plan.program.isEmpty()) {
        plan.blocker = QStringLiteral("%1 is not installed on this device.")
                           .arg(codex ? QStringLiteral("Codex")
                                      : QStringLiteral("Claude Code"));
        plan.code = QStringLiteral("cli_missing");
        return plan;
    }
    if (external) {
        plan.cwd = m_externalSurfaced.value(sessionId).cwd;
    } else {
        const QString repoLocal =
            repoIdx >= 0 ? m_repositories.at(repoIdx).localPath : QString();
        if (!repoLocal.isEmpty() && !session.branchName.isEmpty())
            plan.cwd =
                cachedSessionWorktree(sessionId, repoLocal, session.branchName);
        if (plan.cwd.isEmpty())
            plan.cwd = repoLocal;
    }
    if (plan.cwd.isEmpty() || !QDir(plan.cwd).exists()) {
        plan.blocker = QStringLiteral("This session has no working directory left "
                                      "on disk to continue in.");
        plan.code = QStringLiteral("no_worktree");
        return plan;
    }
    plan.conversationId = agentCliConversationId(sessionId);
    if (!plan.conversationId.isEmpty()) {
        plan.args = codex ? QStringList{QStringLiteral("resume"),
                                        plan.conversationId}
                          : QStringList{QStringLiteral("--resume"),
                                        plan.conversationId};
    }
    const AgentAccountProfile account = activeAgentAccount(provider);
    if (!account.builtIn)
        plan.env.insert(codex ? QStringLiteral("CODEX_HOME")
                              : QStringLiteral("CLAUDE_CONFIG_DIR"),
                        account.configDir);
    plan.ok = true;
    return plan;
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testAgentTerminalHandoffBlocker(int sessionId)
{
    return agentTerminalHandoff(sessionId).blocker;
}
#endif

void MainWindow::popOutAgentSessionToTerminal(int sessionId)
{
    const AgentTerminalHandoff plan = agentTerminalHandoff(sessionId);
    if (m_agentMetaPopup)
        m_agentMetaPopup->hide(); // the click is leaving this pane either way
    if (!plan.ok) {
        flashMessage(plan.blocker, true, QString(),
                     forkmesh::kLocalOperationNoticeKind);
        return;
    }
    bool stopped = false;
    if (isExternalSession(sessionId)) {
        if (m_externalSurfaced.contains(sessionId) &&
            externalIsLive(m_externalSurfaced.value(sessionId).uuid)) {
            stopExternalSession(sessionId);
            stopped = true;
        }
    } else {
        stopped = stopAgentSessionById(sessionId);
    }
    if (!launchSystemTerminal(plan.program, plan.args, plan.cwd, plan.env))
        return;
    // plan.program is the resolved absolute path the emulator needs; the
    // notice names the CLI.
    const QString cli = QFileInfo(plan.program).fileName();
    const QString what =
        plan.conversationId.isEmpty()
            ? QStringLiteral("%1 opened in %2").arg(cli, plan.cwd)
            : QStringLiteral("%1 resumed in %2").arg(cli, plan.cwd);
    flashMessage(stopped ? QStringLiteral("Session stopped here — %1.").arg(what)
                         : QStringLiteral("%1.").arg(what));
}

// ---- "Move to cloud" --------------------------------------------------------
// A session runs on the desk it was started at. Moving it into the cloud hands
// its continuation to the relay, which queues it for a mirror node this account
// owns that serves the repository (or, with no such mirror, for the account's
// other desktops); that node claims the job and carries the work on.
//
// "Everything needed to continue" splits in two, deliberately. The work itself
// travels in Git -- the session's tree is committed and its branch pushed into
// the served mirror, so the receiving node fetches real history rather than a
// patch. The instruction travels in the job: the branch, the base it was cut
// from, the runtime, and the task. The transcript travels in neither; it is
// already owner-sealed on the agent snapshot transport (pushAgentSessionsSnapshot),
// which the account's other devices open with their own key, and copying it into
// a relay-readable job row would hand the relay exactly the plaintext that
// transport exists to withhold.
MainWindow::AgentCloudHandoff MainWindow::agentCloudHandoff(int sessionId,
                                                            bool resolveWorktree)
{
    AgentCloudHandoff plan;
    // Cloud rows live inside the external band, so they are named first.
    if (isCloudSession(sessionId)) {
        plan.blocker = QStringLiteral(
            "This session is already running in the cloud.");
        return plan;
    }
    if (isExternalSession(sessionId)) {
        plan.blocker = QStringLiteral(
            "This session was surfaced from another tool running on this "
            "machine, so ForkMesh has no branch of its own to publish.");
        return plan;
    }
    const AgentSession *live = findAgentSession(sessionId);
    if (!live) {
        plan.blocker = QStringLiteral("Select a session first.");
        return plan;
    }
    // Everything below can shell git, which services UI events, so take a copy
    // rather than holding the pointer across it (git-pump UAF family).
    const AgentSession session = *live;
    if (session.associationOnly) {
        plan.blocker = QStringLiteral(
            "This entry only records a link to other work — there is no agent "
            "run behind it to move.");
        return plan;
    }
    const QString provider = session.provider.trimmed().toLower();
    const bool codex = agentIsCodexProvider(provider);
    if (!codex && provider != QLatin1String("claude-code")) {
        plan.blocker =
            QStringLiteral("%1 sessions run against an API key held on this "
                           "device, so a cloud node could not continue them.")
                .arg(agentProviderName(session.provider));
        return plan;
    }
    plan.provider = codex ? QStringLiteral("codex")
                          : QStringLiteral("claude-code");
    plan.model = session.model.trimmed();
    plan.branch = session.branchName.trimmed();
    if (plan.branch.isEmpty()) {
        plan.blocker = QStringLiteral(
            "This session has no branch of its own, so there is nothing to "
            "publish for another node to continue.");
        return plan;
    }
    plan.prompt = session.prompt.trimmed();
    if (plan.prompt.isEmpty())
        plan.prompt = session.issueTitle.trimmed();
    if (plan.prompt.isEmpty()) {
        plan.blocker = QStringLiteral(
            "This session records no task, so a cloud agent would have nothing "
            "to carry on with.");
        return plan;
    }
    plan.repoIndex = repoIndexFor(session.owner, session.name);
    if (plan.repoIndex < 0) {
        plan.blocker =
            QStringLiteral("%1/%2 is not on this node any more.")
                .arg(session.owner, session.name);
        return plan;
    }
    const RepositoryRecord repo = m_repositories.at(plan.repoIndex);
    if (repo.previewOnly || !repo.publishToNetwork) {
        plan.blocker =
            QStringLiteral("%1/%2 is not published to the network, so no cloud "
                           "node can reach this session's branch. Publish it "
                           "first.")
                .arg(repo.owner, repo.name);
        return plan;
    }
    plan.mirrorPath = writableRecordFor(repo).mirrorPath.trimmed();
    if (plan.mirrorPath.isEmpty()) {
        plan.blocker =
            QStringLiteral("%1/%2 has no served mirror on this node, so the "
                           "branch has no way out to the cloud.")
                .arg(repo.owner, repo.name);
        return plan;
    }
    if (!hasOwnerSigningCapability(accountOwner())) {
        plan.blocker =
            QStringLiteral("This device cannot sign for %1, so it cannot hand "
                           "the session to its cloud nodes. Sign in to "
                           "ForkMesh on this desktop again.")
                .arg(accountOwner().isEmpty() ? QStringLiteral("your account")
                                              : accountOwner());
        return plan;
    }
    plan.owner = repo.owner;
    plan.name = repo.name;
    plan.baseBranch = session.baseBranch.trimmed();
    plan.baseRef = session.baseRef.trimmed();
    plan.issueNumber = session.issueNumber;
    plan.createPr = session.createPr;
    plan.title = session.issueTitle.trimmed();
    if (plan.title.isEmpty())
        plan.title = plan.prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
    // Resolving the worktree shells git; a caller that only wants to know
    // whether to offer the move (the list's right-click menu) stops here and
    // leaves that last gate to the click.
    if (!resolveWorktree) {
        plan.ok = true;
        return plan;
    }
    plan.worktree =
        cachedSessionWorktree(sessionId, repo.localPath, plan.branch);
    if (plan.worktree.isEmpty() || !QDir(plan.worktree).exists()) {
        plan.blocker = QStringLiteral(
            "This session's worktree is gone, so there is nothing left here to "
            "commit and publish.");
        return plan;
    }
    plan.ok = true;
    return plan;
}

// Commit whatever the session left uncommitted, then push its branch into the
// served mirror. Both halves are required before the relay is told anything:
// a job naming a branch the cloud cannot fetch is worse than no job at all.
bool MainWindow::publishAgentHandoffBranch(const AgentCloudHandoff &plan,
                                           int sessionId, QString *error)
{
    const auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    QByteArray status;
    if (!runGitCapture(plan.worktree,
                       {QStringLiteral("status"), QStringLiteral("--porcelain")},
                       &status, nullptr))
        return fail(QStringLiteral("this session's worktree could not be read"));
    if (!status.trimmed().isEmpty()) {
        QString commitError;
        if (!runGitCapture(plan.worktree,
                           {QStringLiteral("add"), QStringLiteral("-A")},
                           nullptr, &commitError))
            return fail(QStringLiteral("its uncommitted changes could not be "
                                       "staged: %1")
                            .arg(commitError.trimmed().right(160)));
        const QString message =
            QStringLiteral("Agent #%1: work in progress, moved to the cloud")
                .arg(sessionId);
        if (!runGitCapture(plan.worktree,
                           {QStringLiteral("commit"), QStringLiteral("-m"),
                            message},
                           nullptr, &commitError))
            return fail(QStringLiteral("its uncommitted changes could not be "
                                       "committed: %1")
                            .arg(commitError.trimmed().right(160)));
        if (AgentSession *live = findAgentSession(sessionId); live && m_agentStore)
            m_agentStore->appendLog(
                *live,
                QStringLiteral("==> Committed the worktree's pending changes "
                               "before the cloud handoff.\n"));
    }
    const QString ref = QStringLiteral("refs/heads/") + plan.branch;
    QString pushError;
    if (!runGitCapture(plan.worktree,
                       {QStringLiteral("push"), plan.mirrorPath,
                        ref + QLatin1Char(':') + ref},
                       nullptr, &pushError)) {
        const QString detail = pushError.trimmed();
        return fail(QStringLiteral("its branch could not be published to the "
                                   "served mirror: %1")
                        .arg(detail.isEmpty()
                                 ? QStringLiteral("the mirror is unavailable")
                                 : detail.right(160)));
    }
    return true;
}

void MainWindow::moveAgentSessionToCloud(int sessionId, const QString &toKey,
                                         const QString &toLabel)
{
    const bool toDesktop = !toKey.isEmpty();
    const QString destination =
        toDesktop ? toLabel : QStringLiteral("the cloud");
    if (m_agentCloudHandoffsInFlight.contains(sessionId)) {
        flashMessage(QStringLiteral("This session is already on its way to "
                                    "another device."));
        return;
    }
    const AgentCloudHandoff plan = agentCloudHandoff(sessionId);
    if (!plan.ok) {
        flashMessage(plan.blocker, true);
        return;
    }
    if (!m_networkAccess) {
        flashMessage(QStringLiteral("ForkMesh is offline, so this session "
                                    "cannot be handed to %1 yet.")
                         .arg(destination),
                     true);
        return;
    }
    QString publishError;
    if (!publishAgentHandoffBranch(plan, sessionId, &publishError)) {
        flashMessage(QStringLiteral("This session stays here — %1.")
                         .arg(publishError),
                     true);
        return;
    }
    // Carry the branch on to the mirror fleet now rather than on the next sync
    // pass: the mirror that claims the job continues from that branch.
    if (const int repoIndex = repoIndexFor(plan.owner, plan.name); repoIndex >= 0)
        pushToSshMirrorRemotes(repoIndex);
    // publishAgentHandoffBranch shells git, which pumps the event loop, so the
    // session list may have been rebuilt underneath us.
    if (!findAgentSession(sessionId)) {
        flashMessage(QStringLiteral("This session went away while its branch "
                                    "was being published."),
                     true);
        return;
    }

    // Addressed by name, not by the index the plan was built from: publishing
    // the branch pumped the event loop and m_repositories may have been
    // rebuilt under us since (git-pump UAF family).
    RepositoryRecord addressed;
    addressed.owner = plan.owner;
    addressed.name = plan.name;
    QUrl url = agentsApiUrl(addressed);
    url.setPath(url.path() + QStringLiteral("/%1/handoff").arg(sessionId));
    const QUrlQuery query = signedInboxQuery(
        repoSegment(accountOwner().trimmed().toLower(), QStringLiteral("owner")));
    if (query.isEmpty()) {
        flashMessage(QStringLiteral("This device could not sign the handoff, "
                                    "so the session stays here."),
                     true);
        return;
    }
    url.setQuery(query);

    QJsonObject body{
        {QStringLiteral("provider"), plan.provider},
        {QStringLiteral("model"), plan.model},
        {QStringLiteral("prompt"), plan.prompt},
        {QStringLiteral("branch"), plan.branch},
        {QStringLiteral("baseBranch"), plan.baseBranch},
        {QStringLiteral("baseRef"), plan.baseRef},
        {QStringLiteral("title"), plan.title},
        {QStringLiteral("issueNumber"), plan.issueNumber},
        {QStringLiteral("createPr"), plan.createPr},
        // Every node this account runs drains the same queue, including this
        // one: name the device giving the session up so it skips its own
        // handoff instead of claiming the work straight back.
        {QStringLiteral("fromDevice"), machineNodeName()},
    };
    if (toDesktop)
        body.insert(QStringLiteral("toKey"), toKey);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    m_agentCloudHandoffsInFlight.insert(sessionId);
    updateAgentActionState();
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, branch = plan.branch, toDesktop,
             destination] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_agentCloudHandoffsInFlight.remove(sessionId);
        if (!networkOk || payload.value(QStringLiteral("ok")).toBool() != true) {
            const QString message =
                payload.value(QStringLiteral("message")).toString().trimmed();
            const QString why =
                message.isEmpty()
                    ? QStringLiteral("the relay rejected the handoff")
                    : message;
            flashMessage(
                toDesktop
                    ? QStringLiteral("This session was not sent to %1: %2")
                          .arg(destination, why)
                    : QStringLiteral("The cloud did not take this session: %1")
                          .arg(why),
                true);
            updateAgentActionState();
            return;
        }
        finishAgentCloudHandoff(sessionId, branch, payload);
    });
}

// The relay has the continuation. Stop the local run and record where the work
// went, so the row does not look like an agent that simply stopped.
void MainWindow::finishAgentCloudHandoff(int sessionId, const QString &branch,
                                         const QJsonObject &payload)
{
    const bool stopped = stopAgentSessionById(sessionId);
    const QString targetNode =
        payload.value(QStringLiteral("targetNode")).toString().trimmed();
    const QString targetDevice =
        payload.value(QStringLiteral("targetDevice")).toString().trimmed();
    if (AgentSession *session = findAgentSession(sessionId)) {
        session->status = AgentStatus::Stopped;
        session->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        session->lastError.clear();
        if (m_agentStore) {
            m_agentStore->saveSession(*session);
            m_agentStore->appendLog(
                *session,
                !targetDevice.isEmpty()
                    ? QStringLiteral("==> Sent to the desktop %1 on %2. It "
                                     "continues the work from there; this "
                                     "device is no longer running it.\n")
                          .arg(targetDevice, branch)
                : targetNode.isEmpty()
                    ? QStringLiteral("==> Moved to the cloud on %1. A node "
                                     "serving this repository picks the work "
                                     "up from there; this device is no longer "
                                     "running it.\n")
                          .arg(branch)
                    : QStringLiteral("==> Moved to the cloud on %1. The mirror "
                                     "node %2 continues the work from there; "
                                     "this device is no longer running it.\n")
                          .arg(branch, targetNode));
        }
    }
    const QString moved =
        targetDevice.isEmpty()
            ? QStringLiteral("Moved to the cloud on %1.").arg(branch)
            : QStringLiteral("Sent to %1 on %2.").arg(targetDevice, branch);
    noteAgentSessionNotice(sessionId, moved);
    scheduleAgentSessionsPush();
    reloadAgents();
    // The continuation is a cloud row now; show it without waiting for the
    // list's next poll.
    refreshCloudAgentSessions(true);
    if (sessionId == m_selectedAgentSessionId)
        showAgentSession(sessionId);
    const QString message =
        payload.value(QStringLiteral("message")).toString().trimmed();
    flashMessage(
        stopped
            ? QStringLiteral("Stopped here — %1")
                  .arg(message.isEmpty() ? moved : message)
            : (message.isEmpty() ? moved : message));
}

void MainWindow::loadAgentHandoffDesktops(int sessionId)
{
    if (m_agentHandoffDesktopsLoading)
        return;
    const AgentCloudHandoff plan = agentCloudHandoff(sessionId, false);
    if (!plan.ok) {
        m_agentHandoffDesktopsError = plan.blocker;
        return;
    }
    if (!m_networkAccess) {
        m_agentHandoffDesktopsError = QStringLiteral(
            "ForkMesh is offline, so it cannot list your other desktops.");
        return;
    }
    RepositoryRecord addressed;
    addressed.owner = plan.owner;
    addressed.name = plan.name;
    QUrl url = agentsApiUrl(addressed);
    url.setPath(url.path() + QStringLiteral("/%1/handoff").arg(sessionId));
    const QUrlQuery query = signedInboxQuery(
        repoSegment(accountOwner().trimmed().toLower(), QStringLiteral("owner")));
    if (query.isEmpty()) {
        m_agentHandoffDesktopsError = QStringLiteral(
            "This device could not sign the request for your desktops.");
        return;
    }
    url.setQuery(query);
    m_agentHandoffDesktopsLoading = true;
    m_agentHandoffDesktopsTriedMs = QDateTime::currentMSecsSinceEpoch();
    m_agentHandoffDesktopsError.clear();
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_agentHandoffDesktopsLoading = false;
        if (networkOk && payload.value(QStringLiteral("ok")).toBool()) {
            m_agentHandoffDesktops = forkmesh::agents::handoffDesktops(payload);
            m_agentHandoffDesktopsLoadedMs = QDateTime::currentMSecsSinceEpoch();
        } else {
            const QString message =
                payload.value(QStringLiteral("message")).toString().trimmed();
            m_agentHandoffDesktopsError =
                message.isEmpty()
                    ? QStringLiteral("Your desktops could not be listed.")
                    : message;
        }
        const QList<QPointer<QMenu>> menus = m_agentHandoffDesktopMenus;
        m_agentHandoffDesktopMenus.clear();
        for (const QPointer<QMenu> &menu : menus)
            if (menu)
                fillAgentHandoffDesktopMenu(
                    menu, menu->property("agentSessionId").toInt());
    });
}

void MainWindow::fillAgentHandoffDesktopMenu(QMenu *menu, int sessionId)
{
    if (!menu)
        return;
    menu->clear();
    menu->setProperty("agentSessionId", sessionId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool fresh = m_agentHandoffDesktopsLoadedMs > 0 &&
                       now - m_agentHandoffDesktopsLoadedMs < 5 * 60 * 1000;
    if (!fresh && now - m_agentHandoffDesktopsTriedMs > 10 * 1000)
        loadAgentHandoffDesktops(sessionId);
    if (m_agentHandoffDesktopsLoading &&
        !m_agentHandoffDesktopMenus.contains(menu))
        m_agentHandoffDesktopMenus.append(menu);
    const auto note = [menu](const QString &text) {
        QAction *action = menu->addAction(text);
        action->setObjectName(QStringLiteral("agentListSendToDesktopNote"));
        action->setEnabled(false);
    };
    if (m_agentHandoffDesktopsLoadedMs <= 0) {
        note(m_agentHandoffDesktopsLoading ||
                     m_agentHandoffDesktopsError.isEmpty()
                 ? QStringLiteral("Looking for your other desktops\u2026")
                 : m_agentHandoffDesktopsError);
        return;
    }
    if (m_agentHandoffDesktops.isEmpty()) {
        note(QStringLiteral("No other desktop is signed in to this account"));
        return;
    }
    const bool inFlight = m_agentCloudHandoffsInFlight.contains(sessionId);
    for (const forkmesh::agents::HandoffDesktop &desktop :
         std::as_const(m_agentHandoffDesktops)) {
        QAction *action = menu->addAction(desktop.label);
        action->setObjectName(QStringLiteral("agentListSendToDesktopTarget"));
        action->setData(desktop.key);
        action->setEnabled(!inFlight);
        action->setToolTip(
            inFlight ? QStringLiteral("This session is already on its way to "
                                      "another device.")
                     : QStringLiteral("%1. Continue this session on %2.")
                           .arg(forkmesh::agents::handoffDesktopSeenText(
                                    desktop.lastSeenMs, now),
                                desktop.label));
        connect(action, &QAction::triggered, this,
                [this, sessionId, key = desktop.key, label = desktop.label] {
            moveAgentSessionToCloud(sessionId, key, label);
        });
    }
}

// Right-click on a row of the Agents list. The move is offered on every row so
// the menu also answers "why can't this one go": a session that cannot be
// handed over shows the entry disabled, with the refusal as its tooltip.
void MainWindow::showAgentListContextMenu(const QPoint &viewportPos)
{
    if (!m_agentTable)
        return;
    const QModelIndex index = m_agentTable->indexAt(viewportPos);
    const QTableWidgetItem *idItem =
        index.isValid() ? m_agentTable->item(index.row(), 0) : nullptr;
    QMenu *folderMenu = nullptr;
    if (!idItem)
        folderMenu = buildAgentListBlankContextMenu(m_agentTable);
    else if (idItem->data(kAgentFolderRole).isValid())
        folderMenu = buildAgentFolderContextMenu(
            idItem->data(kAgentFolderRole).toString(), m_agentTable);
    if (folderMenu) {
        folderMenu->setAttribute(Qt::WA_DeleteOnClose);
        folderMenu->popup(m_agentTable->viewport()->mapToGlobal(viewportPos));
        return;
    }
    if (!idItem || idItem->data(kAgentFolderRole).isValid())
        return;
    const int sessionId = idItem->data(Qt::UserRole).toInt();
    // A right-click inside a multi-selection keeps it; anywhere else it moves
    // the selection to the row the menu is about.
    if (!m_agentTable->selectionModel()->isRowSelected(index.row(),
                                                       QModelIndex()))
        m_agentTable->selectRow(index.row());
    QMenu *menu = buildAgentListContextMenu(sessionId, m_agentTable);
    if (!menu)
        return;
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_agentTable->viewport()->mapToGlobal(viewportPos));
}

QMenu *MainWindow::buildAgentListContextMenu(int sessionId, QWidget *parent)
{
    if (!isCloudSession(sessionId) && !isExternalSession(sessionId) &&
        !findAgentSession(sessionId))
        return nullptr;
    auto *menu = new QMenu(parent);
    menu->setToolTipsVisible(true);
    // A cloud row's move is the other way round: it is already in the cloud,
    // and what is on offer is running it here instead.
    if (isCloudSession(sessionId)) {
        QAction *here = menu->addAction(
            forkmesh::agents::cloudAgentStoppable(
                m_cloudAgentSessions.value(sessionId))
                ? QStringLiteral("Stop the cloud run and continue here")
                : QStringLiteral("Continue on this desktop"));
        here->setObjectName(QStringLiteral("agentListContinueHereAction"));
        const QString blocker = cloudAgentContinueHereBlocker(sessionId);
        here->setEnabled(blocker.isEmpty());
        here->setToolTip(cloudAgentContinueHereTip(sessionId));
        connect(here, &QAction::triggered, this,
                [this, sessionId] { continueCloudAgentHere(sessionId); });
        const QList<forkmesh::agents::CloudAgentAction> cloudActions =
            forkmesh::agents::cloudAgentActions(
                m_cloudAgentSessions.value(sessionId));
        if (cloudActions.contains(
                forkmesh::agents::CloudAgentAction::SignInRunner)) {
            QAction *signIn =
                menu->addAction(QStringLiteral("Sign in runner"));
            signIn->setObjectName(
                QStringLiteral("agentListSignInRunnerAction"));
            signIn->setToolTip(QStringLiteral(
                "Sign the runner's agent CLI in from this desktop"));
            connect(signIn, &QAction::triggered, this, [this, sessionId] {
                signInRunnerForCloudAgent(sessionId);
            });
        }
        if (cloudActions.contains(
                forkmesh::agents::CloudAgentAction::RetryInCloud)) {
            QAction *retry =
                menu->addAction(QStringLiteral("Retry in cloud"));
            retry->setObjectName(QStringLiteral("agentListRetryInCloudAction"));
            const QString retryBlocker = cloudAgentRetryBlocker(sessionId);
            retry->setEnabled(retryBlocker.isEmpty());
            retry->setToolTip(retryBlocker.isEmpty()
                                  ? QStringLiteral("Queue this run's original "
                                                   "prompt again in the cloud")
                                  : retryBlocker);
            connect(retry, &QAction::triggered, this,
                    [this, sessionId] { retryCloudAgentInCloud(sessionId); });
        }
    }
    if (const AgentSession *held = findAgentSession(sessionId);
        held && held->awaitingStart() && !isCloudSession(sessionId) &&
        !isExternalSession(sessionId)) {
        QAction *start = menu->addAction(QStringLiteral("Start now"));
        start->setObjectName(QStringLiteral("agentListStartHeldAction"));
        start->setToolTip(QStringLiteral(
            "Run this paused agent now, or queue it if the concurrent-agent "
            "limit is reached"));
        connect(start, &QAction::triggered, this,
                [this, sessionId] { startHeldAgentSession(sessionId); });
        menu->addSeparator();
    }
    QAction *toCloud =
        menu->addAction(QStringLiteral("Continue in the cloud"));
    toCloud->setObjectName(QStringLiteral("agentListMoveToCloudAction"));
    const bool inFlight = m_agentCloudHandoffsInFlight.contains(sessionId);
    const QString blocker =
        inFlight ? QStringLiteral("This session is already on its way to the "
                                  "cloud.")
                 : agentCloudHandoff(sessionId, false).blocker;
    toCloud->setEnabled(blocker.isEmpty());
    toCloud->setToolTip(
        blocker.isEmpty()
            ? QStringLiteral("Commit and publish this session's branch, queue "
                             "the continuation for one of this account's "
                             "mirror nodes, and stop running it here")
            : blocker);
    connect(toCloud, &QAction::triggered, this,
            [this, sessionId] { moveAgentSessionToCloud(sessionId); });
    QMenu *toDesktop =
        menu->addMenu(QStringLiteral("Send to another desktop"));
    toDesktop->setObjectName(QStringLiteral("agentListSendToDesktopMenu"));
    toDesktop->setToolTipsVisible(true);
    toDesktop->menuAction()->setObjectName(
        QStringLiteral("agentListSendToDesktopAction"));
    toDesktop->menuAction()->setEnabled(blocker.isEmpty());
    toDesktop->menuAction()->setToolTip(
        blocker.isEmpty()
            ? QStringLiteral("Commit and publish this session's branch, have "
                             "one of this account's other desktops continue "
                             "it, and stop running it here")
            : blocker);
    connect(toDesktop, &QMenu::aboutToShow, this, [this, toDesktop, sessionId] {
        fillAgentHandoffDesktopMenu(toDesktop, sessionId);
    });
    QAction *toClaude = menu->addAction(QStringLiteral("Open in Claude app"));
    toClaude->setObjectName(QStringLiteral("agentListOpenInClaudeAction"));
    const QString claudeBlocker = claudeDesktopBlocker(sessionId);
    toClaude->setEnabled(claudeBlocker.isEmpty());
    toClaude->setToolTip(claudeBlocker.isEmpty()
                             ? QStringLiteral("Show this agent's Claude Code session in "
                                              "the Claude desktop app's Code tab")
                             : claudeBlocker);
    connect(toClaude, &QAction::triggered, this,
            [this, sessionId] { addAgentToClaudeDesktop(sessionId, true); });
    if (!isCloudSession(sessionId) && !isExternalSession(sessionId)) {
        QAction *rename = menu->addAction(QStringLiteral("Rename\u2026"));
        rename->setObjectName(QStringLiteral("agentListRenameAction"));
        rename->setToolTip(QStringLiteral(
            "Rename this session; a running Claude Code session takes the new "
            "name too"));
        connect(rename, &QAction::triggered, this,
                [this, sessionId] { renameAgentSessionInteractive(sessionId); });
    }
    addAgentTaskLinkActions(menu, sessionId);
    menu->addSeparator();
    addAgentFolderMenu(menu, sessionId);
    // Acts on the whole selection when the clicked row is part of it (the
    // right-click kept it), so the label says how many go.
    const QList<int> selectedIds = selectedAgentSessionIds();
    const bool bulk =
        selectedIds.size() > 1 && selectedIds.contains(sessionId);
    QAction *remove = menu->addAction(
        bulk ? QStringLiteral("Delete %1 agents").arg(selectedIds.size())
             : QStringLiteral("Delete agent"));
    remove->setObjectName(QStringLiteral("agentListDeleteAction"));
    connect(remove, &QAction::triggered, this, [this, sessionId, bulk] {
        if (bulk)
            deleteSelectedAgentSessions();
        else
            deleteAgentSessionFromRow(sessionId);
    });
    return menu;
}

bool MainWindow::applyCliExitWithoutResult(int sessionId, bool codex, int exitCode)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return false;
    const bool neverLaunched = codex ? lastCodexThreadId(sessionId).isEmpty()
                                     : lastClaudeSessionId(sessionId).isEmpty();
    const int attempts = m_agentRelaunchAttempts.value(sessionId) + 1;
    m_agentRelaunchAttempts.insert(sessionId, attempts);
    const bool giveUp = neverLaunched || attempts > kMaxAgentRelaunchAttempts;
    if (giveUp) {
        m_agentRelaunchAttempts.remove(sessionId);
        session->status = AgentStatus::Failed;
        session->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        const QString cli = codex ? QStringLiteral("Codex app-server")
                                  : QStringLiteral("Claude Code");
        session->lastError =
            neverLaunched
                ? QStringLiteral("%1 exited before starting a %2 (exit %3).")
                      .arg(cli,
                           codex ? QStringLiteral("thread")
                                 : QStringLiteral("session"))
                      .arg(exitCode)
                : QString::fromUtf8("%1 exited without starting a turn on %2 "
                                 "attempts (exit %3). The branch still holds the "
                                 "work \xE2\x80\x94 check the CLI is installed and "
                                 "logged in, then continue this session again.")
                      .arg(cli)
                      .arg(attempts)
                      .arg(exitCode);
    } else {
        session->status = AgentStatus::Queued;
        session->lastError.clear();
        if (!m_agentQueue.contains(sessionId))
            m_agentQueue.append(sessionId);
    }
    restoreAgentSteerMessageAfterFailedLaunch(sessionId);
    if (m_agentStore)
        m_agentStore->saveSession(*session);
    const QString notice =
        giveUp ? session->lastError
               : QString::fromUtf8("%1 exited (exit %2) before answering. "
                                "Restarting the session\xE2\x80\xA6")
                     .arg(codex ? QStringLiteral("Codex app-server")
                                : QStringLiteral("Claude Code"))
                     .arg(exitCode);
    noteAgentSessionNotice(sessionId, notice, /*error=*/giveUp);
    scheduleAgentSessionsPush();
    return !giveUp;
}

void MainWindow::markAgentSessionRunning(int sessionId)
{
    m_agentResultAwaitingCliTasks.remove(sessionId);
    m_agentCompletionChecks.remove(sessionId);
    m_agentBackgroundWork.remove(sessionId);
    applyAgentBackgroundWork();
    m_agentRelaunchAttempts.remove(sessionId);
    acknowledgeAgentSteerMessage(sessionId);
    m_agentDoneNotified.remove(sessionId);
    AgentSession *s = findAgentSession(sessionId);
    if (!s || s->status == AgentStatus::Running)
        return;
    const qint64 turnStartMs = QDateTime::currentMSecsSinceEpoch();
    // A resumed conversation keeps the startedAtMs of its first turn, so the
    // run-time ledger re-anchors here instead: the hours this session may have
    // sat finished between turns are not time it spent working. Banking has to
    // happen before the finish stamp it reads is cleared.
    s->beginRun(turnStartMs);
    s->status = AgentStatus::Running;
    s->lastError.clear();
    s->finishedAtMs = 0;
    s->merged = false;
    s->mergedAtMs = 0;
    s->mergeCandidateHead.clear();
    if (s->startedAtMs <= 0)
        s->startedAtMs = turnStartMs;
    if (m_agentStore && !isExternalSession(sessionId))
        m_agentStore->saveSession(*s);
    updateAgentStatusCell(sessionId);
}

void MainWindow::completeAgentSessionWhenSubprocessesExit(int sessionId)
{
    if (!m_agentCompletionChecks.contains(sessionId))
        return;
    if (const AgentSession *live = findAgentSession(sessionId);
        !live || live->status != AgentStatus::Running) {
        m_agentCompletionChecks.remove(sessionId);
        m_agentBackgroundWork.remove(sessionId);
        applyAgentBackgroundWork();
        return;
    }

    // Walking the process tree opens /proc/<pid>/stat for every process on the
    // machine, and this poll repeats every 250 ms for as long as the session
    // takes to drain — a stall was sampled here. Scan on a worker and decide
    // back on the GUI thread; the re-arm below keeps one scan per session in
    // flight.
    const qint64 processId = agentSessionProcessId(sessionId);
    runOffThread<QList<SystemStats::DescendantProcess>>(
        [processId] { return SystemStats::descendantProcesses(processId); },
        [this, sessionId](QList<SystemStats::DescendantProcess> subprocesses) {
            applyAgentSubprocessCompletionScan(sessionId, subprocesses);
        });
}

void MainWindow::trackAgentCliTask(int sessionId, const QJsonObject &event)
{
    forkmesh::agentclitasks::Ledger &ledger = m_agentCliTasks[sessionId];
    const int before = ledger.openBackground();
    const forkmesh::agentclitasks::Change change = ledger.apply(event);
    const bool settled = ledger.openBackground() == 0;
    if (ledger.openBackground() != before)
        notifyLocalControlAgentsChanged();
    if (ledger.isEmpty())
        m_agentCliTasks.remove(sessionId);
    if (change != forkmesh::agentclitasks::Change::Closed || !settled ||
        !m_agentResultAwaitingCliTasks.contains(sessionId))
        return;
    QTimer::singleShot(kAgentCliTaskSettleMs, this,
                       [this, sessionId] { settleAgentAfterCliTasks(sessionId); });
}

void MainWindow::settleAgentAfterCliTasks(int sessionId)
{
    if (!m_agentResultAwaitingCliTasks.contains(sessionId))
        return;
    if (const auto tasks = m_agentCliTasks.constFind(sessionId);
        tasks != m_agentCliTasks.constEnd() && tasks->openBackground() > 0)
        return;
    m_agentResultAwaitingCliTasks.remove(sessionId);
    const AgentSession *live = findAgentSession(sessionId);
    if (!live || live->status != AgentStatus::Running)
        return;
    m_agentCompletionChecks.insert(sessionId);
    completeAgentSessionWhenSubprocessesExit(sessionId);
}

void MainWindow::applyAgentSubprocessCompletionScan(
    int sessionId, const QList<SystemStats::DescendantProcess> &subprocesses)
{
    // The scan ran off-thread, so re-check everything it could have raced.
    if (!m_agentCompletionChecks.contains(sessionId))
        return;
    if (const AgentSession *live = findAgentSession(sessionId);
        !live || live->status != AgentStatus::Running) {
        m_agentCompletionChecks.remove(sessionId);
        m_agentBackgroundWork.remove(sessionId);
        applyAgentBackgroundWork();
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString closingNotice;

    if (!subprocesses.isEmpty()) {
        AgentBackgroundWork &work = m_agentBackgroundWork[sessionId];
        if (work.startedAtMs == 0) {
            work.startedAtMs = now;
            work.lastBusyAtMs = now; // unproven work counts as busy for one grace
        }
        ++work.polls;
        qint64 ticks = 0;
        for (const SystemStats::DescendantProcess &p : subprocesses)
            ticks += p.cpuTicks;
        work.busy = work.polls == 1 || ticks > work.cpuTicks;
        work.cpuTicks = ticks;
        if (work.busy)
            work.lastBusyAtMs = now;
        work.count = subprocesses.size();
        work.summary = agentSubprocessLabels(subprocesses).join(
            QStringLiteral(", "));

        const bool idle = now - work.lastBusyAtMs >= kAgentCompletionIdleGraceMs;
        const bool overdue = now - work.startedAtMs >= kAgentCompletionMaxWaitMs;
        const bool killed = work.killedAtMs > 0;
        const QString what = work.summary;
        const int count = work.count;
        const bool announce = !work.announced;
        work.announced = true;
        // Repainting the row (and pushing the session to the relay) is only
        // worth it when what the surfaces say has actually changed.
        const QString signature = QStringLiteral("%1|%2|%3")
                                      .arg(count)
                                      .arg(work.busy ? 1 : 0)
                                      .arg(what);
        const bool changed = work.shown != signature;
        work.shown = signature;
        if (!killed && (idle || overdue)) {
            work.killedAtMs = now;
            QList<qint64> pids;
            pids.reserve(subprocesses.size());
            for (const SystemStats::DescendantProcess &p : subprocesses)
                pids.append(p.pid);
            killExternalSessionPids(pids);
            closingNotice =
                QStringLiteral("Stopping %1 background task%2 that outlived the "
                               "turn %3: %4.")
                    .arg(count)
                    .arg(count == 1 ? QString() : QStringLiteral("s"),
                         idle ? QStringLiteral("without using any CPU")
                              : QStringLiteral("after 15 minutes"),
                         what);
        } else if (announce) {
            closingNotice =
                QStringLiteral("The turn is over but %1 background task%2 "
                               "started here %3 still running (%4). This session "
                               "stays Running until %5 finish.")
                    .arg(count)
                    .arg(count == 1 ? QString() : QStringLiteral("s"),
                         count == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                         what,
                         count == 1 ? QStringLiteral("it") : QStringLiteral("they"));
        }

        applyAgentBackgroundWork();
        // The notice pumps the event loop through the transcript, so nothing
        // above may be held across it; the state it needed is already captured.
        if (!closingNotice.isEmpty())
            noteAgentSessionNotice(sessionId, closingNotice);
        if (!m_agentCompletionChecks.contains(sessionId))
            return; // a new turn (or a stop) took the session while we spoke
        if (changed)
            updateAgentStatusCell(sessionId);
        else if (sessionId == m_selectedAgentSessionId)
            refreshAgentDetailMeta(sessionId);

        const auto pending = m_agentBackgroundWork.constFind(sessionId);
        const bool killTimedOut =
            pending != m_agentBackgroundWork.constEnd() &&
            pending->killedAtMs > 0 &&
            now - pending->killedAtMs >= kAgentCompletionKillGraceMs;
        if (!killTimedOut) {
            const int interval =
                pending != m_agentBackgroundWork.constEnd() &&
                        pending->polls < kAgentCompletionFastPolls
                    ? kAgentCompletionFastPollMs
                    : kAgentCompletionSlowPollMs;
            QTimer::singleShot(interval, this, [this, sessionId] {
                completeAgentSessionWhenSubprocessesExit(sessionId);
            });
            return;
        }
        // The stop signal did not take. Say so rather than let the session hang
        // on a process we cannot reap.
        closingNotice = QStringLiteral(
                            "%1 background task%2 ignored the stop signal and "
                            "%3 still running (%4); completing this session "
                            "anyway.")
                            .arg(count)
                            .arg(count == 1 ? QString() : QStringLiteral("s"),
                                 count == 1 ? QStringLiteral("is")
                                            : QStringLiteral("are"),
                                 what);
    }

    m_agentCompletionChecks.remove(sessionId);
    m_agentBackgroundWork.remove(sessionId);
    applyAgentBackgroundWork();
    if (!closingNotice.isEmpty())
        noteAgentSessionNotice(sessionId, closingNotice, /*error=*/true);
    AgentSession *session = findAgentSession(sessionId);
    if (!session || session->status != AgentStatus::Running)
        return;
    session->status = AgentStatus::Success;
    session->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    session->lastError.clear();
    if (m_agentStore && !isExternalSession(sessionId))
        m_agentStore->saveSession(*session);
    updateAgentStatusCell(sessionId);
    notifyAgentDone(sessionId);
}

// Copy the live drain state onto the sessions themselves. reloadAgents() reads
// m_agentSessions back from disk, where this state deliberately does not live,
// so every path that rebuilds the list calls this to put it back.
void MainWindow::applyAgentBackgroundWork()
{
    for (AgentSession &session : m_agentSessions) {
        const auto it = m_agentBackgroundWork.constFind(session.id);
        const bool pending = it != m_agentBackgroundWork.constEnd() &&
                             m_agentCompletionChecks.contains(session.id);
        session.backgroundProcesses = pending ? it->count : 0;
        session.backgroundProcessesBusy = pending && it->busy;
        session.backgroundProcessSummary = pending ? it->summary : QString();
    }
    if (m_agentTranscript && m_renderedTranscriptSession == m_selectedAgentSessionId)
        m_agentTranscript->setBackgroundWorkNote(
            agentBackgroundWorkNote(m_selectedAgentSessionId));
}

QString MainWindow::agentBackgroundWorkNote(int sessionId) const
{
    const AgentSession *session = nullptr;
    for (const AgentSession &s : m_agentSessions)
        if (s.id == sessionId)
            session = &s;
    if (!session || !session->finishingBackgroundWork())
        return QString();
    return QStringLiteral("%1 background task%2 still %3 (%4)")
        .arg(session->backgroundProcesses)
        .arg(session->backgroundProcesses == 1 ? QString() : QStringLiteral("s"),
             session->backgroundProcessesBusy ? QStringLiteral("working")
                                              : QStringLiteral("open"),
             session->backgroundProcessSummary);
}

// The agent's turn ended (or it needs permission) and it's now waiting on the
// user: flag the session "Waiting" in the list and raise a top-bar notification.
void MainWindow::notifyAgentWaiting(int sessionId, bool needsPermission)
{
    m_agentCompletionChecks.remove(sessionId);
    m_agentBackgroundWork.remove(sessionId);
    applyAgentBackgroundWork();
    AgentSession *s = findAgentSession(sessionId);
    if (!s || s->status != AgentStatus::Running)
        return; // only meaningful for a session that was actively running
    s->status = AgentStatus::Waiting;
    if (m_agentStore && !isExternalSession(sessionId))
        m_agentStore->saveSession(*s);
    updateAgentStatusCell(sessionId);

    const QString who = s->issueNumber > 0 ? QStringLiteral("#%1").arg(s->issueNumber)
                                           : QStringLiteral("Agent");
    const QString last = m_lastAssistantText.value(sessionId);
    const bool question = last.endsWith(QLatin1Char('?'));
    QString snippet = last;
    if (snippet.size() > 80)
        snippet = snippet.left(79) + QString::fromUtf8("\xE2\x80\xA6");
    const QString robot = QString::fromUtf8("\xF0\x9F\xA4\x96"); // 🤖
    QString msg;
    if (needsPermission)
        msg = QStringLiteral("%1 %2 needs your approval to continue").arg(robot, who);
    else if (question)
        msg = QStringLiteral("%1 %2 has a question: %3").arg(robot, who, snippet);
    else
        msg = QStringLiteral("%1 %2 is waiting for your reply").arg(robot, who);
    flashPingMessage(msg, /*error=*/false,
                     QStringLiteral("fm:agent:%1").arg(sessionId));
}

QString MainWindow::agentClosingSummary(int sessionId) const
{
    const QString cached = m_lastAssistantText.value(sessionId).trimmed();
    if (!cached.isEmpty())
        return cached;
    const QList<QJsonObject> events = m_streamEvents.value(sessionId);
    for (int i = events.size() - 1; i >= 0; --i) {
        const QJsonObject &ev = events.at(i);
        const QString type = ev.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("_codex_agent_complete")) {
            const QString text = ev.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty())
                return text;
            continue;
        }
        if (type != QLatin1String("assistant"))
            continue;
        QString text;
        const QJsonArray content = ev.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue &bv : content) {
            const QJsonObject block = bv.toObject();
            if (block.value(QStringLiteral("type")).toString() == QLatin1String("text"))
                text += block.value(QStringLiteral("text")).toString();
        }
        if (!text.trimmed().isEmpty())
            return text.trimmed();
    }
    return QString();
}

// The facts that say where a finished run came from: the issue it was working,
// the repository it ran in, how long it took and what it cost. The celebration
// card renders these unqualified — the repo is already on screen around it — but
// a desktop notification is read with no window behind it and needs the owner
// too, which is what qualifyRepo asks for.
QStringList MainWindow::agentDoneFacts(int sessionId, bool qualifyRepo)
{
    QStringList facts;
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return facts;
    if (session->issueNumber > 0)
        facts << QStringLiteral("#%1").arg(session->issueNumber);
    if (!session->name.isEmpty())
        facts << (qualifyRepo && !session->owner.isEmpty()
                      ? session->owner + QLatin1Char('/') + session->name
                      : session->name);
    // The turn that just ended, not the calendar span back to the session's
    // first one: a resumed conversation keeps its original startedAtMs, so
    // subtracting it here used to report the days it had existed for.
    const qint64 ran = session->currentRunMs(QDateTime::currentMSecsSinceEpoch());
    if (ran > 0)
        facts << formatDuration(ran);
    else if (session->durationMs > 0)
        facts << formatDuration(session->durationMs);
    if (session->costUsd > 0)
        facts << QStringLiteral("$%1").arg(session->costUsd, 0, 'f', 2);
    return facts;
}

// A desktop notification is read out of context, hours later, stacked among
// others that look just like it: it has to say which run it is about before it
// says what the run did. Provenance leads, the agent's closing summary follows.
QString MainWindow::agentDoneNotificationBody(int sessionId,
                                              const QString &summary)
{
    QStringList facts = agentDoneFacts(sessionId, /*qualifyRepo=*/true);
    if (const AgentSession *session = findAgentSession(sessionId)) {
        const QString agent = session->model.trimmed().isEmpty()
                                  ? agentProviderName(session->provider)
                                  : session->model.trimmed();
        if (!agent.isEmpty())
            facts << agent;
    }
    const QString trail = facts.join(QString::fromUtf8(" \xC2\xB7 ")); // ·
    if (trail.isEmpty())
        return summary;
    return trail + QLatin1Char('\n') + summary;
}

// A run reached the end: celebrate it. The card carries the agent's own list
// icon, a headline naming the run, and the summary it signed off with, and the
// window edge pulses green so the news lands even when the Agents tab is closed
// Firing once per run is m_agentDoneNotified's job — completion
// can be reached more than once (a re-fired signal, a requeue), and
// markAgentSessionRunning() clears the mark when the next turn starts.
void MainWindow::notifyAgentDone(int sessionId)
{
    if (m_agentDoneNotified.contains(sessionId))
        return;
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || session->status != AgentStatus::Success)
        return;
    if (isExternalSession(sessionId))
        return;
    m_agentDoneNotified.insert(sessionId);
    completeSuccessfulLogMonitorRun(sessionId);
    handleTeammateFinished(sessionId);
    finishTeammateCreatorRun(sessionId);
    finishPullMergeAllRun(sessionId);
    markTeammateDoneUnseen(sessionId);

    QString summary = agentClosingSummary(sessionId).simplified();
    if (summary.isEmpty())
        summary = QStringLiteral("Finished with no closing summary.");
    if (summary.size() > kAgentDoneSummaryChars)
        summary = summary.left(kAgentDoneSummaryChars - 1) +
                  QString::fromUtf8("\xE2\x80\xA6"); // …
    flashPingMessage(summary, /*error=*/false,
                     QStringLiteral("fm:agent:%1").arg(sessionId), kAgentDonePingKind);
    // The completion and its Ping were still recorded above; only its visual
    // celebration is an interruption that a notification pause suppresses.
    if (!pingsPaused())
        flashCelebrationBorder();
    if (!QSettings().value(kAgentDoneAlertSetting, true).toBool())
        return;
    notifyIfInactive(QStringLiteral("ForkMesh %1 Agent #%2 is done!")
                         .arg(QString::fromUtf8("\xE2\x80\x94")) // —
                         .arg(sessionId),
                     agentDoneNotificationBody(sessionId, summary),
                     QStringLiteral("fm:agent:%1").arg(sessionId),
                     agentNotificationIconPath(sessionId));
}

void MainWindow::updateAgentStatusCell(int sessionId)
{
    scheduleAgentSessionsPush();
    AgentSession *s = findAgentSession(sessionId);
    if (!s)
        return;
    noteClaudeDesktopCandidate(*s);
    syncOrgTaskAgentStatus(sessionId);
    if (!s->orgTaskId.isEmpty())
        notifyPackagesAgentSessionsChanged();
    if (auto it = m_streamSessionInfo.find(sessionId); it != m_streamSessionInfo.end())
        it->status = s->status;
    if (!m_agentTable)
        return;
    for (int r = 0; r < m_agentTable->rowCount(); ++r) {
        QTableWidgetItem *idItem = m_agentTable->item(r, kAgentIdColumn);
        if (!idItem || idItem->data(Qt::UserRole).toInt() != sessionId)
            continue;
        QSignalBlocker block(m_agentTable);
        applyAgentStatusCell(idItem, *s, m_agentDiffStats.value(sessionId),
                             QString(), sessionId, agentSessionPullAvatar(*s),
                             agentSpinAngle(*s), agentRunToggleMode(*s));
        applyAgentRowChips(r, *s, m_agentDiffStats.value(sessionId));
        break;
    }
    refreshAgentDotMatrix();
    updateAgentsTabIndicator();
    if (sessionId == m_selectedAgentSessionId) {
        updateAgentActionState();
        refreshAgentStatusPill(sessionId);
        // Status text must change immediately; only the expensive process rows
        // arrive asynchronously after the state transition.
        refreshAgentDetailMeta(sessionId, /*forceProcessRefresh=*/true);
    }
    maybeStartQueuedRebuild();
    scheduleAgentQueuePump();
}

// Coalesces the tool results of a busy fleet into one diff pass. A pending
// deadline is kept rather than pushed back, so a stream of edits still gets
// measured while it goes on instead of only once it stops.
void MainWindow::scheduleAgentLiveDiffRefresh(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (session->owner != repo.owner || session->name != repo.name)
        return;
    m_agentLiveDiffSessions.insert(sessionId);
    armAgentDiffPassTimer(400);
}

qint64 MainWindow::agentDiffCooldownRemainingMs() const
{
    if (m_agentDiffCooldownWaived || !m_agentDiffPassEnded.isValid())
        return 0;
    const qint64 gap =
        qBound(qint64(1000), m_agentDiffPassMs * 3, qint64(15000));
    return qMax<qint64>(0, gap - m_agentDiffPassEnded.elapsed());
}

void MainWindow::armAgentDiffPassTimer(int delayMs)
{
    if (m_agentDiffStatsRefreshing)
        return;
    if (!m_agentLiveDiffTimer) {
        m_agentLiveDiffTimer = new QTimer(this);
        m_agentLiveDiffTimer->setSingleShot(true);
        connect(m_agentLiveDiffTimer, &QTimer::timeout, this, [this] {
            if (m_agentTableRefreshing) {
                m_agentLiveDiffTimer->start(400);
                return;
            }
            if (m_agentDiffStatsRefreshing)
                return;
            if (!m_agentDiffStatsRefreshQueued &&
                m_agentLiveDiffSessions.isEmpty())
                return;
            m_agentDiffLivePass =
                !m_agentDiffStatsRefreshQueued && !m_agentDiffRefreshPending;
            m_agentDiffRefreshPending = true;
            refreshAgentTable();
            m_agentDiffLivePass = false;
        });
    }
    if (m_agentLiveDiffTimer->isActive())
        return;
    const qint64 wait = qMax<qint64>(delayMs, agentDiffCooldownRemainingMs());
    m_agentLiveDiffTimer->start(static_cast<int>(wait));
}

void MainWindow::applyAgentDiffStatResult(int generation, int repoIndex,
                                          int sessionId,
                                          const AgentDiffStat &stat,
                                          const QString &signature)
{
    if (generation != m_agentDiffStatsGen || repoIndex != m_repoDetailIndex)
        return;
    const auto previous = m_agentDiffStats.constFind(sessionId);
    const bool unchanged = previous != m_agentDiffStats.constEnd() &&
                           sameAgentDiffStat(*previous, stat);
    m_agentDiffSig.insert(sessionId, signature);
    if (unchanged)
        return;
    m_agentDiffStats.insert(sessionId, stat);
    if (!stat.worktree.isEmpty())
        m_sessionWorkdirCache.insert(sessionId, stat.worktree);
    if (sessionId == m_selectedAgentSessionId)
        refreshAgentSyncControls(sessionId);

    const AgentSession *session = findAgentSession(sessionId);
    if (!session || !m_agentTable)
        return;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_agentTable->item(row, kAgentIdColumn);
        if (!item || item->data(Qt::UserRole).toInt() != sessionId)
            continue;
        QSignalBlocker blocker(m_agentTable);
        applyAgentStatusCell(item, *session, stat, QString(), sessionId,
                             agentSessionPullAvatar(*session),
                             agentSpinAngle(*session),
                             agentRunToggleMode(*session));
        applyAgentRowChips(row, *session, stat);
        m_agentTable->viewport()->update(m_agentTable->visualItemRect(item));
        break;
    }
}

void MainWindow::refreshAgentStatusPill(int sessionId)
{
    if (!m_agentStatusPill || sessionId != m_selectedAgentSessionId)
        return;
    AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    const qreal activityAngle =
        !session->merged && session->status == AgentStatus::Running
            ? agentSpinAngle(*session)
            : 0.0;
    m_agentStatusPill->setProperty("outcomeTone", agentStatusBadgeTone(*session));
    m_agentStatusPill->setIcon(agentStatusPillIcon(*session, activityAngle));
    m_agentStatusPill->setText(agentStatusPillText(*session));
    m_agentStatusPill->setToolTip(agentStatusBadgeToolTip(*session));
    m_agentStatusPill->style()->unpolish(m_agentStatusPill);
    m_agentStatusPill->style()->polish(m_agentStatusPill);
    m_agentStatusPill->show();
}

void MainWindow::showAgentMetaPopup()
{
    if (!m_agentStatusPill || !m_agentMetaPopup)
        return;
    m_agentMetaPopup->adjustSize();
    QPoint at = m_agentStatusPill->mapToGlobal(
        QPoint(0, m_agentStatusPill->height() + 4));
    const QScreen *screen = m_agentMetaPopup->screen()
                                ? m_agentMetaPopup->screen()
                                : QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        at.setX(qBound(avail.left(),
                       qMin(at.x(), avail.right() - m_agentMetaPopup->width() + 1),
                       avail.right()));
    }
    m_agentMetaPopup->move(at);
    m_agentMetaPopup->show();
}

void MainWindow::toggleAgentMetaPopup()
{
    if (!m_agentMetaPopup)
        return;
    if (m_agentMetaPopup->isVisible()) {
        m_agentMetaPopup->hide();
        return;
    }
    showAgentMetaPopup();
}

void MainWindow::hideAgentMetaPopupIfPointerAway()
{
    if (!m_agentMetaPopup || !m_agentMetaPopup->isVisible())
        return;
    if ((!m_agentStatusPill || !m_agentStatusPill->underMouse()) &&
        !m_agentMetaPopup->underMouse())
        m_agentMetaPopup->hide();
}

// How hard a session is working right now, 0..1: whichever of its raw-output
// burst and its token throughput is the louder.
double MainWindow::agentActivityMeter(const AgentSession &session) const
{
    return qMax(qBound(0.0, m_scannerStates.value(session.id).intensity, 1.0),
                qBound(0.0,
                       agentTokensPerSecond(session, sessionTokenTotal(session)) /
                           kAgentFastTokensPerSecond,
                       1.0));
}

// Where this session's fireball head sits right now.  A pure read: repaints
// that land between animation frames - a status flip, a diff-stat result, a
// table rebuild - redraw the head where it already was instead of snapping it
// back to twelve o'clock.
qreal MainWindow::agentSpinAngle(const AgentSession &session) const
{
    const auto it = m_agentSpinStates.constFind(session.id);
    return it != m_agentSpinStates.constEnd() ? it->phaseDeg
                                              : agentSpinSeedDegrees(session);
}

// One frame of orbit for every running fireball: ease the speed toward the live
// meter, then carry the head forward at that speed.  Both halves are what keeps
// a change of speed smooth - the head never moves backwards or teleports, and
// the rate it moves at slides into its new value over a few frames.
void MainWindow::advanceAgentSpinPhases()
{
    // Paced off elapsed time rather than tick count: the 60 ms timer slips
    // whenever the GUI thread is busy, and following the clock holds the head's
    // speed steady through that instead of stuttering along with the timer.
    qreal frames = 1.0;
    if (m_agentSpinClock.isValid())
        frames = qBound(kAgentSpinMinFrames,
                        qreal(m_agentSpinClock.restart()) / qreal(kAgentSpinTickMs),
                        kAgentSpinMaxFrames);
    else
        m_agentSpinClock.start();

    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.merged || session.status != AgentStatus::Running)
            continue;
        AgentSpinState &spin = m_agentSpinStates[session.id];
        if (!spin.seeded) {
            spin.seeded = true;
            spin.phaseDeg = agentSpinSeedDegrees(session);
            spin.meter = agentActivityMeter(session); // no ramp on the first frame
        }
        spin.meter += (agentActivityMeter(session) - spin.meter) *
                      qMin(1.0, kAgentSpinMeterEase * double(frames));
        spin.phaseDeg = std::fmod(
            spin.phaseDeg + agentSpinStepDegrees(spin.meter) * frames, 360.0);
    }
}

void MainWindow::animateRunningAgentIcons()
{
    if (!m_agentTable)
        return;
    ++m_agentSpinTicks;
    advanceAgentSpinPhases();
    const bool refreshMeta = agentDetailMetaRefreshDue(m_agentSpinTicks);
    QSignalBlocker block(m_agentTable);
    for (int r = 0; r < m_agentTable->rowCount(); ++r) {
        QTableWidgetItem *idItem = m_agentTable->item(r, kAgentIdColumn);
        if (!idItem)
            continue;
        const AgentSession *s = findAgentSession(idItem->data(Qt::UserRole).toInt());
        if (!s || s->merged || s->status != AgentStatus::Running)
            continue;
        const qreal angle = agentSpinAngle(*s);
        const QPixmap lead =
            agentLeadGlyphPixmap(*s, agentSessionPullAvatar(*s), angle);
        idItem->setIcon(lead.isNull() ? QIcon() : QIcon(lead));
        idItem->setData(kAgentSpinAngleRole, angle);
        m_agentTable->viewport()->update(m_agentTable->visualItemRect(idItem));
        if (m_agentListView == AgentListView::Full)
            if (QTableWidgetItem *issueItem = m_agentTable->item(r, kAgentIssueColumn))
                m_agentTable->viewport()->update(
                    m_agentTable->visualItemRect(issueItem));
        if (QTableWidgetItem *issueItem = m_agentTable->item(r, kAgentIssueColumn)) {
            const QString progress = agentTaskProgress(s->id);
            if (issueItem->data(kAgentTaskProgressRole).toString() != progress) {
                issueItem->setData(kAgentTaskProgressRole, progress);
                issueItem->setText(agentTitleWithProgress(
                    issueItem->data(kAgentTitleBaseRole).toString(), progress));
            }
        }
        if (m_agentListView == AgentListView::Compact)
            if (QTableWidgetItem *issueItem = m_agentTable->item(r, kAgentIssueColumn)) {
                const QString ticker =
                    agentTickerText(s->id, m_lastAssistantText.value(s->id));
                if (issueItem->data(kAgentTickerRole).toString() != ticker)
                    issueItem->setData(kAgentTickerRole, ticker);
            }
        if (refreshMeta && s->id == m_selectedAgentSessionId)
            refreshAgentDetailMeta(s->id);
    }
    const AgentSession *selected = findAgentSession(m_selectedAgentSessionId);
    if (m_agentStatusPill && selected && !selected->merged &&
        selected->status == AgentStatus::Running) {
        m_agentStatusPill->setIcon(
            agentStatusPillIcon(*selected, agentSpinAngle(*selected)));
    }
}

qint64 MainWindow::sessionTokenTotal(const AgentSession &session) const
{
    return qMax(m_sessionTokens.value(session.id, 0),
                static_cast<qint64>(session.totalTokens));
}

void MainWindow::seedSessionTokens()
{
    for (const AgentSession &s : std::as_const(m_agentSessions))
        m_sessionTokens[s.id] =
            qMax(m_sessionTokens.value(s.id, 0), static_cast<qint64>(s.totalTokens));
}

void MainWindow::setAgentUsageLabel(const AgentSession &session)
{
    if (!m_navTokenUsage)
        return;
    const int window = session.contextWindow > 0 ? session.contextWindow : 32000;
    const int maxOutput =
        session.maxOutputTokens > 0
            ? session.maxOutputTokens
            : qMax(256, QSettings().value(kAgentMaxOutputSetting, 2000).toInt());
    const int pct = window > 0 ? qMin(100, session.contextTokens * 100 / window) : 0;
    static_cast<TokenUsageMiniChart *>(m_navTokenUsage)->setStats(
        QStringLiteral("Session token usage: %1 total (%2 prompt estimate, %3 transcript estimate) · budget: context %4/%5 (%6%), max output %7 tokens · credits ~%8 · cost ~%9")
            .arg(formatCount(sessionTokenTotal(session)))
            .arg(formatCount(session.promptTokens))
            .arg(formatCount(session.completionTokens))
            .arg(formatCount(session.contextTokens))
            .arg(formatCount(window))
            .arg(pct)
            .arg(formatCount(maxOutput))
            .arg(formatCount(session.estimatedCredits))
            .arg(agentCostText(session.costUsd)));
}

void MainWindow::updateAgentTokenCell(int sessionId)
{
    if (sessionId == m_selectedAgentSessionId)
        if (const AgentSession *s = findAgentSession(sessionId)) {
            setAgentUsageLabel(*s);
            refreshAgentDetailMeta(sessionId);
        }
    refreshAgentDotMatrix();
}

void MainWindow::updateAgentCostCell(int sessionId)
{
    if (sessionId == m_selectedAgentSessionId)
        refreshAgentDetailMeta(sessionId);
}

void MainWindow::updateAgentRunSummaryCells(int sessionId)
{
    if (sessionId == m_selectedAgentSessionId)
        refreshAgentDetailMeta(sessionId);
}

void MainWindow::noteAgentActivity(int sessionId, int bytes)
{
    if (sessionId <= 0)
        return;
    AgentScannerState &st = m_scannerStates[sessionId];
    st.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    const double bump = bytes > 0 ? qMin(1.0, bytes / 512.0) : 0.5;
    st.intensity = qMin(1.0, st.intensity + bump);
    if (m_scannerTimer && !m_scannerTimer->isActive())
        m_scannerTimer->start();
}

void MainWindow::onScannerTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool anyActive = false;
    for (auto it = m_scannerStates.begin(); it != m_scannerStates.end(); ++it) {
        it->intensity *= 0.85;
        if (it->intensity < 0.01)
            it->intensity = 0.0;
        if (it->intensity <= 0.0 && now - it->lastActivityMs >= kScannerIdleMs)
            continue;
        anyActive = true;
    }
    refreshAgentDots();
    if (!anyActive && m_scannerTimer)
        m_scannerTimer->stop();
}

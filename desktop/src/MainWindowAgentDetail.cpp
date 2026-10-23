
#include "MainWindow.h"
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

// Wall clock for a session stamp. Every time the detail card shows is relative
// — "4m", "4m 08s" — so "what time did this start" could only be had by
// subtracting one of them from the current time. Same format the row hover
// already uses for its Updated stamp.
QString agentClockText(qint64 timestampMs)
{
    return QDateTime::fromMSecsSinceEpoch(timestampMs)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString agentSpeedText(const AgentSession &s, qint64 tokens)
{
    const double rate = agentTokensPerSecond(s, tokens);
    return rate > 0 ? QStringLiteral("%1 tok/s").arg(rate, 0, 'f', 1)
                    : QStringLiteral("-");
}

QString agentDiffSummaryText(const AgentDiffStat &stat)
{
    QStringList parts;
    if (stat.files >= 0)
        parts << (stat.files == 1 ? QStringLiteral("1 file")
                                  : QStringLiteral("%1 files").arg(stat.files));
    if (stat.added >= 0 && stat.removed >= 0)
        parts << QStringLiteral("+%1 -%2").arg(stat.added).arg(stat.removed);
    if (stat.ahead >= 0 && stat.behind >= 0 && (stat.ahead > 0 || stat.behind > 0))
        parts << QString::fromUtf8("\xE2\x86\x91%1 \xE2\x86\x93%2")
                     .arg(stat.ahead)
                     .arg(stat.behind);
    return parts.isEmpty() ? QStringLiteral("-")
                           : parts.join(QString::fromUtf8("  \xC2\xB7 "));
}

static QString chipLinkHtml(const QString &href, const QString &labelHtml)
{
    return QStringLiteral(
               "<a href=\"%1\" style=\"color:#c9d1d9;background-color:#21262d;"
               "text-decoration:none\">&nbsp;%2&nbsp;</a>")
        .arg(href, labelHtml);
}

static QString pullLinkHtml(int prNumber)
{
    const QString href = kPullLinkScheme + QString::number(prNumber);
    return chipLinkHtml(href, QStringLiteral("PR #%1 open").arg(prNumber));
}

static QString issueLinkHtml(int issueNumber, const QString &title)
{
    const QString href = kIssueLinkScheme + QString::number(issueNumber);
    const QString label =
        title.isEmpty()
            ? QStringLiteral("issue #%1").arg(issueNumber)
            : QStringLiteral("issue #%1 %2").arg(issueNumber).arg(title.toHtmlEscaped());
    return chipLinkHtml(href, label);
}

static QString agentDetailTableHtml(const QStringList &headers, const QStringList &values)
{
    Q_ASSERT(headers.size() == values.size());
    QString html = QStringLiteral(
        "<table style='border-collapse:collapse;' cellspacing='0' cellpadding='0'>");
    for (int i = 0; i < headers.size(); ++i) {
        html += QStringLiteral(
                    "<tr><th style='text-align:left; font-weight:normal; "
                    "color:#8b949e; padding:0 16px 3px 0;'>%1</th>"
                    "<td style='text-align:left; padding:0 0 3px 0;'>%2</td></tr>")
                    .arg(headers.at(i).toHtmlEscaped(), values.at(i));
    }
    html += QStringLiteral("</table>");
    return html;
}

static void fitAgentMetaWidth(QLabel *label)
{
    if (!label)
        return;
    QTextDocument doc;
    doc.setDefaultFont(label->font());
    doc.setHtml(label->text());
    doc.setTextWidth(-1); // lay the table out at its natural width
    const QScreen *screen = label->screen() ? label->screen()
                                            : QGuiApplication::primaryScreen();
    const int maxWidth =
        screen ? qMax(420, int(screen->availableGeometry().width() * 0.6)) : 900;
    const int ideal = int(doc.idealWidth()) + 4; // +4: rounding + the label frame
    label->setMinimumWidth(qBound(420, ideal > 4 ? ideal : 520, maxWidth));
    label->setMaximumWidth(maxWidth);
}

static QString agentSubprocessText(
    const QList<SystemStats::DescendantProcess> &processes)
{
    QStringList labels = agentSubprocessLabels(processes);
    for (QString &label : labels)
        label = label.toHtmlEscaped();
    return QStringLiteral("%1 running &middot; %2")
        .arg(processes.size())
        .arg(labels.join(QStringLiteral(", ")));
}

struct AgentProcessMetaSnapshot {
    qint64 processId = 0;
    SystemStats::DescendantLoad compilers;
    QString subprocessText;
};

} // namespace

qint64 MainWindow::agentSessionProcessId(int sessionId) const
{
#ifdef FORKMESH_WINDOW_TESTS
    if (const auto testPid = m_testAgentSessionProcessIds.constFind(sessionId);
        testPid != m_testAgentSessionProcessIds.constEnd())
        return testPid.value();
#endif
    if (ClaudeStreamSession *stream = m_streamSessions.value(sessionId))
        return stream->processId();
    if (CodexAppServerSession *codex = m_codexStreams.value(sessionId))
        return codex->processId();
    for (AgentRunner *runner : m_agentRunners) {
        if (runner && runner->busy() && runner->currentSessionId() == sessionId)
            return runner->processId();
    }
    return 0;
}

void MainWindow::scheduleAgentProcessMetaRefresh(qint64 processId,
                                                 bool forceRefresh)
{
    if (processId <= 0) {
        if (m_agentProcessMetaPid != 0 || m_agentProcessMetaRefreshInFlight) {
            ++m_agentProcessMetaRefreshGeneration;
            m_agentProcessMetaRefreshInFlight = false;
            m_agentProcessMetaRefreshPid = 0;
            m_agentProcessMetaPid = 0;
            m_agentProcessMetaSampledAtMs = 0;
            m_agentProcessMetaCompilerCount = 0;
            m_agentProcessMetaCompilerResidentBytes = 0;
            m_agentProcessMetaSubprocessText.clear();
        }
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool cacheFresh =
        m_agentProcessMetaPid == processId &&
        m_agentProcessMetaSampledAtMs > 0 &&
        now - m_agentProcessMetaSampledAtMs < kAgentProcessMetaCacheMs;
    if (!forceRefresh && cacheFresh)
        return;
    // Selection changes and status transitions may both request an immediate
    // update.  One worker for the same PID is enough; its completion repaints
    // the selected header with the fresh process rows.
    if (m_agentProcessMetaRefreshInFlight &&
        m_agentProcessMetaRefreshPid == processId)
        return;

    const quint64 generation = ++m_agentProcessMetaRefreshGeneration;
    m_agentProcessMetaRefreshInFlight = true;
    m_agentProcessMetaRefreshPid = processId;
#ifdef FORKMESH_WINDOW_TESTS
    ++m_testAgentProcessMetaScanLaunches;
#endif

    auto *watcher = new QFutureWatcher<AgentProcessMetaSnapshot>(this);
    connect(watcher, &QFutureWatcher<AgentProcessMetaSnapshot>::finished, this,
            [this, watcher, generation] {
                const AgentProcessMetaSnapshot snapshot = watcher->result();
                watcher->deleteLater();
                if (generation != m_agentProcessMetaRefreshGeneration)
                    return;

                m_agentProcessMetaRefreshInFlight = false;
                m_agentProcessMetaRefreshPid = 0;
                m_agentProcessMetaPid = snapshot.processId;
                m_agentProcessMetaSampledAtMs =
                    QDateTime::currentMSecsSinceEpoch();
                m_agentProcessMetaCompilerCount = snapshot.compilers.count;
                m_agentProcessMetaCompilerResidentBytes =
                    snapshot.compilers.residentBytes;
                m_agentProcessMetaSubprocessText = snapshot.subprocessText;

                const int selected = m_selectedAgentSessionId;
                if (selected > 0 &&
                    agentSessionProcessId(selected) == snapshot.processId)
                    refreshAgentDetailMeta(selected);
            });
    watcher->setFuture(QtConcurrent::run([processId] {
        AgentProcessMetaSnapshot snapshot;
        snapshot.processId = processId;
        snapshot.compilers =
            SystemStats::descendantsNamed(processId, QStringLiteral("cc1plus"));
        const QList<SystemStats::DescendantProcess> subprocesses =
            SystemStats::descendantProcesses(processId);
        if (!subprocesses.isEmpty())
            snapshot.subprocessText = agentSubprocessText(subprocesses);
        return snapshot;
    }));
}

void MainWindow::refreshAgentTaskGradeControl(const AgentSession *session)
{
    if (!m_agentTaskGradeCombo)
        return;
    const bool available = session && session->hasTeammateAttribution() &&
                           AgentStatus::isTerminal(session->status);
    m_agentTaskGradeCombo->setVisible(available);
    if (!available)
        return;

    const int grade = qBound(0, session->taskGrade, 5);
    const QSignalBlocker block(m_agentTaskGradeCombo);
    const int index = m_agentTaskGradeCombo->findData(grade);
    m_agentTaskGradeCombo->setCurrentIndex(index >= 0 ? index : 0);
    const QString teammate = session->teammateName.isEmpty()
                                 ? session->teammateId
                                 : session->teammateName;
    m_agentTaskGradeCombo->setAccessibleName(
        QStringLiteral("Grade %1's finished task").arg(teammate));
    m_agentTaskGradeCombo->setToolTip(
        grade == 0
            ? QStringLiteral("Rate %1's finished task. The rating appears in "
                             "the Special agent performance chart.")
                  .arg(teammate)
            : QStringLiteral("Current task grade: %1 out of 5. Change it to "
                             "update the Special agent performance chart.")
                  .arg(grade));
}

void MainWindow::refreshAgentDetailMeta(int sessionId, bool forceProcessRefresh)
{
    if (!m_agentMeta)
        return;
    const AgentSession *liveSession = findAgentSession(sessionId);
    refreshAgentSyncControls(sessionId);
    if (!liveSession) {
        refreshAgentTaskGradeControl(nullptr);
        return;
    }
    refreshAgentTaskGradeControl(liveSession);
    const AgentSession sessionSnapshot = *liveSession;
    const AgentSession *session = &sessionSnapshot;
    const QString mergedMeta =
        session->merged
            ? QStringLiteral("<span style='color:%1'>merged into %2</span>")
                  .arg(Theme::nextOrHex(Theme::Role::FgSecondary,
                                        QStringLiteral("#a371f7")),
                       agentMergeBase(*session).toHtmlEscaped())
            : QString();
    QString worktreePath;
    if (!session->branchName.isEmpty()) {
        const int repoIdx = repoIndexFor(session->owner, session->name);
        if (repoIdx >= 0)
            worktreePath = cachedSessionWorktreeForUi(
                sessionId, m_repositories.at(repoIdx).localPath,
                session->branchName);
    }
    if (m_agentBranchButton) {
        m_agentBranchButton->setVisible(!session->branchName.isEmpty());
        m_agentBranchButton->setToolTip(
            session->branchName.isEmpty() ? QString()
            : isCloudSession(sessionId)
                ? QStringLiteral("Fetch %1 from the mirror and review its "
                                 "commits, files and diff in the Git view")
                      .arg(session->branchName)
                : QStringLiteral("Review %1's commits, files and diff in the "
                                 "Git view")
                      .arg(session->branchName));
    }
    if (m_agentWorktreeButton) {
        m_agentWorktreeButton->setVisible(!worktreePath.isEmpty());
        m_agentWorktreeButton->setToolTip(
            worktreePath.isEmpty()
                ? QString()
                : QStringLiteral("Open worktree %1").arg(worktreePath));
    }
    const QString conversationId = agentCliConversationId(sessionId);
    if (m_agentPopOutButton) {
        const bool cliSession = isExternalSession(sessionId) ||
                                agentIsCodexProvider(session->provider) ||
                                session->provider == QLatin1String("claude-code");
        m_agentPopOutButton->setEnabled(cliSession && !session->cloud);
        m_agentPopOutButton->setToolTip(
            session->cloud
                ? QStringLiteral("This agent runs in the cloud — its CLI "
                                 "conversation is not on this machine.")
            : !cliSession
                ? QStringLiteral("%1 sessions run against the API — there is no "
                                 "CLI conversation to continue in a terminal.")
                      .arg(agentProviderName(session->provider))
                : conversationId.isEmpty()
                      ? QStringLiteral("Stop this session and open its agent CLI "
                                       "in a system terminal. It has no "
                                       "conversation to resume yet, so the CLI "
                                       "starts fresh in the same directory.")
                      : QStringLiteral("Stop this session and continue "
                                       "conversation %1 in a system terminal. "
                                       "Continue above picks it back up here.")
                            .arg(conversationId));
    }
    if (session->cloud) {
        const forkmesh::agents::CloudAgentSession cloud =
            m_cloudAgentSessions.value(sessionId);
        QStringList headers;
        QStringList values;
        headers << QStringLiteral("Agent");
        values << QStringLiteral("%1 · cloud")
                      .arg(agentProviderName(session->provider))
                      .toHtmlEscaped();
        headers << QStringLiteral("Runs on");
        values << (cloud.targetNode.isEmpty()
                       ? QStringLiteral("<span style='color:#8b949e'>not "
                                        "claimed yet</span>")
                       : cloud.targetNode.toHtmlEscaped());
        headers << QStringLiteral("Repo");
        values << QStringLiteral("%1/%2").arg(session->owner.toHtmlEscaped(),
                                              session->name.toHtmlEscaped());
        if (!session->branchName.isEmpty()) {
            headers << QStringLiteral("Branch");
            values << (cloud.baseBranch.isEmpty()
                           ? session->branchName.toHtmlEscaped()
                           : QStringLiteral("%1 → %2")
                                 .arg(session->branchName.toHtmlEscaped(),
                                      cloud.baseBranch.toHtmlEscaped()));
        }
        if (cloud.diffKnown) {
            headers << QStringLiteral("Changes");
            values << (cloud.diffFiles == 0 && cloud.diffAhead == 0
                           ? QStringLiteral("no changes against the base")
                           : QStringLiteral("%1 file%2 <span style='color:#3fb950'>"
                                            "+%3</span> <span style='color:#f85149'>"
                                            "−%4</span>")
                                 .arg(cloud.diffFiles)
                                 .arg(cloud.diffFiles == 1 ? QString()
                                                           : QStringLiteral("s"))
                                 .arg(cloud.diffAdded)
                                 .arg(cloud.diffRemoved));
        }
        if (cloud.mergeState == QLatin1String("merging") ||
            cloud.mergeState == QLatin1String("conflict")) {
            headers << QStringLiteral("Merge");
            values << (cloud.mergeState == QLatin1String("merging")
                           ? QStringLiteral("merging…")
                           : QStringLiteral("<span style='color:#f85149'>"
                                            "conflicts with the base</span>"));
        }
        if (session->prNumber > 0) {
            headers << QStringLiteral("PR");
            values << pullLinkHtml(session->prNumber);
        }
        if (session->issueNumber > 0) {
            headers << QStringLiteral("Issue");
            values << issueLinkHtml(session->issueNumber, session->issueTitle);
        }
        headers << QStringLiteral("Status");
        values << agentStatusText(session->status).toHtmlEscaped();
        if (!cloud.diagnostic.isEmpty()) {
            headers << (cloud.needsAttention ? QStringLiteral("Attention")
                                             : QStringLiteral("Relay"));
            values << cloud.diagnostic.toHtmlEscaped();
        }
        if (!cloud.createdBy.isEmpty()) {
            headers << QStringLiteral("Queued by");
            values << cloud.createdBy.toHtmlEscaped();
        }
        if (!session->model.isEmpty()) {
            headers << QStringLiteral("Model");
            values << session->model.toHtmlEscaped();
        }
        headers << QStringLiteral("Follow-ups");
        values << (cloud.promptable()
                       ? QStringLiteral("type below to message this agent")
                       : QStringLiteral("<span style='color:#8b949e'>not yet "
                                        "— no node has started it</span>"));
        QString meta = agentDetailTableHtml(headers, values);
        if (!mergedMeta.isEmpty())
            meta += QStringLiteral("<br>") + mergedMeta;
        applyAgentMetaHtml(meta);
        return;
    }
    if (isExternalSession(sessionId)) {
        QStringList headers;
        QStringList values;
        headers << QStringLiteral("Agent");
        values << QStringLiteral("External Claude Code");
        headers << QStringLiteral("Repo");
        values << QStringLiteral("%1/%2").arg(session->owner.toHtmlEscaped(),
                                              session->name.toHtmlEscaped());
        if (!session->branchName.isEmpty()) {
            headers << QStringLiteral("Branch");
            values << session->branchName.toHtmlEscaped();
        }
        if (!worktreePath.isEmpty()) {
            headers << QStringLiteral("Worktree");
            values << worktreePath.toHtmlEscaped();
        }
        headers << QStringLiteral("Status");
        values << agentStatusText(session->status).toHtmlEscaped();
        headers << QStringLiteral("Mode");
        values << QStringLiteral("watch-only");
        headers << QStringLiteral("Session");
        values << (conversationId.isEmpty()
                       ? QStringLiteral("<span style='color:#8b949e'>unknown</span>")
                       : conversationId.toHtmlEscaped());
        QString meta = agentDetailTableHtml(headers, values);
        if (!mergedMeta.isEmpty())
            meta += QStringLiteral("<br>") + mergedMeta;
        applyAgentMetaHtml(meta);
        return;
    }
    QString pr =
        session->prNumber > 0
            ? pullLinkHtml(session->prNumber)
            : (session->createPr ? QStringLiteral("PR opens on finish")
                                 : QStringLiteral("no PR"))
                  .toHtmlEscaped();
    const QString issueValue =
        session->issueNumber > 0
            ? issueLinkHtml(session->issueNumber, session->issueTitle)
            : QStringLiteral("<span style='color:#8b949e'>none yet</span>");
    QStringList headers;
    QStringList lines;
    headers << QStringLiteral("Agent");
    lines << agentProviderName(session->provider).toHtmlEscaped();
    if (session->issueNumber <= 0 && !session->titledBy.isEmpty()) {
        headers << QStringLiteral("Title by");
        lines << session->titledBy.toHtmlEscaped();
    }
    headers << QStringLiteral("Priority");
    lines << QStringLiteral("%1 <span style='color:#8b949e'>(1 highest, 99 "
                            "lowest)</span>")
                 .arg(qBound(1, session->priority, 99));
    if (!session->orgTaskId.isEmpty()) {
        headers << QStringLiteral("Task");
        lines << chipLinkHtml(kTaskLinkScheme + session->orgTaskId,
                              QStringLiteral("organization task"));
    }
    if (session->hasTeammateAttribution()) {
        headers << (session->teammateIds.isEmpty()
                        ? QStringLiteral("Special agent")
                        : QStringLiteral("Special agents"));
        lines << (session->teammateName.isEmpty() ? session->teammateId
                                                   : session->teammateName)
                     .toHtmlEscaped();
        if (session->teammateIds.isEmpty() &&
            AgentStatus::isTerminal(session->status)) {
            headers << QStringLiteral("Task grade");
            lines << (session->taskGrade > 0
                          ? QStringLiteral("%1 / 5").arg(session->taskGrade)
                          : QStringLiteral(
                                "<span style='color:#8b949e'>not graded</span>"));
        }
    }
    QString displayModel = session->model;
    if (displayModel.isEmpty()) {
        const auto &evts = m_streamEvents.value(sessionId);
        for (const QJsonObject &ev : evts) {
            if (ev.value(QStringLiteral("type")).toString() == QLatin1String("system")
                && ev.value(QStringLiteral("subtype")).toString() == QLatin1String("init")) {
                displayModel = ev.value(QStringLiteral("model")).toString().trimmed();
                break;
            }
        }
    }
    headers << QStringLiteral("Model");
    lines << agentModelLabel(displayModel).toHtmlEscaped();
    // Permission mode this Claude Code session runs under (the composer's
    // mode selector, captured on the last follow-up). Older sessions have no
    // stored mode, so show the current composer default they'd resume with.
    if (session->provider == QLatin1String("claude-code") ||
        agentIsCodexProvider(session->provider)) {
        const QString modeLabel =
            session->mode.isEmpty()
                ? QSettings()
                      .value(kAgentModeSetting,
                             QSettings().value(kClaudeAutoModeSetting, true).toBool()
                                 ? kClaudeAutoModeLabel
                                 : kAgentAskModeLabel)
                      .toString()
                : session->mode;
        headers << QStringLiteral("Mode");
        lines << modeLabel.toHtmlEscaped();
    }
    headers << QStringLiteral("Repo");
    lines << QStringLiteral("%1/%2").arg(session->owner.toHtmlEscaped(),
                                         session->name.toHtmlEscaped());
    if (!session->branchName.isEmpty()) {
        headers << QStringLiteral("Branch");
        lines << session->branchName.toHtmlEscaped();
    }
    headers << QStringLiteral("Worktree");
    lines << (worktreePath.isEmpty()
                  ? QStringLiteral("<span style='color:#8b949e'>none</span>")
                  : worktreePath.toHtmlEscaped());
    headers << QStringLiteral("Status");
    lines << agentStatusText(session->status).toHtmlEscaped();
    headers << QStringLiteral("Issue");
    lines << issueValue;
    headers << QStringLiteral("PR");
    lines << pr;
    const qint64 toks = sessionTokenTotal(*session);
    headers << QStringLiteral("Speed");
    lines << agentSpeedText(*session, toks).toHtmlEscaped();
    headers << QStringLiteral("Diff");
    lines << agentDiffSummaryText(agentDiffStat(*session, QString(), QString()))
                 .toHtmlEscaped();
    // When this session began, on the clock. createdAtMs is the stamp that
    // survives: startedAtMs is restamped by every relaunch, so a resumed
    // conversation would otherwise report its latest turn as its origin.
    const qint64 startedMs =
        session->createdAtMs > 0 ? session->createdAtMs : session->startedAtMs;
    if (startedMs > 0) {
        headers << QStringLiteral("Started");
        lines << agentClockText(startedMs).toHtmlEscaped();
    }
    const qint64 updatedMs = qMax(qMax(session->createdAtMs, session->startedAtMs),
                                  qMax(session->finishedAtMs, session->mergedAtMs));
    headers << QStringLiteral("Updated");
    lines << (updatedMs > 0 ? formatShortRelativeTime(updatedMs / 1000)
                            : QStringLiteral("-"))
                 .toHtmlEscaped();
    // How long this session has been at work. A run in flight counts up here on
    // the ~2 s cadence the status glyph already animates on, so "how long has
    // this been going" is read off the card instead of subtracted from Updated.
    // Earlier turns are banked, and named separately from the one in flight.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 thisRunMs = session->currentRunMs(nowMs);
    const qint64 allRunsMs = session->totalRunMs(nowMs);
    if (allRunsMs > 0) {
        QString runtime =
            formatDuration(thisRunMs > 0 ? thisRunMs : allRunsMs).toHtmlEscaped();
        if (session->runInFlight())
            runtime = QStringLiteral("<span style='color:%1'>%2</span>")
                          .arg(Theme::nextOrHex(
                                   Theme::Role::AccentFg,
                                   currentThemeIsDark() ? QStringLiteral("#3fb950")
                                                        : QStringLiteral("#1a7f37")),
                               runtime);
        // Only when there is an open stretch to distinguish it from: a session
        // requeued by a restart has banked time and nothing running, and would
        // otherwise name the same figure twice.
        if (thisRunMs > 0 && allRunsMs > thisRunMs)
            runtime += QStringLiteral(" &middot; %1 total")
                           .arg(formatDuration(allRunsMs).toHtmlEscaped());
        headers << QStringLiteral("Runtime");
        lines << runtime;
    }
    QStringList stats;
    if (session->numTurns > 0)
        stats << QStringLiteral("%1 turns").arg(session->numTurns);
    stats << agentCostText(session->costUsd).toHtmlEscaped();
    if (toks > 0)
        stats << QStringLiteral("%1 tokens").arg(formatCount(toks));
    headers << QStringLiteral("Stats");
    lines << stats.join(QStringLiteral(" &middot; "));
    const qint64 agentPid = agentSessionProcessId(sessionId);
    scheduleAgentProcessMetaRefresh(agentPid, forceProcessRefresh);
    const bool haveProcessSnapshot = m_agentProcessMetaPid == agentPid;
    if (haveProcessSnapshot && m_agentProcessMetaCompilerCount > 0) {
        headers << QStringLiteral("cc1plus");
        lines << QStringLiteral("%1 &middot; %2")
                     .arg(m_agentProcessMetaCompilerCount)
                     .arg(SystemStats::formatBytes(
                              m_agentProcessMetaCompilerResidentBytes)
                              .toHtmlEscaped());
    }
    headers << QStringLiteral("Session");
    lines << QStringLiteral("#%1 &middot; %2")
                 .arg(QString::number(session->id),
                      conversationId.isEmpty()
                          ? QStringLiteral(
                                "<span style='color:#8b949e'>no CLI conversation "
                                "yet</span>")
                          : conversationId.toHtmlEscaped());
    if (agentPid > 0) {
        headers << QStringLiteral("Process");
        lines << QStringLiteral("Agent CLI (PID %1)").arg(agentPid);
    }
    if (haveProcessSnapshot && !m_agentProcessMetaSubprocessText.isEmpty()) {
        headers << QStringLiteral("Subprocesses");
        lines << m_agentProcessMetaSubprocessText;
    }
    if (session->finishingBackgroundWork()) {
        headers << QStringLiteral("Background");
        lines << QStringLiteral(
                     "<span style='color:#d29922'>turn over &middot; %1 task%2 "
                     "still %3</span> &middot; %4")
                     .arg(session->backgroundProcesses)
                     .arg(session->backgroundProcesses == 1
                              ? QString()
                              : QStringLiteral("s"),
                          session->backgroundProcessesBusy
                              ? QStringLiteral("working")
                              : QStringLiteral("open"),
                          session->backgroundProcessSummary.toHtmlEscaped());
    }
    QString meta = agentDetailTableHtml(headers, lines);
    if (!mergedMeta.isEmpty())
        meta += QStringLiteral("<br>") + mergedMeta;
    applyAgentMetaHtml(meta);
}

// The detail header re-renders every ~2 s from the spin timer and on every
// token/cost/status update of the selected session, and most of those passes
// produce byte-identical HTML. Setting it anyway cost a rich-text relayout
// plus fitAgentMetaWidth()'s second QTextDocument layout each time. The
// selected session's Runtime row does tick, so its passes pay that — one small
// table, on the same cadence as the /proc scan beside it — while every other
// session still short-circuits here.
void MainWindow::applyAgentMetaHtml(const QString &html)
{
    if (!m_agentMeta)
        return;
    if (m_agentMeta->text() == html)
        return;
    m_agentMeta->setText(html);
    fitAgentMetaWidth(m_agentMeta);
    if (m_agentMetaPopup && m_agentMetaPopup->isVisible())
        m_agentMetaPopup->adjustSize();
}

void MainWindow::showAgentSession(int sessionId)
{
    m_selectedAgentSessionId = sessionId;
    refreshAgentSubSessions();
    if (m_agentTable && m_agentTable->isVisible() && isActiveWindow()) {
        clearTeammateDoneUnseen(sessionId);
        markAgentSessionRead(sessionId);
    }
    if (m_agentTranscriptRenderSession >= 0 &&
        m_agentTranscriptRenderSession != sessionId) {
        ++m_agentTranscriptRenderGeneration;
        m_agentTranscriptRenderSession = -1;
        if (m_agentTranscript)
            m_agentTranscript->setBulkPopulate(false);
    }
    if (m_externalTranscriptRenderSession != -1 &&
        m_externalTranscriptRenderSession != sessionId) {
        ++m_externalTranscriptRenderGeneration;
        m_externalTranscriptRenderSession = -1;
        m_externalTranscriptRenderEvents.clear();
        if (m_agentTranscript)
            m_agentTranscript->setBulkPopulate(false);
    }
    updateQuickAddTargetAgentLabel();
    scheduleNavRecord();
    AgentSession *liveSession = findAgentSession(sessionId);
    if (!liveSession) {
        refreshAgentTaskGradeControl(nullptr);
        setAgentTitleText(m_agentTitle, QStringLiteral("Select a session"));
        if (m_agentStatusPill) {
            m_agentStatusPill->setText(QString());
            m_agentStatusPill->setIcon(QIcon());
            m_agentStatusPill->hide();
        }
        if (m_agentMeta)
            m_agentMeta->clear();
        if (m_agentMetaPopup)
            m_agentMetaPopup->hide();
        if (m_navTokenUsage)
            static_cast<TokenUsageMiniChart *>(m_navTokenUsage)->setStats(QString());
        if (m_agentNetPanel)
            m_agentNetPanel->clear();
        renderAgentPromptImages(-1);
        if (m_agentViewPrButton)
            m_agentViewPrButton->hide();
        if (m_agentViewIssueButton)
            m_agentViewIssueButton->hide();
        if (m_agentViewTaskButton)
            m_agentViewTaskButton->hide();
        if (m_agentCreatePrButton)
            m_agentCreatePrButton->hide();
        if (m_agentCreateIssueButton)
            m_agentCreateIssueButton->hide();
        if (m_agentMoveToCloudButton)
            m_agentMoveToCloudButton->hide();
        if (m_agentContinueHereButton)
            m_agentContinueHereButton->hide();
        if (m_agentSignInRunnerButton)
            m_agentSignInRunnerButton->hide();
        if (m_agentRetryInCloudButton)
            m_agentRetryInCloudButton->hide();
        if (m_agentCloudMergeButton)
            m_agentCloudMergeButton->hide();
        if (m_agentCloudCleanupButton)
            m_agentCloudCleanupButton->hide();
        if (m_agentCloudUpdateButton)
            m_agentCloudUpdateButton->hide();
        if (m_agentCloudActionsButton)
            m_agentCloudActionsButton->hide();
        if (m_agentBranchButton)
            m_agentBranchButton->hide();
        if (m_agentWorktreeButton)
            m_agentWorktreeButton->hide();
        refreshAgentSyncControls(-1);
        if (m_agentLog)
            m_agentLog->clear();
        m_agentLogSession = -1; // log emptied out-of-band; force the next set to render
        m_agentDetailTabSession = -1; // next opened session re-starts on the Agent tab
        updateAgentActionState();
        updateQuickAddTargetAgentLabel();
        return;
    }
    const AgentSession sessionSnapshot = *liveSession;
    const AgentSession *session = &sessionSnapshot;
    refreshAgentTaskGradeControl(session);
    syncQuickAddControlsToAgentSession(sessionSnapshot);

    ensureStreamEventsLoadedAsync(sessionId);

    if (m_agentDetail)
        m_agentDetail->show();
    if (m_agentWorkspaceSplit) {
        QList<int> sizes = m_agentWorkspaceSplit->sizes();
        if (sizes.size() >= 2 && sizes.at(1) == 0) {
            const int shared = sizes.at(0) + sizes.at(1);
            const int total = shared > 0 ? shared : m_agentWorkspaceSplit->width();
            sizes[0] = total / 2;
            sizes[1] = total - sizes.at(0);
            m_agentWorkspaceSplit->setSizes(sizes);
        }
    }
    if (m_agentTitle) {
        const QString sessionLabel =
            QStringLiteral("Agent #%1").arg(sessionId);
        if (session->cloud) {
            // A run nothing has started yet is not running anywhere: say which
            // node it is waiting for, not that it is on it.
            const bool waiting = session->status == AgentStatus::Queued;
            const QString node = session->cloudNode.isEmpty()
                                     ? QStringLiteral("an unclaimed node")
                                     : session->cloudNode;
            setAgentTitleText(
                m_agentTitle,
                (waiting ? QStringLiteral("Cloud agent · %1 queued for %2 · %3")
                         : QStringLiteral("Cloud agent · %1 on %2 · %3"))
                    .arg(agentProviderName(session->provider), node,
                         session->issueTitle));
        } else if (isExternalSession(sessionId)) {
            const QString label = !session->issueTitle.isEmpty()
                                      ? session->issueTitle
                                      : (session->branchName.isEmpty()
                                             ? QStringLiteral("session")
                                             : session->branchName);
            setAgentTitleText(
                m_agentTitle,
                QStringLiteral("%1 · External Claude Code · %2")
                    .arg(sessionLabel, label));
        } else {
            QString adHocTitle = session->issueTitle;
            if (session->issueNumber <= 0 && !session->prompt.trimmed().isEmpty())
                adHocTitle =
                    session->prompt.section(QLatin1Char('\n'), 0, 0).simplified();
            setAgentTitleText(
                m_agentTitle,
                session->issueNumber > 0
                    ? QStringLiteral("%1 · #%2 · %3")
                          .arg(sessionLabel)
                          .arg(session->issueNumber)
                          .arg(session->issueTitle.isEmpty()
                                   ? agentProviderName(session->provider)
                                   : session->issueTitle)
                    : QStringLiteral("%1 · %2 · %3")
                          .arg(sessionLabel)
                          .arg(agentProviderName(session->provider))
                          .arg(adHocTitle.isEmpty()
                                   ? QStringLiteral("pull #%1").arg(session->prNumber)
                                   : adHocTitle));
        }
    }
    renderAgentPromptImages(sessionId);
    refreshAgentDetailMeta(sessionId, /*forceProcessRefresh=*/true);
    setAgentUsageLabel(*session);
    refreshAgentStatusPill(sessionId);

    if (m_agentViewPrButton) {
        m_agentViewPrButton->setVisible(session->prNumber > 0);
        if (session->prNumber > 0) {
            setAgentActionCaption(m_agentViewPrButton,
                                  QStringLiteral("git-pull-request"),
                                  QStringLiteral("PR #%1").arg(session->prNumber));
            m_agentViewPrButton->setToolTip(
                QStringLiteral("Open pull request #%1")
                    .arg(session->prNumber));
        }
    }
    syncAgentViewTaskButton(sessionId);
    // An agent started from an issue keeps a link back to it at the top of its
    // detail: the caption names the issue it was assigned, so where the run came
    // from is readable without opening the meta popup.
    if (m_agentViewIssueButton) {
        m_agentViewIssueButton->setVisible(session->issueNumber > 0);
        if (session->issueNumber > 0) {
            setAgentActionCaption(
                m_agentViewIssueButton, QStringLiteral("issue-opened"),
                QStringLiteral("Issue #%1").arg(session->issueNumber));
            const QString issueTitle = session->issueTitle.trimmed();
            m_agentViewIssueButton->setToolTip(
                issueTitle.isEmpty()
                    ? QStringLiteral("Open issue #%1, the issue this agent was "
                                     "started from")
                          .arg(session->issueNumber)
                    : QStringLiteral("Open issue #%1 · %2, the issue this "
                                     "agent was started from")
                          .arg(session->issueNumber)
                          .arg(issueTitle));
        }
    }
    if (m_agentCreatePrButton)
        m_agentCreatePrButton->setVisible(
            session->cloud
                ? m_cloudAgentSessions.value(sessionId).canOpenPull
                : session->prNumber <= 0 && !session->branchName.isEmpty() &&
                      !isExternalSession(sessionId));
    // Offered on every local run so the header also answers "why can't this
    // one go": updateAgentActionState() disables it with the refusal as its
    // tooltip. Whether the worktree is still on disk is settled by
    // agentCloudHandoff() on the click, because resolving it shells git.
    if (m_agentMoveToCloudButton)
        m_agentMoveToCloudButton->setVisible(!isExternalSession(sessionId) &&
                                             !session->associationOnly);
    // Offered on every cloud row for the same reason: a run that cannot be
    // taken over says why in updateAgentActionState()'s tooltip.
    if (m_agentContinueHereButton)
        m_agentContinueHereButton->setVisible(session->cloud);
    {
        const QList<forkmesh::agents::CloudAgentAction> cloudActions =
            session->cloud ? forkmesh::agents::cloudAgentActions(
                                 m_cloudAgentSessions.value(sessionId))
                           : QList<forkmesh::agents::CloudAgentAction>();
        if (m_agentSignInRunnerButton)
            m_agentSignInRunnerButton->setVisible(cloudActions.contains(
                forkmesh::agents::CloudAgentAction::SignInRunner));
        if (m_agentRetryInCloudButton)
            m_agentRetryInCloudButton->setVisible(cloudActions.contains(
                forkmesh::agents::CloudAgentAction::RetryInCloud));
        const forkmesh::agents::CloudAgentSession cloud =
            session->cloud ? m_cloudAgentSessions.value(sessionId)
                           : forkmesh::agents::CloudAgentSession();
        if (m_agentCloudMergeButton) {
            m_agentCloudMergeButton->setVisible(session->cloud && cloud.canMerge);
            setAgentActionCaption(
                m_agentCloudMergeButton, QStringLiteral("check-circle"),
                cloud.mergeState == QLatin1String("merging")
                    ? QStringLiteral("Finish merge")
                    : QStringLiteral("Merge & clean up"));
        }
        if (m_agentCloudCleanupButton)
            m_agentCloudCleanupButton->setVisible(session->cloud &&
                                                  cloud.canCleanup);
        if (m_agentCloudUpdateButton) {
            m_agentCloudUpdateButton->setVisible(
                session->cloud && cloud.canMerge &&
                cloud.mergeState != QLatin1String("merging"));
            setAgentActionCaption(
                m_agentCloudUpdateButton, QStringLiteral("download"),
                cloud.baseBranch.isEmpty()
                    ? QStringLiteral("Pull main")
                    : QStringLiteral("Pull %1").arg(cloud.baseBranch));
        }
        if (m_agentCloudActionsButton)
            m_agentCloudActionsButton->setVisible(session->cloud && cloud.merged);
    }
    if (m_agentCreateIssueButton) {
        const int repoIdx = repoIndexFor(session->owner, session->name);
        const bool canTrack =
            session->issueNumber == 0 && !isExternalSession(sessionId) &&
            repoIdx >= 0 &&
            IssueStore(m_repositories.at(repoIdx).localPath,
                       m_repositories.at(repoIdx).mirrorPath, &m_profileIdentity,
                       m_userName)
                .canWrite();
        m_agentCreateIssueButton->setVisible(canTrack);
    }

    const bool external = isExternalSession(sessionId);
    const bool eventsLoading = m_streamEventsLoading.contains(sessionId);
    const bool transcript =
        external || eventsLoading || isStreamTranscriptSession(sessionId);
    if (m_agentStore) {
        const AgentSession snapshot = *session;
        AgentStore *store = m_agentStore;
        const QString status = session->status;
        const bool wantLogSurface = !transcript;
        runOffThread<AgentLogScan>(
            [store, snapshot] { return scanAgentLog(store->readLog(snapshot)); },
            [this, sessionId, status, wantLogSurface](AgentLogScan scan) {
                if (sessionId != m_selectedAgentSessionId)
                    return; // clicked away while the read ran
                applyAgentNetworkPanel(scan, status);
                if (wantLogSurface)
                    setAgentLogText(sessionId, scan.log);
            });
    }
    // Feed the transcript's "session started" divider the run context the CLI
    // itself never reports — branch, permission mode, reasoning strength
    // and the provider login it runs as. Set before any
    // rebuild below so the divider renders with it; external sessions keep
    // whatever their own init event says.
    if (m_agentTranscript) {
        QString ctxMode, ctxStrength, ctxAccount;
        // Only the CLI providers run under a permission mode; the API ones have
        // no such notion (the same rule the detail meta table uses).
        if (!external && (session->provider == QLatin1String("claude-code") ||
                          agentIsCodexProvider(session->provider))) {
            ctxMode = session->mode;
            if (ctxMode.isEmpty()) // pre-adhoc-#38 sessions: the resume default
                ctxMode = QSettings()
                              .value(kAgentModeSetting,
                                     QSettings().value(kClaudeAutoModeSetting, true).toBool()
                                         ? kClaudeAutoModeLabel
                                         : kAgentAskModeLabel)
                              .toString();
            // Accounts are a CLI notion too: each is its own config root, and
            // the login that is signed in there is what the run spends. A live
            // run knows the account it launched under; a stored one is read off
            // the conversation its transcript carries (showAgentSession runs
            // again once those events finish loading, so an unloaded transcript
            // is not stuck with the fallback). Nothing to read means the
            // session resumes as the selected account — the same rule
            // AgentResumeIdentity.h applies to an unstamped conversation.
            QString accountId = m_streamAccountId.value(sessionId);
            if (accountId.isEmpty())
                accountId = forkmesh::agents::resumeConversationAccountId(
                    m_streamEvents.value(sessionId),
                    agentIsCodexProvider(session->provider));
            ctxAccount = agentAccountLabelFor(session->provider, accountId);
            if (ctxAccount.isEmpty())
                ctxAccount = activeAgentAccount(session->provider).label.trimmed();
        }
        if (!external)
            ctxStrength = session->strength.isEmpty() ? composerAgentStrength()
                                                      : session->strength;
        m_agentTranscript->setSessionContext(session->branchName, ctxMode,
                                             ctxStrength, ctxAccount);
        m_agentTranscript->setCodexStyle(agentIsCodexProvider(session->provider));
        // Who is speaking, in portrait: the model's own face from the same table
        // the composer's model chip reads, and the signed-in user's picture. Both
        // are the host's to know, and both are set before any rebuild below so
        // the bubbles are already wearing them when the rows are built.
        constexpr int kSpeakerAvatarPx = 26;
        const QPixmap modelFace = roundedAvatar(
            agentModelIcon(session->provider, session->model)
                .pixmap(QSize(64, 64)),
            kSpeakerAvatarPx);
        // A session with no model pinned runs the provider's default, which only
        // the provider can name — so the bubble is captioned with the CLI.
        QString modelName = forkmesh::ui::agentModelLabel(session->model);
        if (modelName.isEmpty())
            modelName = cliProviderLabel(session->provider);
        m_agentTranscript->setSpeakerAvatars(
            modelFace, modelName,
            roundedAvatar(effectiveUserAvatar(), kSpeakerAvatarPx),
            topBarUserName());
    }
    if (session->cloud) {
        renderCloudAgentTranscript(sessionId);
    } else if (external) {
        const qint64 read = m_externalReadOffset.value(sessionId, -1);
        const QString extPath = m_externalSurfaced.value(sessionId).path;
        if (m_renderedExternalSession != sessionId || read < 0 ||
            QFileInfo(extPath).size() > read)
            renderExternalTranscript(sessionId, /*full=*/true);
    } else if (eventsLoading) {
        if (m_renderedTranscriptSession != sessionId && m_agentTranscript) {
            m_agentTranscript->clear();
            m_renderedTranscriptSession = -1;
            m_renderedExternalSession = -1; // the view no longer shows one
        }
    } else if (isStreamTranscriptSession(sessionId)) {
        if (m_agentTranscriptRenderSession != sessionId &&
            (m_renderedTranscriptSession != sessionId ||
             m_renderedTranscriptCount != m_streamEvents.value(sessionId).size())) {
            // Coming back to a session that only gained events costs the new
            // ones, not a replay of the whole transcript.
            if (!renderTranscriptTail(sessionId))
                renderTranscriptForSession(sessionId);
        }
        refreshAgentFilesPanel(sessionId);
        if (m_agentOutputStack && m_agentOutputStack->currentWidget() == m_agentLog)
            setAgentLogText(sessionId, m_streamRaw.value(sessionId));
    } else if (m_agentLog && m_agentLogSession != sessionId) {
        m_agentLog->setPlainText(QString());
        m_agentLogSession = -1; // set out-of-band; the async set re-renders
    }
    if (m_agentOutputModeButton)
        m_agentOutputModeButton->setVisible(transcript);
    if (m_agentListenButton) {
        m_agentListenButton->setVisible(transcript);
        refreshAgentListenButton();
    }
    const bool codeOk = transcript && !external;
    if (m_agentCodeButton)
        m_agentCodeButton->setVisible(codeOk);
    if (m_agentDetailTabs && m_agentFilesTabIndex >= 0) {
        const bool filesOk = transcript && !external;
        m_agentDetailTabs->setTabVisible(m_agentFilesTabIndex, filesOk);
        if (m_agentDetailTabSession != sessionId) {
            m_agentDetailTabSession = sessionId;
            m_agentDetailTabs->setCurrentIndex(0);
        } else if (!filesOk &&
                   m_agentDetailTabs->currentIndex() == m_agentFilesTabIndex) {
            m_agentDetailTabs->setCurrentIndex(0);
        }
    }
    updateAgentFilesTabState(sessionId);
    if (m_agentOutputStack) {
        const bool termLive = sessionId == m_terminalSessionId && m_agentTerminal &&
                              m_agentTerminal->isRunning();
        if (transcript) {
            m_agentOutputStack->setCurrentWidget(
                m_agentRawOutputMode ? static_cast<QWidget *>(m_agentLog)
                                     : static_cast<QWidget *>(m_agentTranscript));
        } else if (termLive) {
            m_agentOutputStack->setCurrentWidget(m_agentTerminal);
        } else {
            m_agentOutputStack->setCurrentWidget(m_agentLog);
        }
    }
    if (m_agentCodePage) {
        const bool codeShown = codeOk && m_agentCodeMode;
        m_agentCodePage->setVisible(codeShown);
        if (codeShown)
            refreshAgentFilesPanel(sessionId);
    }
    updateAgentActionState();
}

void MainWindow::refreshAgentTranscriptTheme(bool dark)
{
    for (ScrollJumpButtons *jump : findChildren<ScrollJumpButtons *>())
        jump->setTheme(dark);
    refreshSessionsPageTheme(dark);

    if (!m_agentTranscript)
        return;
    m_agentTranscript->setTheme(dark);

    const int sessionId = m_selectedAgentSessionId;
    if (sessionId < 0 || m_streamEventsLoading.contains(sessionId))
        return;
    if (isCloudSession(sessionId)) {
        m_renderedCloudTurns = -1; // same turns, new colours: draw them again
        renderCloudAgentTranscript(sessionId);
        return;
    }
    if (isExternalSession(sessionId)) {
        if (m_externalSurfaced.contains(sessionId))
            renderExternalTranscript(sessionId, /*full=*/true);
        return;
    }
    if (isStreamTranscriptSession(sessionId))
        renderTranscriptForSession(sessionId);
}

void MainWindow::syncQuickAddControlsToAgentSession(const AgentSession &session)
{
    if (m_quickAddAgentProvider && !session.provider.isEmpty()) {
        const int providerIndex = m_quickAddAgentProvider->findData(session.provider);
        if (providerIndex >= 0)
            m_quickAddAgentProvider->setCurrentIndex(providerIndex);
    }
    if (m_quickAddClaudeModel && !session.model.isEmpty())
        selectModelComboValue(m_quickAddClaudeModel, session.model);
    if (m_quickAddModeSelector && !session.mode.isEmpty()) {
        const int modeIndex = m_quickAddModeSelector->findText(session.mode);
        if (modeIndex >= 0)
            m_quickAddModeSelector->setCurrentIndex(modeIndex);
    }
    if (const QString strength = session.strength.trimmed().toLower();
        !strength.isEmpty()) {
        // Only write (and rebuild the speed combo) when the value actually
        // moves: this runs on every session open and each reload.
        QSettings settings;
        if (settings.value(kClaudeEffortSetting).toString().trimmed().toLower() !=
            strength) {
            settings.setValue(kClaudeEffortSetting, strength);
            refreshQuickAddSpeedSelector();
        }
    }
    // The combined selector's catalog changes when models/outcomes change, not
    // when the user clicks between sessions. Select the already-built row in
    // place; rebuilding it here scanned every retired run and re-created every
    // icon-rich option on each agent click.
    if (m_quickAddAgentModelSelector) {
        const QString provider = session.provider;
        const QString model = session.model;
        int row = -1;
        for (int i = 0; i < m_quickAddAgentModelSelector->count(); ++i) {
            if (m_quickAddAgentModelSelector->itemData(i).toString() != provider)
                continue;
            const QString rowModel = m_quickAddAgentModelSelector
                                         ->itemData(i, Qt::UserRole + 1)
                                         .toString();
            if (model.isEmpty() || rowModel == model) {
                row = i;
                break;
            }
        }
        if (row >= 0) {
            const QSignalBlocker blocker(m_quickAddAgentModelSelector);
            m_quickAddAgentModelSelector->setCurrentIndex(row);
        } else {
            refreshQuickAddAgentModelSelector();
        }
    }
}

void MainWindow::setAgentLogText(int sessionId, const QString &text)
{
    if (!m_agentLog)
        return;
    const QString safeText =
        redactProviderCredentials(text, localProviderCredentialValues());
    if (m_agentLogSession == sessionId && m_agentLogText == safeText)
        return;
    // The transcript is the default surface. Retain Raw's complete text while
    // it is hidden, but do not make QPlainTextEdit lay out hundreds of KB on an
    // agent-selection click merely to prepare an off-screen alternative view.
    if (!m_agentLog->isVisible()) {
        m_agentLogSession = sessionId;
        m_agentLogText = safeText;
        return;
    }
    if (m_agentLogSession == sessionId && !m_agentLogText.isEmpty() &&
        safeText.startsWith(m_agentLogText)) {
        QTextCursor cursor(m_agentLog->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(safeText.mid(m_agentLogText.size()));
        m_agentLogText = safeText;
        m_agentLog->moveCursor(QTextCursor::End);
        return;
    }
    m_agentLogSession = sessionId;
    m_agentLogText = safeText;
    m_agentLog->setPlainText(safeText);
    m_agentLog->moveCursor(QTextCursor::End); // raw log opens at the tail
}

MainWindow::AgentLogScan MainWindow::scanAgentLog(QString log)
{
    AgentLogScan scan;
    static const QRegularExpression tokenRe(
        QStringLiteral("in=(\\d+)\\s+out=(\\d+)"));
    const auto lines = QStringView(log).split(QLatin1Char('\n'));
    for (const auto &lineView : lines) {
        const QString line = lineView.toString();
        if (!line.contains(QLatin1String("[net]")))
            continue;
        if (line.contains(QLatin1String("request #")))
            ++scan.requests;
        else if (line.contains(QLatin1String("response #"))) {
            ++scan.responses;
            const auto m = tokenRe.match(line);
            if (m.hasMatch()) {
                scan.inTokens += m.captured(1).toLongLong();
                scan.outTokens += m.captured(2).toLongLong();
            }
        } else if (line.contains(QLatin1String("error #")))
            ++scan.errors;
    }
    scan.log = std::move(log);
    return scan;
}

void MainWindow::applyAgentNetworkPanel(const AgentLogScan &scan, const QString &status)
{
    if (!m_agentNetPanel)
        return;
    const int requests = scan.requests, responses = scan.responses,
              errors = scan.errors;
    const long long inTokens = scan.inTokens, outTokens = scan.outTokens;

    if (requests == 0 && responses == 0) {
        m_agentNetPanel->hide();
        return;
    }
    m_agentNetPanel->show();

    const bool live = status == AgentStatus::Running;
    const QString dot =
        live ? Theme::nextOrHex(Theme::Role::AccentFg, QStringLiteral("#3fb950"))
             : Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#8b949e"));
    auto fmtTokens = [](long long n) {
        if (n >= 1000)
            return QStringLiteral("%1k").arg(n / 1000.0, 0, 'f', 1);
        return QString::number(n);
    };
    const long long maxTok = qMax<long long>(1, qMax(inTokens, outTokens));
    auto bar = [&](long long n, const QString &color) {
        const int width = int((double(n) / double(maxTok)) * 22.0 + 0.5);
        return QStringLiteral("<span style='color:%1'>%2</span>")
            .arg(color, QString(qMax(n > 0 ? 1 : 0, width),
                                QChar(0x2587))); // ▇
    };
    const QString errText =
        errors > 0 ? QString::fromUtf8(
                         " \xC2\xB7 <span style='color:#f85149'>%1 error%2</span>")
                         .arg(errors)
                         .arg(errors == 1 ? "" : "s")
                   : QString();
    m_agentNetPanel->setText(
        QString::fromUtf8(
            "<table cellspacing='0' cellpadding='0' style='font-size:12px'>"
            "<tr><td style='padding-bottom:3px'>"
            "<span style='color:%1'>\xE2\x97\x8F</span> "
            "<b style='color:#8b949e'>\xF0\x9F\x8C\x90 API traffic</b> "
            "<span style='color:#8b949e'>\xC2\xB7 %2 request%3 \xC2\xB7 %4 response%5%6</span>"
            "</td></tr>"
            "<tr><td><span style='color:#8b949e'>\xE2\x86\x91 in&nbsp;</span>"
            "%7 <span style='color:#8b949e'>&nbsp;%8</span></td></tr>"
            "<tr><td><span style='color:#8b949e'>\xE2\x86\x93 out</span>&nbsp;"
            "%9 <span style='color:#8b949e'>&nbsp;%10</span></td></tr>"
            "</table>")
            .arg(dot)
            .arg(requests)
            .arg(requests == 1 ? "" : "s")
            .arg(responses)
            .arg(responses == 1 ? "" : "s")
            .arg(errText)
            .arg(bar(inTokens, "#58a6ff"), fmtTokens(inTokens))
            .arg(bar(outTokens, "#d2a8ff"), fmtTokens(outTokens)));
}

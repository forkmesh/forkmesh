
#include "MainWindow.h"
#include "MessageBoxClicks.h"
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

struct AgentDiffBatch {
    QString base;
    QHash<int, AgentDiffStat> stats;
    QHash<int, QString> signatures;
    QSet<int> liveIds;
};

struct AgentImageBatch {
    QHash<int, QStringList> images;
    QHash<int, QString> stamps;
};

void summarizeAgentPatch(const QString &patch, AgentDiffStat *stat)
{
    if (!stat)
        return;
    int files = patch.startsWith(QLatin1String("diff --git ")) ? 1 : 0;
    files += patch.count(QStringLiteral("\ndiff --git "));
    int added = 0;
    int removed = 0;
    for (const QStringView line : QStringView(patch).split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("+++")) ||
            line.startsWith(QLatin1String("---")))
            continue;
        if (line.startsWith(QLatin1Char('+')))
            ++added;
        else if (line.startsWith(QLatin1Char('-')))
            ++removed;
    }
    stat->files = files;
    stat->added = added;
    stat->removed = removed;
}

void summarizeAgentNumstat(const QByteArray &numstat, AgentDiffStat *stat)
{
    if (!stat)
        return;
    int files = 0;
    int added = 0;
    int removed = 0;
    for (const QByteArray &line : numstat.split('\n')) {
        const int firstTab = line.indexOf('\t');
        const int secondTab = firstTab < 0 ? -1 : line.indexOf('\t', firstTab + 1);
        if (firstTab < 0 || secondTab < 0)
            continue;
        ++files;
        added += line.left(firstTab).toInt();
        removed += line.mid(firstTab + 1, secondTab - firstTab - 1).toInt();
    }
    stat->files = files;
    stat->added = added;
    stat->removed = removed;
}

QString backgroundDefaultBranch(const QString &gitDir, QString configured,
                                QString checkedOut)
{
    configured = configured.trimmed();
    if (!configured.isEmpty())
        return configured;
    QByteArray refsOut;
    QStringList branches;
    if (runGitCapture(gitDir,
                      {QStringLiteral("for-each-ref"),
                       QStringLiteral("--format=%(refname:short)"),
                       QStringLiteral("refs/heads/")},
                      &refsOut, nullptr)) {
        for (const QString &line : QString::fromUtf8(refsOut).split('\n')) {
            const QString branch = line.trimmed();
            if (!branch.isEmpty())
                branches.append(branch);
        }
    }
    if (branches.contains(QStringLiteral("main")))
        return QStringLiteral("main");
    if (branches.contains(QStringLiteral("master")))
        return QStringLiteral("master");
    if (!checkedOut.isEmpty() && branches.contains(checkedOut))
        return checkedOut;
    return branches.isEmpty() ? QString() : branches.first();
}

AgentDiffStat readAgentDiffStat(const AgentStore &store,
                                const AgentSession &session,
                                const QString &gitDir, const QString &base,
                                const QString &worktree,
                                bool probeConflict, bool idleSession)
{
    AgentDiffStat stat;
    // Pull metadata branches are internal records, not agent-authored code.
    // Comparing them to main produces a bogus 99+ file badge and can trigger
    // an equally bogus behind/conflict state.
    if (session.branchName == QLatin1String("forkmesh/pulls") ||
        session.branchName.startsWith(QLatin1String("forkmesh/pull/")))
        return stat;
    const QString patch = store.readPatch(session);
    if (!patch.isEmpty())
        summarizeAgentPatch(patch, &stat);
    // Paths the agent has touched but not committed yet. A running agent
    // edits its checkout for many minutes before its first commit, so the
    // row's file count has to see these or it reads 0 the whole time.
    QSet<QString> dirtyTracked;
    QStringList dirtyUntracked;
    if (!worktree.isEmpty() && QDir(worktree).exists()) {
        stat.worktree = worktree;
        QByteArray dirtyOut;
        if (runGitCapture(worktree,
                          {QStringLiteral("status"), QStringLiteral("--porcelain"),
                           QStringLiteral("-z"),
                           QStringLiteral("--untracked-files=all")},
                          &dirtyOut, nullptr)) {
            const QList<QByteArray> entries = dirtyOut.split('\0');
            int dirty = 0;
            for (int i = 0; i < entries.size(); ++i) {
                const QByteArray &entry = entries.at(i);
                if (entry.size() < 4)
                    continue;
                ++dirty;
                const QString path = QString::fromUtf8(entry.mid(3));
                if (entry.startsWith("??"))
                    dirtyUntracked.append(path);
                else
                    dirtyTracked.insert(path);
                // A rename or copy carries its old path as the next entry.
                if (entry.at(0) == 'R' || entry.at(0) == 'C')
                    ++i;
            }
            stat.dirty = dirty;
        }
    }
    if (!gitDir.isEmpty() && !base.isEmpty() && !session.branchName.isEmpty() &&
        session.branchName != base &&
        runGitCapture(gitDir,
                      {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                       QStringLiteral("--quiet"),
                       QStringLiteral("refs/heads/%1").arg(session.branchName)},
                      nullptr, nullptr)) {
        QByteArray liveDiff;
        QString liveError;
        QSet<QString> ownedPaths;
        const bool ownedPathsOk =
            readAgentOwnedPaths(gitDir, base, session.branchName, &ownedPaths);
        if (ownedPathsOk) {
            stat.files = 0;
            stat.added = 0;
            stat.removed = 0;
            if (!ownedPaths.isEmpty()) {
                QStringList diffArgs{QStringLiteral("diff"),
                                     QStringLiteral("--numstat"), base,
                                     session.branchName, QStringLiteral("--")};
                diffArgs.append(literalPathspecs(ownedPaths));
                if (runGitCapture(gitDir, diffArgs, &liveDiff, &liveError))
                    summarizeAgentNumstat(liveDiff, &stat);
            }
        } else if (runGitCapture(
                       gitDir,
                       {QStringLiteral("diff"), QStringLiteral("--numstat"),
                        base + QStringLiteral("...") + session.branchName},
                       &liveDiff, &liveError)) {
            summarizeAgentNumstat(liveDiff, &stat);
        }
        if (!dirtyTracked.isEmpty() || !dirtyUntracked.isEmpty()) {
            // Measure the checkout itself rather than the branch tip: the
            // same comparison as above, with the working tree on the right.
            AgentDiffStat live;
            live.files = 0;
            live.added = 0;
            live.removed = 0;
            QByteArray worktreeDiff;
            if (ownedPathsOk) {
                const QSet<QString> paths = ownedPaths + dirtyTracked;
                if (!paths.isEmpty()) {
                    QStringList args{QStringLiteral("diff"),
                                     QStringLiteral("--numstat"), base,
                                     QStringLiteral("--")};
                    args.append(literalPathspecs(paths));
                    if (runGitCapture(worktree, args, &worktreeDiff, nullptr))
                        summarizeAgentNumstat(worktreeDiff, &live);
                }
            } else if (runGitCapture(worktree,
                                     {QStringLiteral("diff"),
                                      QStringLiteral("--numstat"),
                                      QStringLiteral("--merge-base"), base},
                                     &worktreeDiff, nullptr)) {
                summarizeAgentNumstat(worktreeDiff, &live);
            }
            // New files are invisible to git diff until they are added.
            for (const QString &path : std::as_const(dirtyUntracked)) {
                ++live.files;
                QFile file(QDir(worktree).filePath(path));
                if (file.size() <= 1024 * 1024 &&
                    file.open(QIODevice::ReadOnly)) {
                    const QByteArray bytes = file.readAll();
                    if (!bytes.contains('\0'))
                        live.added += int(bytes.count('\n')) +
                                      (bytes.endsWith('\n') || bytes.isEmpty()
                                           ? 0
                                           : 1);
                }
            }
            stat.files = live.files;
            stat.added = live.added;
            stat.removed = live.removed;
        }
        QByteArray counts;
        if (runGitCapture(gitDir,
                          {QStringLiteral("rev-list"), QStringLiteral("--left-right"),
                           QStringLiteral("--count"),
                           base + QStringLiteral("...") + session.branchName},
                          &counts, nullptr)) {
            // rev-list --left-right --count emits one whitespace-separated
            // pair; building a QRegularExpression per call to split two
            // integers cost more than the split itself.
            static const QRegularExpression countSeparator(
                QStringLiteral("\\s+"));
            const QStringList parts =
                QString::fromUtf8(counts).trimmed().split(countSeparator);
            if (parts.size() >= 2) {
                stat.behind = parts.at(0).toInt();
                stat.ahead = parts.at(1).toInt();
                stat.base = base;
            }
        }
        if ((probeConflict || idleSession) && stat.behind > 0 && !session.merged &&
            !runGitCapture(gitDir,
                           {QStringLiteral("merge-tree"),
                            QStringLiteral("--write-tree"), session.branchName,
                            base},
                           nullptr, nullptr))
            stat.conflicted = true;
    }
    return stat;
}

QHash<QString, QString> backgroundBranchTips(const QString &gitDir)
{
    QHash<QString, QString> result;
    QByteArray out;
    if (gitDir.isEmpty() ||
        !runGitCapture(gitDir,
                       {QStringLiteral("for-each-ref"),
                        QStringLiteral("--format=%(refname:short)\t%(objectname)"),
                        QStringLiteral("refs/heads/")},
                       &out, nullptr))
        return result;
    for (const QByteArray &raw : out.split('\n')) {
        const int tab = raw.indexOf('\t');
        if (tab <= 0)
            continue;
        result.insert(QString::fromUtf8(raw.left(tab)).trimmed(),
                      QString::fromUtf8(raw.mid(tab + 1)).trimmed());
    }
    return result;
}

QImage decodeAgentThumbnail(const QString &path, int side, qreal dpr)
{
    const int target = qMax(1, qRound(side * dpr));
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize src = reader.size(); // header only, no pixel decode
    if (src.isValid() && src.width() > 0 && src.height() > 0) {
        const double factor = double(target) / qMin(src.width(), src.height());
        if (factor < 1.0)
            reader.setScaledSize(QSize(qMax(target, qRound(src.width() * factor)),
                                       qMax(target, qRound(src.height() * factor))));
    }
    QImage img = reader.read();
    if (img.isNull())
        return QImage();
    if (img.width() != target || img.height() != target) {
        img = img.scaled(target, target, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        img = img.copy((img.width() - target) / 2, (img.height() - target) / 2,
                       target, target);
    }
    QImage rounded(target, target, QImage::Format_ARGB32_Premultiplied);
    rounded.fill(Qt::transparent);
    QPainter p(&rounded);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    const qreal radius = 4.0 * dpr;
    clip.addRoundedRect(QRectF(0, 0, target, target), radius, radius);
    p.setClipPath(clip);
    p.drawImage(0, 0, img);
    p.end();
    return rounded;
}

QImage decodeAgentPromptStripImage(const QString &path, const QSize &bounds)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize source = reader.size();
    if (source.isValid() && !source.isEmpty() &&
        (source.width() > bounds.width() || source.height() > bounds.height())) {
        QSize target = source;
        target.scale(bounds, Qt::KeepAspectRatio);
        reader.setScaledSize(target.expandedTo(QSize(1, 1)));
    }
    return reader.read();
}

static bool agentCommitLandedInBase(const QString &dir, const QString &commit,
                                    const QString &base)
{
    if (commit.isEmpty() || base.isEmpty())
        return false;
    return runGitCapture(dir,
                         {"merge-base", "--is-ancestor", commit, base},
                         nullptr, nullptr);
}

} // namespace

void MainWindow::initAgents()
{
    QString root;
#ifdef FORKMESH_WINDOW_TESTS
    // Focused window tests may run with a read-only home. Their fixture supplies
    // a process-private writable root so a lazy Agents-tab reload exercises the
    // real persistence path without touching developer data.
    root = qEnvironmentVariable("FORKMESH_WINDOW_TEST_AGENT_ROOT");
#endif
    if (root.isEmpty()) {
        root = forkmesh::paths::dataPath(QStringLiteral("agents"));
    }
    m_agentStore = new AgentStore(root);
    loadAgentReadState();
    m_agentFolders.load();
    m_agentToolLedger = std::make_unique<AgentToolEfficiencyLedger>(
        QDir(root).filePath(QStringLiteral("tool-efficiency.json")));
    if (QString ledgerError; !m_agentToolLedger->load(&ledgerError))
        qWarning() << "agent tool ledger:" << ledgerError;
    m_agentToolLedger->changed = [this] { scheduleAgentEfficiencyRefresh(); };
    m_agentToolImprovements = std::make_unique<AgentToolImprovementLog>(
        QDir(root).filePath(QStringLiteral("tool-improvements.jsonl")));
    if (QString improvementError;
        !m_agentToolImprovements->load(&improvementError))
        qWarning() << "agent tool improvements:" << improvementError;
    m_agentToolImprovements->changed = [this] { scheduleAgentEfficiencyRefresh(); };
    m_agentRunLog = std::make_unique<AgentRunEfficiencyLog>(
        QDir(root).filePath(QStringLiteral("agent-runs.jsonl")));
    if (QString runError; !m_agentRunLog->load(&runError))
        qWarning() << "agent run log:" << runError;
    m_agentRunLog->changed = [this] { scheduleAgentEfficiencyRefresh(); };
    m_agentToolImprovements->occurred = [this](int sessionId, const QString &, qint64 tokens) {
        if (m_agentRunLog)
            m_agentRunLog->noteSignal(sessionId, tokens);
    };
    m_agentToolLedger->callObserved = [this](const AgentToolCallObservation &call) {
        if (m_agentRunLog)
            m_agentRunLog->observeCall(call);
        if (m_agentToolImprovements)
            m_agentToolImprovements->observe(call);
    };
    {
        const QStringList disabled = disabledAgentToolFamilies();
        m_agentToolLedger->setDisabledFamilies(
            QSet<QString>(disabled.cbegin(), disabled.cend()));
    }
    if (m_agentEfficiencyBoard) {
        m_agentEfficiencyBoard->setImprovementLog(m_agentToolImprovements.get());
        m_agentEfficiencyBoard->setLedger(m_agentToolLedger.get());
    }
    m_agentGuardrailLedger = std::make_unique<AgentGuardrailLedger>(
        QDir(root).filePath(QStringLiteral("guardrails.json")));
    if (QString ledgerError; !m_agentGuardrailLedger->load(&ledgerError))
        qWarning() << "agent guardrail ledger:" << ledgerError;
    m_agentGuardrailLedger->changed = [this] { scheduleAgentGuardrailRefresh(); };
    if (m_agentGuardrailBoard)
        m_agentGuardrailBoard->setLedger(m_agentGuardrailLedger.get());

    m_agentSessions = m_agentStore->loadAllSessions();
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.associationOnly &&
            (session.branchName == QLatin1String("forkmesh/pulls") ||
             session.branchName.startsWith(
                 QLatin1String("forkmesh/pull/")))) {
            m_agentStore->deleteSession(session);
        }
    }
    m_agentSessions = m_agentStore->loadAllSessions();
    for (AgentSession &session : m_agentSessions) {
        // No process survives a ForkMesh restart. Put a prompt that was handed
        // to a not-yet-announced process back in front of the pending prompts,
        // then keep that durable combined value mirrored in memory for the
        // normal queue pump. This is also the recovery point for a restart in
        // the tiny interval between launching the CLI and receiving its init.
        QString pending = session.inFlightSteerMessage.trimmed();
        const QString parked = session.pendingSteerMessage.trimmed();
        if (pending.isEmpty())
            pending = parked;
        else if (!parked.isEmpty() && pending != parked &&
                 !pending.endsWith(QStringLiteral("\n\n") + parked))
            pending += QStringLiteral("\n\n") + parked;
        const bool recoveredPrompt =
            session.pendingSteerMessage != pending ||
            !session.inFlightSteerMessage.isEmpty();
        session.pendingSteerMessage = pending;
        session.inFlightSteerMessage.clear();
        if (!pending.isEmpty())
            m_pendingSteerMessage.insert(session.id, pending);
        const QJsonObject orgJob = session.orgAgentJob;
        const qint64 orgJobId =
            qint64(orgJob.value(QStringLiteral("jobId")).toDouble());
        if (orgJobId > 0 &&
            !orgJob.value(QStringLiteral("leaseId")).toString().isEmpty() &&
            orgJob.value(QStringLiteral("kind")).toString() ==
                QLatin1String("start")) {
            m_orgAgentBindings.insert(session.id, orgJob);
        }
        if (session.merged && (session.status == AgentStatus::Running ||
                               session.status == AgentStatus::Queued)) {
            session.status = AgentStatus::Success;
            session.lastError.clear();
            if (session.finishedAtMs <= 0)
                session.finishedAtMs = QDateTime::currentMSecsSinceEpoch();
            m_agentStore->saveSession(session);
            continue;
        }
        if (session.status == AgentStatus::Running) {
            session.status = AgentStatus::Queued;
            session.lastError.clear();
            session.finishedAtMs = 0;
            m_agentStore->saveSession(session);
            m_agentStore->appendLog(
                session, QStringLiteral("\n==> Resuming after ForkMesh restart."));
        } else if (recoveredPrompt) {
            m_agentStore->saveSession(session);
        }
    }
    m_agentQueue =
        AgentStore::queuedSessionIdsOldestFirst(m_agentSessions);
    for (int id : std::as_const(m_agentQueue))
        m_startupQuietAgentSessions.insert(id);
    m_agentSessions = m_agentStore->loadAllSessions();
    seedSessionTokens();
    refreshAgentDotMatrix(); // the top-bar fleet matrix reflects sessions from the start
    refreshAgentModelSelectors();
    QTimer::singleShot(90 * 1000, this, &MainWindow::backfillClaudeDesktopSessions);
}

void MainWindow::reloadAgents(AgentReloadScan scan)
{
    if (deferUiRefresh(UiRefreshAgents)) {
        // The batch replays this once. Carry the ask across the wait, so a
        // rescan is not lost and a page visit does not become one.
        m_agentRescanDeferred |= scan == AgentReloadScan::Rescan;
        return;
    }
    if (!m_agentStore)
        return;
    const bool rescan =
        scan == AgentReloadScan::Rescan || m_agentRescanDeferred;
    m_agentRescanDeferred = false;
#ifdef FORKMESH_WINDOW_TESTS
    if (rescan)
        ++m_testAgentListRescanCount;
#endif
    m_agentSessions = m_agentStore->loadAllSessions();
    seedSessionTokens(); // keep the live token counter from regressing on reload
    applyAgentBackgroundWork(); // and the "still finishing background work" marker
    injectExternalSessions(); // append any surfaced external (watch-only) sessions
    reloadAgentRecycleBin(); // the deleted-agents strip over the list
    refreshCloudAgentSessions(); // throttled; lands through applyCloudAgentSessions
    if (rescan) {
        refreshAgentMergeState();
        if (!m_agentTableRefreshing)
            m_agentDiffRefreshPending = true;
        m_agentImageScanPending = true;
    }
    refreshAgentTable();
    if (m_selectedAgentSessionId > 0)
        showAgentSession(m_selectedAgentSessionId);
    updateAgentsTabIndicator();
    refreshPullAgentActivity();
    refreshAgentDotMatrix();
    refreshBranchAgentButton();
    for (const AgentSession &session : std::as_const(m_agentSessions))
        syncOrgTaskAgentStatus(session.id);
    notifyPackagesAgentSessionsChanged();
    updateAgentsNavBadge();
    maybeStartQueuedRebuild();
    updateAgentActionState();
    scheduleAgentQueuePump();
}

// deleteStoredAgentSession() has already erased the deleted rows from
// m_agentSessions, so repainting is all that is left. reloadAgents() would
// re-read every session from disk and rescan them all - merge state, diff
// stats, org-task status, the run queue - which touches every other agent in
// the list and the activity field when only the deleted ones changed.
void MainWindow::refreshAgentListAfterRemoval()
{
    reloadAgentRecycleBin(); // the deleted sessions land in the bin strip
    if (!deferUiRefresh(UiRefreshAgentState)) {
        refreshAgentTable();
        if (m_selectedAgentSessionId > 0)
            showAgentSession(m_selectedAgentSessionId);
    }
    updateAgentsTabIndicator();
    refreshPullAgentActivity();
    refreshBranchAgentButton();
    updateAgentsNavBadge();
    updateAgentActionState();
}

void MainWindow::updateAgentsNavBadge()
{
    if (!m_agentsNavButton)
        return;
    const int total = m_agentSessions.size();
    int running = 0;
    int cloudRunning = 0;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.merged || session.status != AgentStatus::Running)
            continue;
        ++running;
        if (session.cloud)
            ++cloudRunning;
    }
    if (auto *railButton =
            dynamic_cast<ActivityRailButton *>(m_agentsNavButton))
        railButton->setBadgeCount(total);
    if (total > 0) {
        // The count is every repository's, and so is the cloud tail: a mirror
        // run belongs to whichever repository its job named, which is rarely
        // the one open behind the badge.
        m_agentsNavButton->setToolTip(
            QString::fromUtf8("Agents \xE2\x80\x94 %1 running of %2 session%3%4")
                .arg(running)
                .arg(total)
                .arg(total == 1 ? QString() : QStringLiteral("s"),
                     cloudRunning > 0
                         ? QStringLiteral(" (%1 in the cloud)").arg(cloudRunning)
                         : QString()));
    } else {
        m_agentsNavButton->setToolTip(QStringLiteral("Agents"));
    }
    refreshAgentDotMatrix();
}

void MainWindow::refreshAgentDotMatrix()
{
    scheduleSessionsPageRefresh();
    refreshAgentDots();
}

void MainWindow::refreshAgentDots()
{
    if (!m_agentDotMatrix)
        return;
    QVector<AgentDotMatrix::Dot> dots;
    dots.reserve(m_agentSessions.size());
    QHash<QString, int> tally; // status label -> count, for the tooltip
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        AgentDotMatrix::Dot dot;
        dot.sessionId = session.id;
        dot.color = agentStatusIconColor(session);
        dot.running = session.status == AgentStatus::Running;
        if (dot.running) {
            dot.intensity = m_scannerStates.value(session.id).intensity;
            dot.throughput = qBound(
                0.0,
                agentTokensPerSecond(session, sessionTokenTotal(session)) /
                    kAgentFastTokensPerSecond,
                1.0);
        }
        dots.append(dot);
        tally[session.merged ? QStringLiteral("merged")
                             : agentStatusText(session.status)]++;
    }
    m_agentDotMatrix->setDots(dots);
    m_agentDotMatrix->setVisible(
        !dots.isEmpty() && !topBarIndicatorHidden(QStringLiteral("agents")));
    updateChromeDotDivider();

    if (dots.isEmpty()) {
        m_agentDotTooltipKey.clear();
        return;
    }
    QStringList parts;
    for (auto it = tally.constBegin(); it != tally.constEnd(); ++it)
        parts << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
    std::sort(parts.begin(), parts.end());
    const QString key = parts.join(QStringLiteral(", "));
    if (key == m_agentDotTooltipKey)
        return;
    m_agentDotTooltipKey = key;
    QString tip = QString::fromUtf8("%1 agent session%2 \xE2\x80\x94 %3")
                      .arg(dots.size())
                      .arg(dots.size() == 1 ? QString() : QStringLiteral("s"),
                           key);
    tip += QStringLiteral("\nClick a square to open that session.");
    m_agentDotMatrix->setToolTip(tip);
}

#ifdef FORKMESH_WINDOW_TESTS
int MainWindow::testAgentDotCount() const
{
    return m_agentDotMatrix && !m_agentDotMatrix->isHidden()
               ? m_agentDotMatrix->shownCount()
               : 0;
}

void MainWindow::testSetAgentSessionStatus(int sessionId, const QString &status)
{
    if (AgentSession *session = findAgentSession(sessionId)) {
        session->status = status;
        if (m_agentStore)
            m_agentStore->saveSession(*session);
        refreshAgentDotMatrix();
        refreshBranchAgentButton();
    }
}

QString MainWindow::testAgentQueueSnapshot(const QList<int> &sessionIds) const
{
    QStringList parts;
    for (const int sessionId : sessionIds) {
        const AgentSession *session = nullptr;
        for (const AgentSession &candidate : m_agentSessions)
            if (candidate.id == sessionId) {
                session = &candidate;
                break;
            }
        QString entry = QStringLiteral("%1:%2").arg(sessionId).arg(
            session ? session->status : QStringLiteral("<gone>"));
        if (const int at = m_agentQueue.indexOf(sessionId); at >= 0)
            entry += QStringLiteral(":q%1").arg(at);
        parts << entry;
    }
    return parts.join(QLatin1Char(' '));
}

void MainWindow::testRemoveAgentSession(int sessionId)
{
    for (auto it = m_agentSessions.begin(); it != m_agentSessions.end(); ++it) {
        if (it->id != sessionId)
            continue;
        if (m_agentStore)
            m_agentStore->deleteSession(*it);
        m_agentSessions.erase(it);
        m_agentQueue.removeAll(sessionId); // a deleted session must not resume
        refreshAgentDotMatrix();
        return;
    }
}
#endif

QString MainWindow::agentFilterQuery() const
{
    const bool onHome = !m_sectionStack || m_sectionStack->currentIndex() == 0;
    const bool onAgents = onHome && m_repoDetailStack &&
                          m_repoDetailStack->currentIndex() == kRepoAgentsTab;
    return onAgents && m_globalSearch ? m_globalSearch->text().trimmed()
                                      : QString();
}

bool MainWindow::agentTeammateFilterShows(const AgentSession &session) const
{
    switch (m_agentTeammateFilter) {
    case AgentTeammateFilter::TeammatesOnly:
        return session.hasTeammateAttribution();
    case AgentTeammateFilter::WithoutTeammates:
        return !session.hasTeammateAttribution();
    case AgentTeammateFilter::All:
        break;
    }
    return true;
}

bool MainWindow::agentSessionMatchesQuery(const AgentSession &session,
                                          const QString &query) const
{
    if (query.isEmpty())
        return true;
    QStringList haystack{session.issueTitle, agentProviderName(session.provider),
                         agentStatusText(session.status)};
    if (session.cloud)
        haystack << QStringLiteral("cloud") << session.cloudNode;
    else
        haystack << QStringLiteral("desktop");
    if (session.issueNumber > 0)
        haystack << QStringLiteral("#%1").arg(session.issueNumber);
    if (session.prNumber > 0)
        haystack << QStringLiteral("#%1").arg(session.prNumber);
    if (haystack.join(QLatin1Char(' ')).contains(query, Qt::CaseInsensitive))
        return true;
    return m_agentTranscriptSearchQuery == query &&
           m_agentTranscriptHits.contains(session.id);
}

QString MainWindow::agentTranscriptSearchRepoKey() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    return repo.owner + QLatin1Char('/') + repo.name;
}

QString MainWindow::agentTranscriptQuery() const
{
    return m_globalSearch ? m_globalSearch->text().trimmed() : QString();
}

MainWindow::AgentTranscriptHit MainWindow::agentTranscriptHit(int sessionId) const
{
    if (m_agentTranscriptSearchQuery.isEmpty() ||
        m_agentTranscriptSearchQuery != agentTranscriptQuery())
        return AgentTranscriptHit();
    return m_agentTranscriptHits.value(sessionId);
}

void MainWindow::scheduleAgentTranscriptSearch()
{
    const QString query = agentTranscriptQuery();
    if (query.isEmpty()) {
        if (m_agentTranscriptSearchTimer)
            m_agentTranscriptSearchTimer->stop();
        ++m_agentTranscriptSearchGen; // discard whatever is still in flight
        if (!m_agentTranscriptSearchQuery.isEmpty() ||
            !m_agentTranscriptHits.isEmpty()) {
            m_agentTranscriptSearchQuery.clear();
            m_agentTranscriptSearchRepo.clear();
            m_agentTranscriptHits.clear();
            refreshAgentTable();
        }
        return;
    }
    if (query.size() < 2)
        return;
    if (query == m_agentTranscriptSearchQuery &&
        agentTranscriptSearchRepoKey() == m_agentTranscriptSearchRepo)
        return;
    if (!m_agentTranscriptSearchTimer) {
        m_agentTranscriptSearchTimer = new QTimer(this);
        m_agentTranscriptSearchTimer->setSingleShot(true);
        m_agentTranscriptSearchTimer->setInterval(180);
        connect(m_agentTranscriptSearchTimer, &QTimer::timeout, this,
                &MainWindow::runAgentTranscriptSearch);
    }
    m_agentTranscriptSearchTimer->start();
}

void MainWindow::runAgentTranscriptSearch()
{
    const QString query = agentTranscriptQuery();
    if (!m_agentStore || query.size() < 2)
        return;
    QString owner, name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }
    QList<AgentSession> sessions;
    for (const AgentSession &session : std::as_const(m_agentSessions))
        if (session.owner == owner && session.name == name)
            sessions.append(session);
    const int generation = ++m_agentTranscriptSearchGen;
    const QString repoKey = agentTranscriptSearchRepoKey();
    if (sessions.isEmpty()) {
        m_agentTranscriptSearchQuery = query;
        m_agentTranscriptSearchRepo = repoKey;
        if (!m_agentTranscriptHits.isEmpty()) {
            m_agentTranscriptHits.clear();
            refreshAgentTable();
        }
        return;
    }
    const AgentStore store = *m_agentStore;
    runOffThread<QHash<int, AgentTranscriptHit>>(
        [store, sessions, query] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("search agent transcripts"),
                forkmesh::ActionTelemetry::Execution::Worker);
            QHash<int, AgentTranscriptHit> hits;
            for (const AgentSession &session : sessions) {
                AgentTranscriptHit hit;
                hit.count = store.searchTranscript(session, query, &hit.snippet);
                if (hit.count > 0)
                    hits.insert(session.id, hit);
            }
            return hits;
        },
        [this, generation, query, repoKey](QHash<int, AgentTranscriptHit> hits) {
            if (generation != m_agentTranscriptSearchGen ||
                query != agentTranscriptQuery())
                return;
            m_agentTranscriptSearchQuery = query;
            m_agentTranscriptSearchRepo = repoKey;
            m_agentTranscriptHits = std::move(hits);
            refreshAgentTable();
            if (m_globalSearchPopup && m_globalSearchPopup->isVisible())
                rebuildGlobalSearchResults();
        });
}

void MainWindow::scanAgentSessionImages(const QList<AgentSession> &sessions,
                                        const QString &owner, const QString &name)
{
    if (!m_agentImageScanPending || !m_agentStore)
        return;
    m_agentImageScanPending = false;
    if (m_agentImageScanRunning) {
        m_agentImageScanQueued = true; // re-armed when the running pass lands
        return;
    }
    QList<AgentSession> mine;
    for (const AgentSession &session : sessions)
        if (session.owner == owner && session.name == name)
            mine.append(session);
    m_agentImageScanRunning = true;
    const int generation = ++m_agentImageScanGen;
    const int repoIndex = m_repoDetailIndex;
    const AgentStore store = *m_agentStore;
    const QHash<int, QString> knownStamps = m_agentImageStamps;
    const QHash<int, QStringList> known = m_agentSessionImages;
    runOffThread<AgentImageBatch>(
        [store, mine, knownStamps, known] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("scan agent prompt attachments"),
                forkmesh::ActionTelemetry::Execution::Worker);
            AgentImageBatch batch;
            for (const AgentSession &session : mine) {
                const QString stamp = store.transcriptStamp(session) +
                                      QStringLiteral("|%1").arg(session.prompt.size());
                batch.stamps.insert(session.id, stamp);
                if (knownStamps.value(session.id) == stamp) {
                    const QStringList cached = known.value(session.id);
                    if (!cached.isEmpty())
                        batch.images.insert(session.id, cached);
                    continue;
                }
                QStringList resolved;
                const QStringList written = store.attachmentPaths(session);
                for (const QString &raw : written) {
                    const QString path = AgentPromptImages::resolve(raw);
                    if (!path.isEmpty() && !resolved.contains(path))
                        resolved.append(path);
                }
                if (!resolved.isEmpty())
                    batch.images.insert(session.id, resolved);
            }
            return batch;
        },
        [this, generation, repoIndex](AgentImageBatch batch) {
            m_agentImageScanRunning = false;
            if (m_agentImageScanQueued) {
                m_agentImageScanQueued = false;
                m_agentImageScanPending = true;
            }
            if (generation != m_agentImageScanGen || repoIndex != m_repoDetailIndex)
                return;
            const bool changed = batch.images != m_agentSessionImages;
            m_agentImageStamps = std::move(batch.stamps);
            m_agentSessionImages = std::move(batch.images);
            if (changed) {
                refreshAgentTable();
                if (m_selectedAgentSessionId > 0)
                    renderAgentPromptImages(m_selectedAgentSessionId);
            }
        });
}

QStringList MainWindow::agentSessionImages(int sessionId) const
{
    return m_agentSessionImages.value(sessionId);
}

void MainWindow::renderAgentPromptImages(int sessionId)
{
    if (!m_agentPromptImages)
        return;
    auto *row = qobject_cast<QHBoxLayout *>(m_agentPromptImages->layout());
    if (!row)
        return;
    QStringList paths;
    if (const AgentSession *session = findAgentSession(sessionId)) {
        for (const QString &raw : AgentStore::attachmentPathsIn(session->prompt)) {
            const QString path = AgentPromptImages::resolve(raw);
            if (!path.isEmpty() && !paths.contains(path))
                paths.append(path);
        }
    }
    for (const QString &path : agentSessionImages(sessionId))
        if (!paths.contains(path))
            paths.append(path);
    if (m_agentPromptImageSession == sessionId && paths == m_agentPromptImagePaths)
        return;
    m_agentPromptImageSession = sessionId;
    m_agentPromptImagePaths = paths;
    const int generation = ++m_agentPromptImageRenderGen;
    while (QLayoutItem *item = row->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    row->addStretch(1);
    m_agentPromptImages->hide();
    if (paths.isEmpty())
        return;

    runOffThread<QHash<QString, QImage>>(
        [paths] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("decode selected agent attachments"),
                forkmesh::ActionTelemetry::Execution::Worker);
            QHash<QString, QImage> decoded;
            const QSize bounds(220, 76);
            for (const QString &path : paths) {
                if (!isVideoAttachmentPath(path))
                    decoded.insert(path,
                                   decodeAgentPromptStripImage(path, bounds));
            }
            return decoded;
        },
        [this, generation, sessionId, paths](QHash<QString, QImage> decoded) {
            if (generation != m_agentPromptImageRenderGen ||
                sessionId != m_agentPromptImageSession ||
                paths != m_agentPromptImagePaths || !m_agentPromptImages)
                return;
            auto *currentRow =
                qobject_cast<QHBoxLayout *>(m_agentPromptImages->layout());
            if (!currentRow)
                return;
            while (QLayoutItem *item = currentRow->takeAt(0)) {
                if (QWidget *widget = item->widget())
                    widget->deleteLater();
                delete item;
            }
            int shown = 0;
            for (const QString &path : paths) {
                // A video attachment has no frame to decode here, so it stands
                // as a named play button rather than disappearing from the strip.
                if (isVideoAttachmentPath(path)) {
                    auto *play = new QPushButton(
                        QString::fromUtf8("\xE2\x96\xB6  ") +
                            QFileInfo(path).fileName(),
                        m_agentPromptImages);
                    play->setObjectName(QStringLiteral("agentPromptImage"));
                    play->setFlat(true);
                    play->setCursor(Qt::PointingHandCursor);
                    play->setFocusPolicy(Qt::NoFocus);
                    play->setToolTip(QStringLiteral("Play %1").arg(
                        QFileInfo(path).fileName()));
                    connect(play, &QPushButton::clicked, this,
                            [this, path] { showAttachedVideoDetail(path); });
                    currentRow->addWidget(play);
                    ++shown;
                    continue;
                }
                const QImage image = decoded.value(path);
                if (image.isNull())
                    continue;
                const QPixmap pixmap = QPixmap::fromImage(image);
                auto *thumbnail = new QPushButton(m_agentPromptImages);
                thumbnail->setObjectName(QStringLiteral("agentPromptImage"));
                thumbnail->setFlat(true);
                thumbnail->setCursor(Qt::PointingHandCursor);
                thumbnail->setFocusPolicy(Qt::NoFocus);
                thumbnail->setToolTip(QStringLiteral("View %1").arg(
                    QFileInfo(path).fileName()));
                thumbnail->setIcon(QIcon(pixmap));
                thumbnail->setIconSize(pixmap.size());
                thumbnail->setFixedSize(pixmap.width() + 6,
                                        pixmap.height() + 6);
                connect(thumbnail, &QPushButton::clicked, this,
                        [this, path] { showQuickAddImageDetail(path); });
                currentRow->addWidget(thumbnail);
                ++shown;
            }
            currentRow->addStretch(1);
            m_agentPromptImages->setVisible(shown > 0);
        });
}

QIcon MainWindow::agentThumbnail(const QString &path)
{
    const auto cached = m_agentThumbnails.constFind(path);
    if (cached != m_agentThumbnails.constEnd())
        return *cached; // decoded, or null for a file that can't be drawn
    if (m_agentThumbnailsPending.contains(path))
        return QIcon(); // decoding; the row fills in when it lands
    m_agentThumbnailsPending.insert(path);
    const qreal dpr = iconDevicePixelRatio();
    runOffThread<QImage>(
        [path, dpr] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("decode agent attachment thumbnail"),
                forkmesh::ActionTelemetry::Execution::Worker);
            return decodeAgentThumbnail(path, kAgentThumbnailPx, dpr);
        },
        [this, path, dpr](QImage img) {
            m_agentThumbnailsPending.remove(path);
            QIcon square;
            if (!img.isNull()) {
                QPixmap pixmap = QPixmap::fromImage(img);
                pixmap.setDevicePixelRatio(dpr);
                square = QIcon(pixmap);
            }
            m_agentThumbnails.insert(path, square);
            if (!square.isNull())
                applyAgentThumbnail(path);
        });
    return QIcon();
}

void MainWindow::applyAgentThumbnail(const QString &path)
{
    if (!m_agentTable)
        return;
    const QIcon square = m_agentThumbnails.value(path);
    if (square.isNull())
        return;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        QTableWidgetItem *cell = m_agentTable->item(row, kAgentIssueColumn);
        if (!cell)
            continue;
        const QStringList images = cell->data(kAgentImagesRole).toStringList();
        if (!images.isEmpty() && images.first() == path)
            cell->setIcon(square);
    }
}

void MainWindow::showAgentSessionImages(int sessionId)
{
    const QStringList paths = agentSessionImages(sessionId);
    if (paths.isEmpty())
        return;

    auto *dialog = new QDialog(this);
    dialog->setObjectName("imageDetailDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(
        paths.size() == 1
            ? QString::fromUtf8("Agent #%1 \xE2\x80\x94 attached image").arg(sessionId)
            : QString::fromUtf8("Agent #%1 \xE2\x80\x94 %2 attached images")
                  .arg(sessionId)
                  .arg(paths.size()));

    QSize maxSize(1200, 800);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize avail = screen->availableSize();
        maxSize = QSize(avail.width() * 9 / 10, avail.height() * 9 / 10);
    }

    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(12);
    QSize largest(0, 0);
    for (const QString &path : paths) {
        const QPixmap pixmap(path);
        if (pixmap.isNull())
            continue; // deleted between the scan and the click
        QPixmap shown = pixmap;
        if (pixmap.width() > maxSize.width() || pixmap.height() > maxSize.height())
            shown = pixmap.scaled(maxSize, Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation);
        largest = largest.expandedTo(shown.size());
        auto *image = new QLabel;
        image->setAlignment(Qt::AlignCenter);
        image->setPixmap(shown);
        bodyLayout->addWidget(image);
        auto *caption = new QLabel(QString::fromUtf8("%1  \xC2\xB7  %2 \xC3\x97 %3")
                                       .arg(QFileInfo(path).fileName())
                                       .arg(pixmap.width())
                                       .arg(pixmap.height()));
        caption->setObjectName("mutedLabel");
        caption->setAlignment(Qt::AlignCenter);
        bodyLayout->addWidget(caption);
    }
    if (largest.isEmpty()) { // every attachment has gone missing
        delete body; // not parented to the dialog until the scroll area below
        delete dialog;
        logSystem(QStringLiteral(
            "That agent's attached image is no longer on disk."));
        return;
    }
    bodyLayout->addStretch();

    auto *scroll = new QScrollArea;
    scroll->setObjectName("messageView");
    scroll->setWidgetResizable(true);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setWidget(body);

    auto *closeButton = new QPushButton(QStringLiteral("Close"));
    closeButton->setObjectName("primaryButton");
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    layout->addWidget(scroll, 1);
    layout->addLayout(buttonRow);

    dialog->resize(qMin(largest.width() + 48, maxSize.width()),
                   qMin(largest.height() + 120, maxSize.height()));
    dialog->show();
}

void MainWindow::refreshAgentTable()
{
    scheduleTeammateRailRefresh();
    scheduleSessionsPageRefresh();
    if (!m_agentTable)
        return;
    if (m_agentTableRefreshing)
        return;
    m_agentTableRefreshing = true;
    struct RefreshGuard {
        bool &flag;
        ~RefreshGuard() { flag = false; }
    } refreshGuard{m_agentTableRefreshing};
    QString owner, name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }

    const int keep = m_selectedAgentSessionId;
    QList<int> keepSelected = selectedAgentSessionIds();
    // Only carry a multi-selection the user made around the open session.
    // Navigation (a new agent, a Ping, switchToAgentsTab) publishes a new ID
    // first; unioning it with the stale rows piled up highlighted agents the
    // user never chose.
    if (keep > 0 && !keepSelected.contains(keep))
        keepSelected = {keep};
    const int scrollPos =
        m_agentTable->verticalScrollBar()
            ? m_agentTable->verticalScrollBar()->value()
            : 0;
    const QString agentGitDir = repoGitDir();
    QString agentBase = m_repoInfo.defaultBranch.trimmed();
    if (agentBase.isEmpty()) {
        const QStringList cachedBranches =
            m_branchesCacheDir == agentGitDir ? m_branchesCache : QStringList();
        if (cachedBranches.contains(QStringLiteral("main")))
            agentBase = QStringLiteral("main");
        else if (cachedBranches.contains(QStringLiteral("master")))
            agentBase = QStringLiteral("master");
        else
            agentBase = m_repoBranch;
    }
    const QString query = agentFilterQuery();
    QList<AgentSession> sessions = m_agentSessions;
    auto passesFilter = [&](const AgentSession &session) {
        return session.owner == owner && session.name == name &&
               agentTeammateFilterShows(session) &&
               agentSessionMatchesQuery(session, query);
    };

    if (m_agentDiffRefreshPending)
        startAgentDiffStatsRefresh(sessions, owner, name, agentGitDir);

    scanAgentSessionImages(sessions, owner, name);

    QList<const AgentSession *> visible;
    int mergedDeletable = 0;
    for (const AgentSession &session : std::as_const(sessions)) {
        if (session.owner != owner || session.name != name)
            continue;
        if (session.merged && !session.branchName.isEmpty()
            && !isExternalSession(session.id))
            ++mergedDeletable;
        if (passesFilter(session))
            visible.append(&session);
    }

    sortAgentTableRows(visible, query);

    const QString folderRepo = AgentFolders::repoKey(owner, name);
    QStringList memberKeys;
    memberKeys.reserve(visible.size());
    for (const AgentSession *sp : std::as_const(visible))
        memberKeys << agentReadKey(*sp);
    const QList<AgentFolderRow> layout =
        m_agentFolders.layout(folderRepo, memberKeys, !query.isEmpty());
    QStringList identities;
    identities.reserve(layout.size());
    QSet<int> laidOut;
    for (const AgentFolderRow &entry : layout) {
        if (entry.header()) {
            identities << QStringLiteral("f:") + entry.folderId;
            continue;
        }
        const int id = visible.at(entry.member)->id;
        identities << QStringLiteral("s:") + QString::number(id);
        laidOut.insert(id);
    }
    bool keepFolded = false;
    for (const AgentSession *sp : std::as_const(visible))
        if (sp->id == keep && !laidOut.contains(keep))
            keepFolded = true;

    const bool reuseRows = agentTableRowsMatch(identities);

    QSignalBlocker block(m_agentTable);
    TableRepaintGuard repaintGuard(m_agentTable);
    if (!reuseRows) {
        m_agentTable->clearSpans();
        m_agentTable->setRowCount(layout.size());
    }
    const int sessionRowHeight =
        m_agentTable->verticalHeader()->defaultSectionSize();
    for (int row = 0; row < layout.size(); ++row) {
        const AgentFolderRow &entry = layout.at(row);
        if (entry.header()) {
            const AgentFolder *folder =
                m_agentFolders.folder(folderRepo, entry.folderId);
            QList<const AgentSession *> members;
            for (int i = 0; i < visible.size(); ++i)
                if (m_agentFolders.folderOf(folderRepo, memberKeys.at(i)) ==
                    entry.folderId)
                    members << visible.at(i);
            if (folder)
                applyAgentFolderRowCells(row, *folder, members, !query.isEmpty());
            if (m_agentCompactRowHeight > 0 &&
                m_agentTable->rowHeight(row) != m_agentCompactRowHeight)
                m_agentTable->setRowHeight(row, m_agentCompactRowHeight);
            continue;
        }
        if (QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
            lead && lead->data(kAgentFolderRole).isValid()) {
            if (m_agentTable->columnSpan(row, 0) > 1)
                m_agentTable->setSpan(row, 0, 1, 1);
            delete m_agentTable->takeItem(row, kAgentIdColumn);
            delete m_agentTable->takeItem(row, kAgentIssueColumn);
        }
        applyAgentRowCells(row, *visible.at(entry.member), agentGitDir, agentBase);
        if (QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn))
            lead->setData(kAgentRowFolderRole, entry.folderId);
        if (m_agentTable->rowHeight(row) != sessionRowHeight)
            m_agentTable->setRowHeight(row, sessionRowHeight);
    }
    const bool noneVisible = visible.isEmpty();
    visible.clear();
    sessions = QList<AgentSession>();
    if (m_agentDeleteMergedButton) {
        m_agentDeleteMergedButton->setEnabled(mergedDeletable > 0);
        m_agentDeleteMergedButton->setToolTip(
            mergedDeletable > 0
                ? QStringLiteral("Delete the worktree, branch and session of %1 "
                                 "merged agent%2")
                      .arg(mergedDeletable)
                      .arg(mergedDeletable == 1 ? QString() : QStringLiteral("s"))
                : QStringLiteral("No merged agent sessions to delete"));
    }

    int selRow = -1;
    QList<int> selectedRows;
    int firstSessionRow = -1;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *lead = m_agentTable->item(row, 0);
        if (!lead || lead->data(kAgentFolderRole).isValid())
            continue;
        if (firstSessionRow < 0)
            firstSessionRow = row;
        const int sessionId = lead->data(Qt::UserRole).toInt();
        if (keepSelected.contains(sessionId))
            selectedRows.append(row);
        if (sessionId == keep) {
            selRow = row;
        }
    }
    if (selRow < 0 && !selectedRows.isEmpty())
        selRow = selectedRows.first();
    if (selRow < 0 && !keepFolded)
        selRow = firstSessionRow;
    int selectedId = keepFolded ? keep : -1;
    QItemSelectionModel *selection = m_agentTable->selectionModel();
    selection->clearSelection();
    for (const int row : std::as_const(selectedRows)) {
        selection->select(m_agentTable->model()->index(row, 0),
                          QItemSelectionModel::Select |
                              QItemSelectionModel::Rows);
    }
    if (selRow >= 0) {
        const QModelIndex current = m_agentTable->model()->index(selRow, 0);
        selection->select(current, QItemSelectionModel::Select |
                                      QItemSelectionModel::Rows);
        selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
        if (QTableWidgetItem *selected =
                m_agentTable->item(selRow, kAgentIdColumn))
            selectedId = selected->data(Qt::UserRole).toInt();
    }
    block.unblock();
    if (selectedId != keep) {
        // Selection-model signals used to call showAgentSession() while this
        // table was still sorting and sizing itself. Finish the table event
        // first, then select and build the transcript on a clean event-loop
        // turn. Keep the old selection until then: navigation callers publish
        // their requested ID before refreshing, and a fast second click must
        // invalidate this fallback rather than briefly reporting another row.
        QTimer::singleShot(0, this, [this, selectedId, keep] {
            if (m_selectedAgentSessionId != keep)
                return;
            // Pin the old surface before showAgentSession() can reveal and
            // resize the detail pane. Otherwise its newly-visible scrollbar
            // paints at the top for a frame before it reaches the latest turn.
            if (m_agentTranscript)
                m_agentTranscript->jumpToBottom();
            showAgentSession(selectedId);
        });
    }
    if (m_agentTable->verticalScrollBar())
        m_agentTable->verticalScrollBar()->setValue(scrollPos);
    updateAgentActionState();
    // A filtered-to-nothing table is just a blank pane, with no clue that a
    // term three panes away is what emptied it.
    updateSearchEmptyNotice(m_agentSearchEmpty, m_agentTable, query,
                            noneVisible);
    updateAgentRecycleBinBar(); // the open repository may have changed
    pumpCloudAgentMoves();
}

// Rebuilds the per-session diff/worktree stats on a worker. Each stat lands as
// soon as it is read; the batch then prunes rows that no longer exist and
// refreshes the table again.
void MainWindow::startAgentDiffStatsRefresh(const QList<AgentSession> &sessions,
                                            const QString &owner,
                                            const QString &name,
                                            const QString &agentGitDir)
{
    m_agentDiffRefreshPending = false;
    const bool livePass = m_agentDiffLivePass && !m_agentDiffStatsRefreshQueued;
    if (m_agentDiffStatsRefreshing) {
        ++m_agentDiffStatsGen;
        m_agentDiffStatsRefreshQueued = true;
    } else if (agentDiffCooldownRemainingMs() > 0) {
        if (!livePass)
            m_agentDiffStatsRefreshQueued = true;
        armAgentDiffPassTimer(0);
    } else if (m_agentStore) {
        ++m_agentDiffStatsGen;
        m_agentDiffStatsRefreshing = true;
        m_agentDiffStatsRefreshQueued = false;
        m_agentDiffCooldownWaived = false;
        if (m_agentLiveDiffTimer)
            m_agentLiveDiffTimer->stop();
#ifdef FORKMESH_WINDOW_TESTS
        ++m_testAgentDiffPassCount;
#endif
        const QSet<int> editedIds =
            livePass ? m_agentLiveDiffSessions : QSet<int>();
        m_agentLiveDiffSessions.clear();
        m_agentDiffPassClock.start();
        const int generation = m_agentDiffStatsGen;
        const int repoIndex = m_repoDetailIndex;
        const QString configuredBase = m_repoInfo.defaultBranch;
        const QString checkedOut = m_repoBranch;
        const QHash<int, QString> oldSignatures = m_agentDiffSig;
        const QHash<int, AgentDiffStat> statsBefore = m_agentDiffStats;
        const QSet<int> cachedIds =
            QSet<int>(m_agentDiffStats.keyBegin(),
                      m_agentDiffStats.keyEnd());
        const int selectedSessionId = m_selectedAgentSessionId;
        const AgentStore store = *m_agentStore;
        runOffThread<AgentDiffBatch>(
            [this, store, sessions, owner, name, agentGitDir, configuredBase,
             checkedOut, oldSignatures, cachedIds, selectedSessionId,
             generation, repoIndex, livePass, editedIds] {
                const forkmesh::BackgroundScope activity(
                    QStringLiteral("agents"),
                    QStringLiteral("refresh diff and worktree status"),
                    forkmesh::ActionTelemetry::Execution::Worker);
                AgentDiffBatch batch;
                batch.base = backgroundDefaultBranch(
                    agentGitDir, configuredBase, checkedOut);
                const QHash<QString, QString> worktrees =
                    backgroundWorktrees(agentGitDir);
                const QHash<QString, QString> branchTips =
                    backgroundBranchTips(agentGitDir);
                for (const AgentSession &session : sessions) {
                    if (session.owner != owner || session.name != name)
                        continue;
                    batch.liveIds.insert(session.id);
                    const bool active =
                        session.status == AgentStatus::Running ||
                        session.status == AgentStatus::Waiting ||
                        session.status == AgentStatus::Queued;
                    const QString requestedBase = session.baseBranch.trimmed();
                    const QString sessionBase =
                        !requestedBase.isEmpty() &&
                                branchTips.contains(requestedBase)
                            ? requestedBase
                            : batch.base;
                    const QString signature =
                        QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                            .arg(session.status, session.branchName,
                                 sessionBase, branchTips.value(sessionBase))
                            .arg(session.merged ? 1 : 0)
                            .arg(session.finishedAtMs)
                            .arg(session.prNumber)
                            .arg(branchTips.value(session.branchName));
                    batch.signatures.insert(session.id, signature);
                    const bool measureActive =
                        active &&
                        (!livePass || editedIds.contains(session.id));
                    if (measureActive || session.id == selectedSessionId ||
                        oldSignatures.value(session.id) != signature ||
                        !cachedIds.contains(session.id)) {
                        const bool signatureChanged =
                            oldSignatures.value(session.id) != signature;
                        const bool shouldProbeConflict =
                            session.id == selectedSessionId || signatureChanged ||
                            !cachedIds.contains(session.id);
                        const AgentDiffStat stat = readAgentDiffStat(
                            store, session, agentGitDir, sessionBase,
                            worktrees.value(session.branchName),
                            session.id == selectedSessionId,
                            !active && shouldProbeConflict);
                        batch.stats.insert(session.id, stat);
                        QMetaObject::invokeMethod(
                            this,
                            [this, generation, repoIndex, id = session.id,
                             stat, signature] {
                                applyAgentDiffStatResult(
                                    generation, repoIndex, id, stat,
                                    signature);
                            },
                            Qt::QueuedConnection);
                    }
                }
                return batch;
            },
            [this, generation, repoIndex, statsBefore](AgentDiffBatch batch) {
                m_agentDiffStatsRefreshing = false;
                m_agentDiffPassMs = m_agentDiffPassClock.elapsed();
                m_agentDiffPassEnded.start();
                bool changed = false;
                if (generation == m_agentDiffStatsGen &&
                    repoIndex == m_repoDetailIndex) {
                    for (auto it = m_agentDiffStats.begin();
                         it != m_agentDiffStats.end();) {
                        if (batch.liveIds.contains(it.key())) {
                            ++it;
                        } else {
                            it = m_agentDiffStats.erase(it);
                            changed = true;
                        }
                    }
                    for (auto it = batch.stats.cbegin();
                         it != batch.stats.cend(); ++it) {
                        const auto before = statsBefore.constFind(it.key());
                        if (before == statsBefore.cend() ||
                            !sameAgentDiffStat(*before, it.value()))
                            changed = true;
                        m_agentDiffStats.insert(it.key(), it.value());
                        if (!it->worktree.isEmpty())
                            m_sessionWorkdirCache.insert(it.key(),
                                                         it->worktree);
                    }
                    m_agentDiffSig = std::move(batch.signatures);
                    refreshAgentSyncControls(m_selectedAgentSessionId);
                }
                if (m_agentDiffStatsRefreshQueued ||
                    !m_agentLiveDiffSessions.isEmpty())
                    armAgentDiffPassTimer(0);
                if (changed || m_agentDiffRefreshPending)
                    refreshAgentTable();
            });
    }
}

// True when the table already shows exactly these sessions in this order, so
// refreshAgentTable can update cells in place instead of rebuilding rows.
bool MainWindow::agentTableRowsMatch(const QStringList &identities) const
{
    if (m_agentTable->rowCount() != identities.size())
        return false;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *it = m_agentTable->item(row, 0);
        if (!it)
            return false;
        const QString identity =
            it->data(kAgentFolderRole).isValid()
                ? QStringLiteral("f:") + it->data(kAgentFolderRole).toString()
                : QStringLiteral("s:") +
                      QString::number(it->data(Qt::UserRole).toInt());
        if (identity != identities.at(row))
            return false;
    }
    return true;
}

// Orders the visible rows by the chosen list sort, then (while searching)
// floats title matches above rows that matched only their transcript.
void MainWindow::sortAgentTableRows(QList<const AgentSession *> &visible,
                                    const QString &query)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto compareTimestamp = [](qint64 a, qint64 b, bool newestFirst) {
        if (a == b)
            return 0;
        // Legacy sessions may not have every timestamp. Keep unknown dates at
        // the bottom in either direction instead of presenting them as the
        // oldest or newest known run.
        if (a <= 0)
            return 1;
        if (b <= 0)
            return -1;
        if (newestFirst)
            return a > b ? -1 : 1;
        return a < b ? -1 : 1;
    };
    const auto statusRank = [](const AgentSession *session) {
        if (!session || session->merged)
            return 7;
        if (session->status == AgentStatus::Running)
            return 0;
        if (session->status == AgentStatus::Waiting)
            return 1;
        if (session->status == AgentStatus::Queued)
            return 2;
        if (session->status == AgentStatus::Failed)
            return 3;
        if (session->status == AgentStatus::Stopped)
            return 4;
        if (session->status == AgentStatus::Success)
            return 5;
        return 6;
    };
    const auto conflictRank = [this](const AgentSession *session) {
        if (!session || session->merged)
            return 4;
        const AgentDiffStat stat = m_agentDiffStats.value(session->id);
        if (stat.conflicted)
            return 0;
        if (stat.behind > 0)
            return 1;
        if (stat.behind == 0)
            return 2;
        return 3;
    };
    std::stable_sort(
        visible.begin(), visible.end(),
        [this, nowMs, &compareTimestamp, &statusRank,
         &conflictRank](const AgentSession *a, const AgentSession *b) {
            int order = 0;
            switch (m_agentListSort) {
            case AgentListSort::Priority:
                if (a->priority != b->priority)
                    return a->priority < b->priority;
                break;
            case AgentListSort::NewestAdded:
                order = compareTimestamp(a->createdAtMs, b->createdAtMs, true);
                break;
            case AgentListSort::OldestAdded:
                order = compareTimestamp(a->createdAtMs, b->createdAtMs, false);
                break;
            case AgentListSort::RecentActivity: {
                const qint64 aActivity = qMax(
                    m_scannerStates.value(a->id).lastActivityMs,
                    qMax(a->createdAtMs,
                         qMax(a->startedAtMs, a->finishedAtMs)));
                const qint64 bActivity = qMax(
                    m_scannerStates.value(b->id).lastActivityMs,
                    qMax(b->createdAtMs,
                         qMax(b->startedAtMs, b->finishedAtMs)));
                order = compareTimestamp(aActivity, bActivity, true);
                break;
            }
            case AgentListSort::Status:
                order = statusRank(a) - statusRank(b);
                break;
            case AgentListSort::Title:
                order = QString::localeAwareCompare(a->issueTitle, b->issueTitle);
                break;
            case AgentListSort::Provider: {
                const QString aProvider =
                    agentProviderName(a->provider) + QLatin1Char(' ') + a->model;
                const QString bProvider =
                    agentProviderName(b->provider) + QLatin1Char(' ') + b->model;
                order = QString::localeAwareCompare(aProvider, bProvider);
                break;
            }
            case AgentListSort::IssueNumber:
                if (a->issueNumber != b->issueNumber) {
                    if (a->issueNumber <= 0)
                        return false;
                    if (b->issueNumber <= 0)
                        return true;
                    return a->issueNumber < b->issueNumber;
                }
                break;
            case AgentListSort::TokenUsage: {
                const qint64 aTokens = sessionTokenTotal(*a);
                const qint64 bTokens = sessionTokenTotal(*b);
                if (aTokens != bTokens)
                    return aTokens > bTokens;
                break;
            }
            case AgentListSort::Cost:
                if (a->costUsd != b->costUsd)
                    return a->costUsd > b->costUsd;
                break;
            case AgentListSort::Runtime: {
                const qint64 aRuntime = a->totalRunMs(nowMs);
                const qint64 bRuntime = b->totalRunMs(nowMs);
                if (aRuntime != bRuntime)
                    return aRuntime > bRuntime;
                break;
            }
            case AgentListSort::FilesChanged: {
                const int aFiles = m_agentDiffStats.value(a->id).files;
                const int bFiles = m_agentDiffStats.value(b->id).files;
                if (aFiles != bFiles)
                    return aFiles > bFiles;
                break;
            }
            case AgentListSort::Conflicts:
                order = conflictRank(a) - conflictRank(b);
                break;
            }
            if (order != 0)
                return order < 0;
            if (a->priority != b->priority)
                return a->priority < b->priority;
            return a->id < b->id;
        });

    if (!query.isEmpty()) {
        // A title is the strongest signal in an Agents search. Keep rows that
        // only matched their transcript or metadata below title matches, while
        // preserving the session order within each group.
        std::stable_sort(
            visible.begin(), visible.end(),
            [&query](const AgentSession *a, const AgentSession *b) {
                const bool aTitle = a->issueTitle.contains(
                    query, Qt::CaseInsensitive);
                const bool bTitle = b->issueTitle.contains(
                    query, Qt::CaseInsensitive);
                return aTitle && !bTitle;
            });
    }
}

QList<int> MainWindow::selectedAgentSessionIds() const
{
    QList<int> ids;
    if (!m_agentTable || !m_agentTable->selectionModel())
        return ids;
    const QModelIndexList rows = m_agentTable->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        const QTableWidgetItem *item =
            m_agentTable->item(row.row(), kAgentIdColumn);
        const int sessionId = item ? item->data(Qt::UserRole).toInt() : 0;
        if (sessionId > 0)
            ids.append(sessionId);
    }
    return ids;
}

void MainWindow::deleteSelectedAgentSessions()
{
    const QList<int> sessionIds = selectedAgentSessionIds();
    if (sessionIds.isEmpty())
        return;
    if (sessionIds.size() == 1) {
        deleteAgentSessionFromRow(sessionIds.first());
        return;
    }
    if (auto *existing = findChild<QMessageBox *>(
            QStringLiteral("agentBulkDeleteConfirm"))) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    auto *box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("agentBulkDeleteConfirm"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QStringLiteral("Delete selected agent sessions"));
    box->setText(QStringLiteral("Delete %1 selected agent sessions?")
                     .arg(sessionIds.size()));
    box->setInformativeText(
        QStringLiteral("This deletes each selected session, its transcript, and "
                       "any attached unmerged pull request, worktree folder and "
                       "branch. Merged pull requests are retained. This cannot be "
                       "undone."));
    QPushButton *confirm = box->addButton(
        QStringLiteral("Delete %1 agents").arg(sessionIds.size()),
        QMessageBox::DestructiveRole);
    confirm->setObjectName(QStringLiteral("agentBulkDeleteConfirmButton"));
    QPushButton *cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, confirm, sessionIds](QAbstractButton *clicked) {
                if (clicked != confirm)
                    return;
                for (const int sessionId : sessionIds)
                    if (findAgentSession(sessionId))
                        deleteAgentSessionAndCleanup(sessionId);
            });
    box->show();
}

void MainWindow::setAgentListSort(AgentListSort sort)
{
    m_agentListSort = sort;
    QString label;
    switch (sort) {
    case AgentListSort::Priority:
        label = QStringLiteral("Priority");
        break;
    case AgentListSort::NewestAdded:
        label = QStringLiteral("Newest added");
        break;
    case AgentListSort::OldestAdded:
        label = QStringLiteral("Oldest added");
        break;
    case AgentListSort::RecentActivity:
        label = QStringLiteral("Recent activity");
        break;
    case AgentListSort::Status:
        label = QStringLiteral("Status");
        break;
    case AgentListSort::Title:
        label = QStringLiteral("Title");
        break;
    case AgentListSort::Provider:
        label = QStringLiteral("Provider and model");
        break;
    case AgentListSort::IssueNumber:
        label = QStringLiteral("Issue number");
        break;
    case AgentListSort::TokenUsage:
        label = QStringLiteral("Token usage");
        break;
    case AgentListSort::Cost:
        label = QStringLiteral("Cost");
        break;
    case AgentListSort::Runtime:
        label = QStringLiteral("Runtime");
        break;
    case AgentListSort::FilesChanged:
        label = QStringLiteral("Files changed");
        break;
    case AgentListSort::Conflicts:
        label = QStringLiteral("Conflicts");
        break;
    }
    if (m_agentSortButton) {
        m_agentSortButton->setAccessibleName(
            QStringLiteral("Sort agents: %1").arg(label));
        m_agentSortButton->setToolTip(
            sort == AgentListSort::Priority
                ? QStringLiteral("Sort agents by priority. Drag rows to change "
                                 "their priority order.")
                : QStringLiteral("Sort agents by %1").arg(label.toLower()));
        if (QMenu *menu = m_agentSortButton->menu()) {
            for (QAction *action : menu->actions())
                if (action->isCheckable())
                    action->setChecked(
                        action->data().toInt() == static_cast<int>(sort));
        }
    }
    if (m_agentTable) {
        const bool priorityOrder = sort == AgentListSort::Priority;
        m_agentTable->setToolTip(
            priorityOrder
                ? QStringLiteral("Drag an agent row to change its priority (1 is "
                                 "highest; 99 is lowest), or onto a folder to "
                                 "file it there. Right-click for folders.")
                : QStringLiteral("Drag agents onto a folder to file them there, "
                                 "and drag folders to reorder them. Choose "
                                 "Priority sorting to drag agents into a new "
                                 "priority order. Right-click for folders."));
        refreshAgentTable();
    }
}

void MainWindow::setAgentTeammateFilter(AgentTeammateFilter filter)
{
    m_agentTeammateFilter = filter;
    if (m_agentTeammateFilterButton) {
        QString name;
        QString tip;
        QString glyph;
        QColor glyphDark;
        QColor glyphLight;
        switch (filter) {
        case AgentTeammateFilter::All:
            name = QStringLiteral("Hide special agent sessions");
            tip = QStringLiteral("Showing every agent session. Click to hide "
                                 "the sessions special agents started");
            break;
        case AgentTeammateFilter::TeammatesOnly:
            name = QStringLiteral("Show all agent sessions");
            tip = QStringLiteral("Showing only sessions started by special agents. "
                                 "Click to show every agent session");
            glyph = QStringLiteral("check-white");
            glyphDark = Theme::nextOr(Theme::Role::AccentFg, QColor("#3fb950"));
            glyphLight = Theme::nextOr(Theme::Role::AccentFg, QColor("#1a7f37"));
            break;
        case AgentTeammateFilter::WithoutTeammates:
            name = QStringLiteral("Show only special agent sessions");
            tip = QStringLiteral("Special agent sessions are hidden: showing only "
                                 "sessions no special agent started. Click to show "
                                 "only sessions started by special agents");
            glyph = QStringLiteral("circle-slash");
            glyphDark = QColor("#f85149");
            glyphLight = QColor("#cf222e");
            break;
        }
        m_agentTeammateFilterButton->setAccessibleName(name);
        m_agentTeammateFilterButton->setToolTip(tip);
        m_agentTeammateFilterButton->setStateGlyph(glyph, glyphDark, glyphLight);
        // The button keeps its lit look for both filtered states so a glance
        // still separates "filtered" from "showing everything".
        m_agentTeammateFilterButton->setChecked(filter !=
                                                AgentTeammateFilter::All);
    }
    if (m_agentTable)
        refreshAgentTable();
}

void MainWindow::reprioritizeAgentSessions(int sourceSessionId,
                                           int targetSessionId,
                                           bool placeAfterTarget)
{
    if (!m_agentStore || sourceSessionId <= 0 || targetSessionId <= 0 ||
        sourceSessionId == targetSessionId)
        return;
    const AgentSession *source = findAgentSession(sourceSessionId);
    const AgentSession *target = findAgentSession(targetSessionId);
    if (!source || !target || source->owner != target->owner ||
        source->name != target->name)
        return;

    const QString owner = source->owner;
    const QString name = source->name;
    QList<int> ordered;
    for (const AgentSession &session : std::as_const(m_agentSessions))
        if (session.id > 0 && session.owner == owner && session.name == name)
            ordered.append(session.id);
    std::stable_sort(ordered.begin(), ordered.end(), [this](int a, int b) {
        const AgentSession *left = findAgentSession(a);
        const AgentSession *right = findAgentSession(b);
        return left && right && left->priority < right->priority;
    });

    if (!ordered.removeOne(sourceSessionId))
        return;
    int targetIndex = ordered.indexOf(targetSessionId);
    if (targetIndex < 0)
        return;
    if (placeAfterTarget)
        ++targetIndex;
    ordered.insert(targetIndex, sourceSessionId);

    bool saved = true;
    for (int i = 0; i < ordered.size(); ++i) {
        AgentSession *session = findAgentSession(ordered.at(i));
        if (!session)
            continue;
        const int priority = qMin(99, i + 1);
        if (session->priority == priority)
            continue;
        session->priority = priority;
        saved &= m_agentStore->saveSession(*session);
    }
    if (!saved) {
        reloadAgents(AgentReloadScan::FromCache);
        flashMessage(QStringLiteral("Could not save the new agent priority order."),
                     true);
        return;
    }
    m_selectedAgentSessionId = sourceSessionId;
    refreshAgentTable();
    updateAgentActionState();
    scheduleAgentQueuePump();
    flashMessage(QStringLiteral("Agent #%1 is now priority %2.")
                     .arg(sourceSessionId)
                     .arg(findAgentSession(sourceSessionId)
                              ? findAgentSession(sourceSessionId)->priority
                              : 50));
}

void MainWindow::moveAgentSessionToPriorityEdge(int sessionId, bool bottom)
{
    if (!m_agentStore || sessionId <= 0)
        return;
    const AgentSession *source = findAgentSession(sessionId);
    if (!source)
        return;

    QList<int> ordered;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.id > 0 && session.owner == source->owner &&
            session.name == source->name)
            ordered.append(session.id);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [this](int a, int b) {
        const AgentSession *left = findAgentSession(a);
        const AgentSession *right = findAgentSession(b);
        return left && right && left->priority < right->priority;
    });
    if (ordered.size() < 2)
        return;

    const int targetSessionId = bottom ? ordered.last() : ordered.first();
    if (targetSessionId != sessionId)
        reprioritizeAgentSessions(sessionId, targetSessionId,
                                  /*placeAfterTarget=*/bottom);
}

void MainWindow::applyAgentRowCells(int row, const AgentSession &session,
                                    const QString &agentGitDir,
                                    const QString &agentBase)
{
    if (!m_agentTable)
        return;
    auto plain = [&](int col) -> QTableWidgetItem * {
        QTableWidgetItem *it = m_agentTable->item(row, col);
        if (!it) {
            it = new QTableWidgetItem;
            m_agentTable->setItem(row, col, it);
        }
        return it;
    };
    auto sortable = [&](int col) -> QTableWidgetItem * {
        QTableWidgetItem *it = m_agentTable->item(row, col);
        if (!it) {
            it = new SortTableWidgetItem;
            m_agentTable->setItem(row, col, it);
        }
        return it;
    };

    const AgentDiffStat diffStat = agentDiffStat(session, agentGitDir, agentBase);
    applyAgentStatusCell(sortable(kAgentIdColumn), session, diffStat, agentBase,
                         session.id, agentSessionPullAvatar(session),
                         agentSpinAngle(session), agentRunToggleMode(session));
    QString title = session.issueNumber > 0 ? QStringLiteral("#%1 %2")
                                                  .arg(session.issueNumber)
                                                  .arg(session.issueTitle)
                                            : session.issueTitle;
    if (m_agentListView == AgentListView::Zen)
        title = session.issueTitle;
    else if (session.id > 0 && m_agentListView == AgentListView::Compact) {
        title = QString::fromUtf8("P%1  \xC2\xB7  %2")
                    .arg(qBound(1, session.priority, 99), 2, 10,
                         QLatin1Char('0'))
                    .arg(title);
    }
    QTableWidgetItem *issueCell = plain(kAgentIssueColumn);
    const AgentTranscriptHit hit = agentFilterQuery().isEmpty()
                                       ? AgentTranscriptHit()
                                       : agentTranscriptHit(session.id);
    QStringList tip;
    if (session.id > 0)
        tip << QStringLiteral("Priority %1 (1 is highest, 99 is lowest)")
                   .arg(qBound(1, session.priority, 99));
    if (!session.orgTaskId.isEmpty())
        tip << QStringLiteral("Organization task %1").arg(session.orgTaskId);
    tip << (!session.cloud ? QStringLiteral("Desktop agent — runs on this desktop")
            : session.cloudNode.isEmpty()
                ? QStringLiteral("Cloud agent — not claimed by a node yet")
                : QStringLiteral("Cloud agent on %1").arg(session.cloudNode));
    if (hit.count > 0 && m_agentListView != AgentListView::Zen) {
        title += QString::fromUtf8("   \xC2\xB7  %1 in transcript")
                     .arg(hit.count);
        tip << hit.snippet;
    }
    issueCell->setText(title);
    const bool ticking = m_agentListView == AgentListView::Compact &&
                         !session.merged && session.status == AgentStatus::Running;
    issueCell->setData(kAgentTickerRole,
                       ticking ? agentTickerText(session.id,
                                                 m_lastAssistantText.value(session.id))
                               : QString());
    if (agentSessionWatched(session.id) && noteAgentSessionSeen(session))
        notifyLocalControlAgentsChanged();
    const bool unread = agentSessionUnread(session);
    issueCell->setData(kAgentUnreadRole, unread);
    if (unread)
        tip << QStringLiteral("Unread: finished since you last opened it");
    const QStringList images = agentSessionImages(session.id);
    issueCell->setData(kAgentImagesRole, images);
    issueCell->setData(kAgentImagesSessionRole, session.id);
    if (m_agentListView != AgentListView::Compact)
        issueCell->setIcon(QIcon());
    else
        issueCell->setIcon(!images.isEmpty()
                               ? agentThumbnail(images.first())
                               : themedOcticon(session.cloud
                                                   ? QStringLiteral("cloud")
                                                   : QStringLiteral("device-desktop"),
                                               session.cloud ? Theme::nextOr(Theme::Role::AccentFg,
                                                                             QColor("#58a6ff"))
                                                             : Theme::remapColor(QColor("#8b949e")),
                                               14));
    applyAgentRowChips(row, session, diffStat);
    if (!images.isEmpty())
        tip << (images.size() == 1
                    ? QString::fromUtf8("1 attached image \xE2\x80\x94 click the "
                                     "thumbnail to see it")
                    : QString::fromUtf8("%1 attached images \xE2\x80\x94 click the "
                                     "thumbnail to see them")
                          .arg(images.size()));
    issueCell->setToolTip(tip.join(QLatin1Char('\n')));
}

void MainWindow::applyAgentRowChips(int row, const AgentSession &session,
                                    const AgentDiffStat &stat)
{
    QTableWidgetItem *cell =
        m_agentTable ? m_agentTable->item(row, kAgentIssueColumn) : nullptr;
    if (!cell)
        return;
    cell->setData(kAgentRowChipsRole, m_agentListView == AgentListView::Full
                                          ? QVariant(agentRowChips(session, stat))
                                          : QVariant());
}

void MainWindow::setAgentListView(AgentListView view)
{
    m_agentListView = view;
    QString description;
    switch (view) {
    case AgentListView::Compact:
        description = QStringLiteral("status, branch and title on one line");
        break;
    case AgentListView::Zen:
        description = QStringLiteral("nothing but each agent's title");
        break;
    case AgentListView::Full:
        description = QStringLiteral("the title, with each detail named in "
                                     "one word underneath");
        break;
    }
    forkmesh::ui::storeListViewMode(QStringLiteral("agents/listView"), view);
    if (m_agentViewButton) {
        const forkmesh::ui::ListViewModeLabel label =
            forkmesh::ui::listViewModeLabel(view);
        m_agentViewButton->setGlyph(label.icon, label.caption);
        m_agentViewButton->setAccessibleName(
            forkmesh::ui::listViewModeAccessibleName(
                QStringLiteral("Agent list"), view));
        m_agentViewButton->setToolTip(
            forkmesh::ui::listViewModeTooltip(view, description));
    }
    if (!m_agentTable)
        return;
    m_agentTable->setColumnHidden(kAgentIdColumn, view != AgentListView::Compact);
    const int rowHeight =
        view == AgentListView::Full
            ? m_agentCompactRowHeight + m_agentTable->fontMetrics().height() + 4
            : m_agentCompactRowHeight;
    if (rowHeight > 0) {
        m_agentTable->verticalHeader()->setDefaultSectionSize(rowHeight);
        for (int row = 0; row < m_agentTable->rowCount(); ++row)
            m_agentTable->setRowHeight(row, rowHeight);
    }
    refreshAgentTable();
}

QPixmap MainWindow::agentSessionPullAvatar(const AgentSession &session)
{
    if (session.prNumber <= 0)
        return QPixmap();
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        if (pr.number != session.prNumber)
            continue;
        // Every branch below is a pure function of the inputs named in the
        // key: the cached author pixmap (its cacheKey changes with its data),
        // the profile avatar bytes, or the author text the placeholder is
        // synthesised from. The animation timer asks for this per running
        // row per tick, so memoize; bounded because pulls and authors are.
        enum class Source { CachedPixmap, OwnAvatar, Generated };
        Source source = Source::Generated;
        QString key;
        const QPixmap cached = m_avatars.value(pr.author);
        QString generatedFrom;
        if (!cached.isNull()) {
            source = Source::CachedPixmap;
            key = QStringLiteral("px|%1|%2|%3")
                      .arg(pr.number)
                      .arg(pr.author)
                      .arg(cached.cacheKey());
        } else if (!pr.author.isEmpty() &&
                   pr.author == m_profileIdentity.publicKey()) {
            source = Source::OwnAvatar;
            // effectiveUserAvatar() synthesises a PNG when no avatar is set,
            // so key on its inputs and only call it on a miss.
            key = QStringLiteral("own|%1|%2")
                      .arg(pr.number)
                      .arg(m_userAvatar.isEmpty()
                               ? QStringLiteral("seed:") + topBarUserName() +
                                     QLatin1Char('|') + m_profileIdentity.publicKey()
                               : QString::fromLatin1(
                                     QCryptographicHash::hash(
                                         m_userAvatar, QCryptographicHash::Sha1)
                                         .toHex()));
        } else {
            const QString author = pr.authorName.trimmed().isEmpty()
                                       ? (pr.author.isEmpty()
                                              ? QString::number(pr.number)
                                              : pr.author)
                                       : pr.authorName.trimmed();
            generatedFrom = author.toLower();
            key = QStringLiteral("gen|%1").arg(generatedFrom);
        }
        key += QLatin1Char('|') + QString::number(iconDevicePixelRatio());
        const auto hit = m_agentPullAvatarCache.constFind(key);
        if (hit != m_agentPullAvatarCache.cend())
            return *hit;
        QPixmap out;
        switch (source) {
        case Source::CachedPixmap:
            out = roundedRectPixmap(cached, kAgentIdentityArtworkPx,
                                    kAgentIdentityArtworkPx / 2.0);
            break;
        case Source::OwnAvatar:
            out = roundedAvatar(effectiveUserAvatar(), kAgentIdentityArtworkPx, 0.5);
            break;
        case Source::Generated:
            out = roundedAvatar(forkMeshAvatarPng(generatedFrom),
                                kAgentIdentityArtworkPx, 0.5);
            break;
        }
        if (m_agentPullAvatarCache.size() > 512)
            m_agentPullAvatarCache.clear();
        m_agentPullAvatarCache.insert(key, out);
        return out;
    }
    return QPixmap();
}

void MainWindow::scheduleAgentSessionSave(int sessionId)
{
    if (!m_agentStore || sessionId <= 0)
        return;
    m_pendingAgentSessionSaves.insert(sessionId);
    if (!m_agentSessionSaveTimer) {
        m_agentSessionSaveTimer = new QTimer(this);
        m_agentSessionSaveTimer->setSingleShot(true);
        m_agentSessionSaveTimer->setInterval(400);
        connect(m_agentSessionSaveTimer, &QTimer::timeout, this,
                &MainWindow::flushPendingAgentSessionSaves);
    }
    if (!m_agentSessionSaveTimer->isActive())
        m_agentSessionSaveTimer->start();
}

void MainWindow::flushPendingAgentSessionSaves()
{
    if (m_agentSessionSaveTimer)
        m_agentSessionSaveTimer->stop();
    const QSet<int> pending = std::exchange(m_pendingAgentSessionSaves, {});
    if (!m_agentStore)
        return;
    for (const int sessionId : pending) {
        if (isExternalSession(sessionId))
            continue;
        if (const AgentSession *session = findAgentSession(sessionId))
            m_agentStore->saveSession(*session);
    }
}

// The same face, for the notification the desktop itself puts on screen:
// notify-send takes a file path as readily as a theme icon name, so the
// medallion is unpacked out of the binary's resources once per model and the
// cached copy is handed over from then on.
QString MainWindow::agentNotificationIconPath(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return QString();
    const QString cache =
        forkmesh::paths::cacheRoot();
    if (cache.isEmpty())
        return QString();
    const QString dir = cache + QStringLiteral("/agent-faces");
    if (!QDir().mkpath(dir))
        return QString();
    const QByteArray identityHash = QCryptographicHash::hash(
                                        (session->provider + QLatin1Char('\n') +
                                         session->model)
                                            .toUtf8(),
                                        QCryptographicHash::Sha256)
                                        .toHex()
                                        .left(16);
    const QString path = QStringLiteral("%1/model-%2.png")
                             .arg(dir, QString::fromLatin1(identityHash));
    if (QFileInfo::exists(path))
        return path;
    const QPixmap art = agentStatusModelIcon(*session).pixmap(
        QSize(kAgentNotificationIconPx, kAgentNotificationIconPx), 1.0);
    if (art.isNull() || !art.save(path, "PNG"))
        return QString();
    return path;
}

AgentSession *MainWindow::findAgentSession(int sessionId)
{
    for (AgentSession &session : m_agentSessions)
        if (session.id == sessionId)
            return &session;
    return nullptr;
}

void MainWindow::loadAgentReadState()
{
    QSettings settings;
    m_agentReadBaselineMs =
        settings.value(kAgentReadBaselineSetting).toLongLong();
    if (m_agentReadBaselineMs <= 0) {
        m_agentReadBaselineMs = QDateTime::currentMSecsSinceEpoch();
        settings.setValue(kAgentReadBaselineSetting, m_agentReadBaselineMs);
    }
    m_agentSeenFinishedAt.clear();
    const QVariantMap seen = settings.value(kAgentSeenFinishedAtSetting).toMap();
    for (auto it = seen.cbegin(); it != seen.cend(); ++it)
        m_agentSeenFinishedAt.insert(it.key(), it.value().toLongLong());
}

QString MainWindow::agentReadKey(const AgentSession &session) const
{
    if (session.cloud || isCloudSession(session.id))
        return session.cloudSessionId.isEmpty()
                   ? QString()
                   : QStringLiteral("cloud:") + session.cloudSessionId;
    if (isExternalSession(session.id)) {
        const auto it = m_externalSurfaced.constFind(session.id);
        return it == m_externalSurfaced.cend() || it->uuid.isEmpty()
                   ? QString()
                   : QStringLiteral("claude:") + it->uuid;
    }
    return session.id > 0 ? QString::number(session.id) : QString();
}

bool MainWindow::agentSessionUnread(const AgentSession &session) const
{
    if (session.finishedAtMs <= 0 || session.merged || session.deletedAtMs > 0 ||
        session.associationOnly || session.runInFlight() ||
        session.status == AgentStatus::Queued)
        return false;
    const QString key = agentReadKey(session);
    if (key.isEmpty())
        return false;
    return session.finishedAtMs >
           qMax(m_agentReadBaselineMs, m_agentSeenFinishedAt.value(key));
}

bool MainWindow::agentSessionWatched(int sessionId) const
{
    return sessionId != 0 && sessionId == m_selectedAgentSessionId &&
           m_agentTable && m_agentTable->isVisible() && isActiveWindow();
}

bool MainWindow::noteAgentSessionSeen(const AgentSession &session)
{
    if (!agentSessionUnread(session))
        return false;
    m_agentSeenFinishedAt.insert(agentReadKey(session), session.finishedAtMs);
    if (m_agentSeenFinishedAt.size() > 400) {
        QSet<QString> live;
        for (const AgentSession &s : std::as_const(m_agentSessions))
            live.insert(agentReadKey(s));
        for (auto it = m_agentSeenFinishedAt.begin();
             it != m_agentSeenFinishedAt.end();) {
            if (!live.contains(it.key()))
                it = m_agentSeenFinishedAt.erase(it);
            else
                ++it;
        }
    }
    QVariantMap seen;
    for (auto it = m_agentSeenFinishedAt.cbegin();
         it != m_agentSeenFinishedAt.cend(); ++it)
        seen.insert(it.key(), it.value());
    QSettings().setValue(kAgentSeenFinishedAtSetting, seen);
    return true;
}

void MainWindow::markAgentSessionRead(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || !noteAgentSessionSeen(*session))
        return;
    if (m_agentTable) {
        for (int row = 0; row < m_agentTable->rowCount(); ++row) {
            QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
            if (!lead || lead->data(Qt::UserRole).toInt() != sessionId)
                continue;
            if (QTableWidgetItem *title = m_agentTable->item(row, kAgentIssueColumn))
                title->setData(kAgentUnreadRole, false);
            break;
        }
    }
    notifyLocalControlAgentsChanged();
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testTypeGlobalSearch(const QString &text)
{
    if (m_globalSearch)
        m_globalSearch->setText(text);
}

QString MainWindow::testAgentSearchText() const
{
    return agentFilterQuery();
}

QString MainWindow::testTranscriptSearchText() const
{
    return m_transcriptSearch ? m_transcriptSearch->text() : QString();
}

bool MainWindow::testGlobalSearchClearVisible() const
{
    return m_globalSearchClear && m_globalSearchClear->isVisible();
}

void MainWindow::testTriggerGlobalSearchClear()
{
    if (m_globalSearchClear)
        m_globalSearchClear->trigger();
}

QString MainWindow::testAgentSearchEmptyText() const
{
    return m_agentSearchEmpty && !m_agentSearchEmpty->isHidden()
               ? m_agentSearchEmpty->text()
               : QString();
}

void MainWindow::testActivateAgentSearchEmptyLink()
{
    if (m_agentSearchEmpty && !m_agentSearchEmpty->isHidden())
        Q_EMIT m_agentSearchEmpty->linkActivated(kClearSearchLink);
}

QStringList MainWindow::testAgentRowTitles() const
{
    QStringList titles;
    if (!m_agentTable)
        return titles;
    for (int row = 0; row < m_agentTable->rowCount(); ++row)
        if (QTableWidgetItem *cell = m_agentTable->item(row, kAgentIssueColumn);
            cell && !cell->data(kAgentFolderRole).isValid())
            titles << cell->text();
    return titles;
}

QStringList MainWindow::testAgentRowChipWords(int sessionId) const
{
    QStringList words;
    if (!m_agentTable)
        return words;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
        const QTableWidgetItem *cell = m_agentTable->item(row, kAgentIssueColumn);
        if (!lead || !cell || lead->data(Qt::UserRole).toInt() != sessionId)
            continue;
        for (const QVariant &chip : cell->data(kAgentRowChipsRole).toList())
            words << chip.toStringList().value(2);
    }
    return words;
}

bool MainWindow::testAgentLeadColumnHidden() const
{
    return m_agentTable && m_agentTable->isColumnHidden(kAgentIdColumn);
}

QImage MainWindow::testGrabAgentTitleRow(int sessionId)
{
    QWidget *viewport = m_agentTable ? m_agentTable->viewport() : nullptr;
    if (!viewport)
        return QImage();
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
        const QTableWidgetItem *cell = m_agentTable->item(row, kAgentIssueColumn);
        if (!lead || !cell || lead->data(Qt::UserRole).toInt() != sessionId)
            continue;
        const QRect band = m_agentTable->visualItemRect(cell);
        viewport->repaint();
        return viewport
            ->grab(QRect(0, band.top(), viewport->width(), band.height()))
            .toImage();
    }
    return QImage();
}

bool MainWindow::testAgentRowUnread(int sessionId) const
{
    if (!m_agentTable)
        return false;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
        if (!lead || lead->data(Qt::UserRole).toInt() != sessionId)
            continue;
        const QTableWidgetItem *title = m_agentTable->item(row, kAgentIssueColumn);
        return title && title->data(kAgentUnreadRole).toBool();
    }
    return false;
}

QStringList MainWindow::testAgentRowImages(int sessionId) const
{
    if (!m_agentTable)
        return {};
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *cell = m_agentTable->item(row, kAgentIssueColumn);
        if (cell && cell->data(kAgentImagesSessionRole).toInt() == sessionId)
            return cell->data(kAgentImagesRole).toStringList();
    }
    return {};
}

bool MainWindow::testAgentRowHasThumbnail(int sessionId) const
{
    if (!m_agentTable)
        return false;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *cell = m_agentTable->item(row, kAgentIssueColumn);
        if (cell && cell->data(kAgentImagesSessionRole).toInt() == sessionId)
            return !cell->icon().isNull();
    }
    return false;
}

QString MainWindow::testAgentDetailTitleText() const
{
    if (const auto *wrapped = dynamic_cast<const WrappedTitleLabel *>(m_agentTitle))
        return wrapped->fullText();
    return m_agentTitle ? m_agentTitle->text() : QString();
}

bool MainWindow::testAgentDetailTitleWraps() const
{
    return m_agentTitle && m_agentTitle->wordWrap();
}

QString MainWindow::testRenderAgentDetailTitle(const QString &text)
{
    setAgentTitleText(m_agentTitle, text);
    return testAgentDetailTitleText();
}

QString MainWindow::testAgentStatusCellText(int sessionId) const
{
    for (const AgentSession &s : m_agentSessions)
        if (s.id == sessionId)
            return agentStatusLabel(s);
    return QString();
}

QString MainWindow::testAgentStatusCellBadges(int sessionId,
                                              const AgentDiffStat &stat) const
{
    for (const AgentSession &s : m_agentSessions) {
        if (s.id != sessionId)
            continue;
        QTableWidgetItem item;
        applyAgentStatusCell(&item, s, stat);
        return QStringLiteral("%1|%2|%3|%4|%5")
            .arg(item.data(kAgentBranchFilesRole).toInt())
            .arg(item.data(kAgentBranchDirtyRole).toInt())
            .arg(item.data(kAgentBranchWorktreeRole).toString())
            .arg(item.data(kAgentBehindRole).toInt())
            .arg(item.data(kAgentAheadRole).toInt());
    }
    return QString();
}

QString MainWindow::testAgentStatusCellToolTip(int sessionId,
                                               const AgentDiffStat &stat) const
{
    for (const AgentSession &s : m_agentSessions) {
        if (s.id != sessionId)
            continue;
        QTableWidgetItem item;
        applyAgentStatusCell(&item, s, stat, agentMergeBase(s), s.id, QPixmap(),
                             0.0, agentRunToggleMode(s));
        return item.toolTip();
    }
    return QString();
}

int MainWindow::testAgentRunToggleMode(int sessionId) const
{
    if (!m_agentTable)
        return -1;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *cell = m_agentTable->item(row, kAgentIdColumn);
        if (cell && cell->data(Qt::UserRole).toInt() == sessionId)
            return cell->data(kAgentRunToggleRole).toInt();
    }
    return -1;
}

// The row's lead glyph — the avatar, the teammate badge on its corner and the
// status mark — in viewport coordinates, measured the way the delegate measures
// the decoration the style lays out for it.
QRect MainWindow::testAgentRowLeadRect(int sessionId)
{
    if (!m_agentTable)
        return QRect();
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        QTableWidgetItem *cell = m_agentTable->item(row, kAgentIdColumn);
        if (!cell || cell->data(Qt::UserRole).toInt() != sessionId)
            continue;
        m_agentTable->scrollToItem(cell, QAbstractItemView::PositionAtCenter);
        const QRect rect = m_agentTable->visualItemRect(cell);
        if (rect.isEmpty())
            return QRect();
        QStyleOptionViewItem opt;
        opt.initFrom(m_agentTable);
        opt.rect = rect;
        opt.widget = m_agentTable;
        opt.decorationSize = m_agentTable->iconSize();
        opt.features |= QStyleOptionViewItem::HasDecoration;
        opt.icon = cell->icon();
        const QRect deco = m_agentTable->style()->subElementRect(
            QStyle::SE_ItemViewItemDecoration, &opt, m_agentTable);
        if (deco.isValid() && deco.width() >= kAgentLeadGlyphsPx)
            return deco;
        return QRect(rect.left(), rect.center().y() - kAgentIdentityCirclePx / 2,
                     kAgentLeadGlyphsPx, kAgentIdentityCirclePx);
    }
    return QRect();
}

// The whole row band, controls included, for eyeballing what the list looks
// like: FORKMESH_AGENT_ROWS_SHOT in the window tests writes it out.
QImage MainWindow::testGrabAgentRow(int sessionId)
{
    const QRect lead = testAgentRowLeadRect(sessionId);
    QWidget *viewport = m_agentTable ? m_agentTable->viewport() : nullptr;
    if (lead.isEmpty() || !viewport)
        return QImage();
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *cell = m_agentTable->item(row, kAgentIdColumn);
        if (!cell || cell->data(Qt::UserRole).toInt() != sessionId)
            continue;
        const QRect band = m_agentTable->visualItemRect(cell);
        viewport->repaint();
        return viewport
            ->grab(QRect(0, band.top(), viewport->width(), band.height()))
            .toImage();
    }
    return QImage();
}

QImage MainWindow::testGrabAgentRowLead(int sessionId)
{
    const QRect lead = testAgentRowLeadRect(sessionId);
    if (lead.isEmpty() || !m_agentTable || !m_agentTable->viewport())
        return QImage();
    m_agentTable->viewport()->repaint();
    return m_agentTable->viewport()->grab(lead).toImage();
}

// Click the status mark itself — where the run control used to live. Nothing
// answers there now, and the test that says so needs the click to have landed.
bool MainWindow::testClickAgentRowLeadGlyph(int sessionId)
{
    const QRect lead = testAgentRowLeadRect(sessionId);
    if (lead.isEmpty())
        return false;
    const QPointF pos(lead.right() - kAgentStatusGlyphPx / 2.0, lead.center().y());
    const QModelIndex hit = m_agentTable->indexAt(pos.toPoint());
    if (hit.column() != kAgentIdColumn)
        return false;
    QWidget *viewport = m_agentTable->viewport();
    const QPointF global = viewport->mapToGlobal(pos);
    QMouseEvent move(QEvent::MouseMove, pos, global, Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, global, Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
    QApplication::processEvents();
    return true;
}

// Hover the row the way a pointer would and let its controls finish fading in,
// then hand back the cell they are drawn in. Returns a null rect when the row
// is not there.
QRect MainWindow::testRevealAgentRowActions(int sessionId)
{
    if (!m_agentTable)
        return QRect();
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        QTableWidgetItem *cell = m_agentTable->item(row, kAgentIdColumn);
        QTableWidgetItem *issue = m_agentTable->item(row, kAgentIssueColumn);
        if (!cell || !issue || cell->data(Qt::UserRole).toInt() != sessionId)
            continue;
        // A row parked below the fold has a rect the pointer could never
        // reach, and a click there would land on nothing.
        m_agentTable->scrollToItem(cell, QAbstractItemView::PositionAtCenter);
        const QRect strip = m_agentTable->visualItemRect(issue);
        if (strip.isEmpty())
            return QRect();
        QWidget *viewport = m_agentTable->viewport();
        // Point at the row's *lead* column: the controls belong to the agent,
        // so hovering it anywhere has to bring them out, and a reveal that
        // read State_MouseOver alone would leave them a hidden hotspot.
        const QPointF over(m_agentTable->visualItemRect(cell).center());
        QMouseEvent move(QEvent::MouseMove, over, viewport->mapToGlobal(over),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        viewport->repaint();
        // The controls fade in over a few frames, so a click or a grab taken on
        // the frame the pointer lands would still find bare row. Let the
        // delegate's reveal timer run itself out first. Installed on this
        // column and nowhere else, in this same file.
        auto *actions = static_cast<AgentRowActionsDelegate *>(
            m_agentTable->itemDelegateForColumn(kAgentIssueColumn));
        for (int spin = 0; spin < 60 && actions && actions->revealing(); ++spin) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            viewport->repaint();
        }
        return strip;
    }
    return QRect();
}

bool MainWindow::testClickAgentRowAction(int sessionId, int action)
{
    const QRect strip = testRevealAgentRowActions(sessionId);
    if (strip.isEmpty())
        return false;
    QRect target = AgentRowActionsDelegate::runToggleButtonRect(strip);
    if (action == 1)
        target = AgentRowActionsDelegate::deleteButtonRect(strip);
    else if (action == 2)
        target = AgentRowActionsDelegate::moveToTopButtonRect(strip);
    else if (action == 3)
        target = AgentRowActionsDelegate::moveToBottomButtonRect(strip);
    const QPointF pos(target.center());
    // A point that does not resolve back to this row's last column would
    // silently click on nothing, and the test would read as a broken control.
    const QModelIndex hit = m_agentTable->indexAt(pos.toPoint());
    const QTableWidgetItem *lead =
        hit.isValid() ? m_agentTable->item(hit.row(), kAgentIdColumn) : nullptr;
    if (hit.column() != kAgentIssueColumn || !lead ||
        lead->data(Qt::UserRole).toInt() != sessionId)
        return false;
    QWidget *viewport = m_agentTable->viewport();
    const QPointF global = viewport->mapToGlobal(pos);
    QMouseEvent move(QEvent::MouseMove, pos, global, Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, global, Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
    // Both controls hand their work to the event loop so the click can finish
    // first; run that hand-off out before returning.
    QApplication::processEvents();
    return true;
}

bool MainWindow::testClickAgentRunToggle(int sessionId)
{
    return testClickAgentRowAction(sessionId, /*action=*/0);
}

bool MainWindow::testClickAgentRowDelete(int sessionId)
{
    return testClickAgentRowAction(sessionId, /*action=*/1);
}

bool MainWindow::testClickAgentRowMoveToTop(int sessionId)
{
    return testClickAgentRowAction(sessionId, /*action=*/2);
}

bool MainWindow::testClickAgentRowMoveToBottom(int sessionId)
{
    return testClickAgentRowAction(sessionId, /*action=*/3);
}

bool MainWindow::testAgentRowRevealsRunToggleOnHover(int sessionId)
{
    if (!m_agentTable)
        return false;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        QTableWidgetItem *cell = m_agentTable->item(row, kAgentIdColumn);
        QTableWidgetItem *issue = m_agentTable->item(row, kAgentIssueColumn);
        if (!cell || !issue || cell->data(Qt::UserRole).toInt() != sessionId)
            continue;
        m_agentTable->scrollToItem(cell, QAbstractItemView::PositionAtCenter);
        QWidget *viewport = m_agentTable->viewport();
        const QRect strip = m_agentTable->visualItemRect(issue);
        if (strip.isEmpty())
            return false;
        // The end of the row, where both controls are drawn.
        const QRect slot = AgentRowActionsDelegate::runToggleButtonRect(strip)
                               .united(AgentRowActionsDelegate::deleteButtonRect(
                                   strip));
        // Park the pointer off this row first, so the "cold" grab is a row
        // nobody is pointing at.
        const QPointF away(viewport->width() - 1, viewport->height() - 1);
        QMouseEvent leave(QEvent::MouseMove, away, viewport->mapToGlobal(away),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &leave);
        viewport->repaint();
        auto *actions = static_cast<AgentRowActionsDelegate *>(
            m_agentTable->itemDelegateForColumn(kAgentIssueColumn));
        for (int spin = 0; spin < 60 && actions && actions->revealing(); ++spin) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            viewport->repaint();
        }
        const QImage cold = viewport->grab(slot).toImage();
        if (testRevealAgentRowActions(sessionId).isEmpty())
            return false;
        const QImage hot = viewport->grab(slot).toImage();
        return !cold.isNull() && !hot.isNull() && cold != hot;
    }
    return false;
}

QString MainWindow::testAgentRowTeammate(int sessionId) const
{
    for (const AgentSession &s : m_agentSessions)
        if (s.id == sessionId)
            return s.hasTeammateAttribution() ? s.teammateName : QString();
    return QString();
}

bool MainWindow::testAgentSessionMerged(int sessionId) const
{
    for (const AgentSession &s : m_agentSessions)
        if (s.id == sessionId)
            return s.merged;
    return false;
}
#endif

const AgentSession *MainWindow::latestAgentSessionForIssue(int issueNumber) const
{
    if (issueNumber <= 0)
        return nullptr;
    const int idx = issuesRepoIndex();
    if (idx < 0 || idx >= m_repositories.size())
        return nullptr;
    const RepositoryRecord &repo = m_repositories.at(idx);
    for (const Issue &issue : m_currentIssues) {
        if (issue.number != issueNumber)
            continue;
        for (auto it = issue.events.crbegin(); it != issue.events.crend(); ++it) {
            if (it->type != QLatin1String("agent"))
                continue;
            if (it->agentSessionId <= 0 || it->agentStatus == AgentStatus::Cleared)
                return nullptr;
            for (const AgentSession &session : m_agentSessions) {
                if (session.id == it->agentSessionId && session.owner == repo.owner &&
                    session.name == repo.name && session.issueNumber == issueNumber)
                    return &session;
            }
            return nullptr;
        }
        break;
    }
    for (const AgentSession &session : m_agentSessions) {
        if (session.owner == repo.owner && session.name == repo.name &&
            session.issueNumber == issueNumber)
            return &session;
    }
    return nullptr;
}

const AgentSession *MainWindow::agentSessionForPull(int prNumber,
                                                    const QString &headBranch) const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return nullptr;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (prNumber > 0) {
        for (const AgentSession &session : m_agentSessions) {
            if (session.prNumber == prNumber && session.owner == repo.owner &&
                session.name == repo.name)
                return &session;
        }
    }
    const QString branch =
        headBranch.section(QLatin1Char(':'), -1, -1).trimmed();
    if (!branch.isEmpty() && m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size()) {
        for (auto it = m_agentSessions.crbegin(); it != m_agentSessions.crend();
             ++it) {
            if (it->branchName == branch && it->owner == repo.owner &&
                it->name == repo.name &&
                (it->prNumber == 0 || it->prNumber == prNumber))
                return &*it;
        }
    }
    return nullptr;
}

bool MainWindow::bindAgentSessionsToPull(int prNumber,
                                         const QString &headBranch)
{
    if (!m_agentStore || prNumber <= 0 || headBranch.trimmed().isEmpty() ||
        m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return false;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    bool changed = false;
    bool linked = false;
    for (AgentSession &session : m_agentSessions) {
        if (session.owner != repo.owner || session.name != repo.name)
            continue;
        if (session.prNumber == prNumber) {
            linked = true;
            continue;
        }
        if (session.branchName != headBranch)
            continue;
        if (session.prNumber > 0)
            continue;
        session.prNumber = prNumber;
        m_agentStore->saveSession(session);
        m_agentStore->appendLog(
            session,
            QStringLiteral("\n==> Branch %1 linked to PR #%2.")
                .arg(headBranch)
                .arg(prNumber));
        linked = true;
        changed = true;
    }
    if (!linked) {
        AgentSession association;
        association.owner = repo.owner;
        association.name = repo.name;
        association.issueTitle =
            QStringLiteral("PR #%1 branch association").arg(prNumber);
        association.prompt =
            QStringLiteral("Track branch %1 and PR #%2 as durable provenance. "
                           "No Agent code run was performed.")
                .arg(headBranch)
                .arg(prNumber);
        association.associationOnly = true;
        association.originInstanceId = activeServerInstanceId();
        association.originInstanceUrl = activeServerInstanceUrl();
        association.prNumber = prNumber;
        association.status = AgentStatus::Success;
        association.branchName = headBranch;
        association.baseBranch = repoDefaultBranch(repoBranches());
        association.finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        association = m_agentStore->createSession(association);
        m_agentStore->appendLog(
            association,
            QStringLiteral("Branch %1 linked to PR #%2. This is a provenance-only "
                           "record; no Agent code run was performed.")
                .arg(headBranch)
                .arg(prNumber));
        m_agentSessions.append(association);
        changed = true;
    }
    if (changed)
        scheduleAgentSessionsPush();
    return changed;
}

AgentDiffStat MainWindow::agentDiffStat(const AgentSession &session,
                                        const QString &gitDir, const QString &base)
{
    Q_UNUSED(gitDir);
    Q_UNUSED(base);
    auto cached = m_agentDiffStats.constFind(session.id);
    if (cached != m_agentDiffStats.constEnd())
        return cached.value();
    return AgentDiffStat();
}

bool MainWindow::markAgentSessionsMerged(int prNumber, const QString &branch,
                                         bool mergeVerified)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return false;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    return markRepoAgentSessionsMerged(repo.owner, repo.name, prNumber, branch,
                                       mergeVerified);
}

bool MainWindow::markRepoAgentSessionsMerged(const QString &owner,
                                             const QString &name, int prNumber,
                                             const QString &branch,
                                             bool mergeVerified)
{
    if (!mergeVerified || !m_agentStore)
        return false;
    m_agentDiffRefreshPending = true;
    bool changed = false;
    QList<int> mergedIds;
    for (AgentSession &s : m_agentSessions) {
        if (s.merged || s.owner != owner || s.name != name)
            continue;
        const bool byPr = prNumber > 0 && s.prNumber == prNumber;
        const bool byBranch =
            !branch.isEmpty() && !s.branchName.isEmpty() && s.branchName == branch;
        if (!byPr && !byBranch)
            continue;
        s.merged = true;
        s.mergeAttested = true; // ForkMesh just performed this merge itself
        s.mergedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_agentStore->saveSession(s);
        m_agentStore->appendLog(
            s, QStringLiteral("\n==> %1 merged into %2.")
                   .arg(byPr ? QStringLiteral("PR #%1").arg(prNumber)
                             : QStringLiteral("Branch %1").arg(branch),
                        agentMergeBase(s)));
        mergedIds << s.id;
        changed = true;
    }
    for (int id : mergedIds)
        completeOrgTaskForSession(id, QStringLiteral("The work has been merged."));
    if (deferUiRefresh(UiRefreshAgentState))
        return changed;
    if (changed) {
        refreshAgentTable();
        refreshAgentModelSelectors();
        if (m_selectedAgentSessionId > 0)
            showAgentSession(m_selectedAgentSessionId);
    } else
        refreshAgentTable();
    return changed;
}

void MainWindow::refreshAgentMergeState()
{
    if (!m_agentStore)
        return;
    if (m_agentMergeStateRefreshing)
        return;
    QString owner, name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }
    const QString dir = repoGitDir();
    const QString configuredBase = m_repoInfo.defaultBranch;
    QString base = configuredBase.trimmed();
    if (base.isEmpty() && m_branchesCacheDir == dir) {
        if (m_branchesCache.contains(QStringLiteral("main")))
            base = QStringLiteral("main");
        else if (m_branchesCache.contains(QStringLiteral("master")))
            base = QStringLiteral("master");
    }
    if (base.isEmpty())
        base = m_repoBranch;
    struct Candidate {
        int id;
        QString branch;
        QString mergeHead;
        QString mergeBase; // the base the session's own merge claim is about
        bool recheckMerged; // a merged verdict this sweep inferred, not attested
    };
    QList<Candidate> candidates;
    QList<int> mergedFromPulls;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.owner != owner || s.name != name)
            continue;
        // An attested merge is settled. An inferred one is not: older builds
        // read a branch that had merely caught up with main as merged work, and
        // a session that was only ever stopped must not keep that badge.
        if (s.merged && s.mergeAttested)
            continue;
        if (s.status == AgentStatus::Queued || s.status == AgentStatus::Running)
            continue;
        if (s.prNumber > 0) {
            const PullRequest *pr = nullptr;
            for (const PullRequest &p : m_currentPulls)
                if (p.number == s.prNumber) {
                    pr = &p;
                    break;
                }
            if (pr) {
                if (pr->status == QLatin1String("merged"))
                    mergedFromPulls.append(s.id);
                continue; // an open/closed PR settles it; no git fallback needed
            }
        }
        if (dir.isEmpty())
            continue;
        if (s.mergeCandidateHead.isEmpty() && s.branchName.isEmpty())
            continue;
        if (!base.isEmpty() && s.mergeCandidateHead.isEmpty() &&
            s.branchName == base)
            continue;
        candidates.append({s.id, s.branchName, s.mergeCandidateHead,
                           agentMergeBase(s), s.merged});
    }
    markAgentSessionsLanded(mergedFromPulls, /*refreshUi=*/false,
                            /*attested=*/true);
    if (candidates.isEmpty())
        return;
    m_agentMergeStateRefreshing = true;
    auto landed = std::make_shared<QList<int>>();
    auto withdrawn = std::make_shared<QList<int>>();
    auto observedHeads = std::make_shared<QHash<int, QString>>();
    const QString checkedOut = m_repoBranch;
    QThread *worker = QThread::create(
        [dir, base, configuredBase, checkedOut, candidates, landed, withdrawn,
         observedHeads]() {
        const forkmesh::BackgroundScope activity(
            QStringLiteral("agents"),
            QStringLiteral("detect branches landed in base"),
            forkmesh::ActionTelemetry::Execution::Worker);
        const QString resolvedBase =
            base.isEmpty()
                ? backgroundDefaultBranch(dir, configuredBase, checkedOut)
                : base;
        if (resolvedBase.isEmpty())
            return;
        // Every candidate's branch tip from one for-each-ref instead of a
        // rev-parse process each. A branch that is gone is simply absent here,
        // which is the same answer the per-candidate probe gave: a deleted
        // branch leaves Git with nothing to say about a merge either way, and
        // the loop below still gets to tell that apart from a live tip.
        const QHash<QString, QString> branchTips = backgroundBranchTips(dir);
        // resolvedBase is fixed for this sweep, so "has this commit landed in
        // base" is a pure function of the commit. Agent branches routinely
        // share a tip — a fleet parked on one commit made most of these probes
        // byte-identical duplicates — so ask Git once per distinct commit.
        // Sweep-scoped by design: it also means every candidate is judged
        // against one snapshot of base rather than a tip that can move
        // mid-loop.
        // Sweep-scoped was not enough: the sweep itself repeats, and it re-ran
        // the identical probes every time — 184 spawns for 17 distinct
        // questions in ten minutes, 24 repeats of each commit (actions.jsonl
        // 2026-09-10), and 22 of the UI stalls logged over 2026-09-01..10.
        // Ancestry is a pure function of the two object names, so memoise it
        // against base's *resolved tip* rather than its name: when base moves
        // (or is rewritten) the tip changes and every entry is recomputed.
        const QString baseTip = branchTips.value(resolvedBase);
        static QHash<QString, bool> landedAcrossSweeps;
        static QMutex landedMutex;
        QHash<QString, bool> landedInBase;
        const auto commitLandedInBase = [&](const QString &commit) {
            if (commit.isEmpty())
                return false;
            const auto known = landedInBase.constFind(commit);
            if (known != landedInBase.constEnd())
                return known.value();
            const QString durableKey =
                baseTip.isEmpty() ? QString()
                                  : dir + QLatin1Char('\x1f') + commit +
                                        QLatin1Char('\x1f') + baseTip;
            if (!durableKey.isEmpty()) {
                QMutexLocker lock(&landedMutex);
                const auto seen = landedAcrossSweeps.constFind(durableKey);
                if (seen != landedAcrossSweeps.constEnd()) {
                    const bool landed = seen.value();
                    lock.unlock();
                    landedInBase.insert(commit, landed);
                    return landed;
                }
            }
            const bool landed =
                agentCommitLandedInBase(dir, commit, resolvedBase);
            landedInBase.insert(commit, landed);
            if (!durableKey.isEmpty()) {
                QMutexLocker lock(&landedMutex);
                if (landedAcrossSweeps.size() > 4096)
                    landedAcrossSweeps.clear();
                landedAcrossSweeps.insert(durableKey, landed);
            }
            return landed;
        };
        for (const Candidate &c : candidates) {
            const QString head = branchTips.value(c.branch);
            const bool outsideBase = !head.isEmpty() && !commitLandedInBase(head);
            if (outsideBase) {
                observedHeads->insert(c.id, head);
                if (!c.recheckMerged)
                    continue; // still ahead of base: nothing has landed yet
            }
            // A merged verdict Git can neither confirm nor deny is left alone:
            // a deleted branch is exactly what a merge-and-clean-up leaves
            // behind, and another base's history has no say over this one.
            if (c.recheckMerged &&
                (head.isEmpty() || c.mergeBase != resolvedBase))
                continue;
            // The only proof a session's own work reached base: a tip we saw
            // outside base while it was still outside, and that base now holds.
            const bool tipLanded =
                !c.mergeHead.isEmpty() && commitLandedInBase(c.mergeHead);
            if (!tipLanded && c.recheckMerged)
                withdrawn->append(c.id);
            else if (tipLanded && !c.recheckMerged)
                landed->append(c.id);
        }
    });
    connect(worker, &QThread::finished, this,
            [this, worker, landed, withdrawn, observedHeads]() {
        m_agentMergeStateRefreshing = false;
        worker->deleteLater();
        // Withdraw first: updateAgentMergeCandidates() ignores sessions that
        // still carry the badge, and a withdrawn one needs its tip recorded so
        // a later sweep can see that tip actually land.
        withdrawAgentSessionMerges(*withdrawn);
        updateAgentMergeCandidates(*observedHeads);
        markAgentSessionsLanded(*landed, /*refreshUi=*/true);
    });
    worker->start();
}

void MainWindow::updateAgentMergeCandidates(const QHash<int, QString> &heads)
{
    if (!m_agentStore || heads.isEmpty())
        return;
    for (AgentSession &s : m_agentSessions) {
        const auto it = heads.constFind(s.id);
        if (it == heads.constEnd() || s.merged ||
            s.status == AgentStatus::Queued || s.status == AgentStatus::Running ||
            s.mergeCandidateHead == it.value())
            continue;
        s.mergeCandidateHead = it.value();
        m_agentStore->saveSession(s);
    }
}

void MainWindow::markAgentSessionsLanded(const QList<int> &sessionIds, bool refreshUi,
                                         bool attested)
{
    if (!m_agentStore || sessionIds.isEmpty())
        return;
    bool changed = false;
    QList<int> mergedIds;
    for (AgentSession &s : m_agentSessions) {
        if (!sessionIds.contains(s.id))
            continue;
        if (s.status == AgentStatus::Queued || s.status == AgentStatus::Running)
            continue; // re-run since the sweep snapshotted it; verdict is stale
        if (s.merged) {
            // Already flagged, but a verdict the pull ledger stands behind
            // settles it for good and stops later sweeps re-checking it.
            if (attested && !s.mergeAttested) {
                s.mergeAttested = true;
                m_agentStore->saveSession(s);
            }
            continue;
        }
        s.merged = true;
        s.mergeAttested = attested;
        s.mergedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_agentStore->saveSession(s);
        m_agentStore->appendLog(
            s, QStringLiteral("\n==> Worktree/PR merged into %1.").arg(agentMergeBase(s)));
        mergedIds << s.id;
        changed = true;
    }
    if (!mergedIds.isEmpty())
        danceGenie();
    for (int id : mergedIds)
        completeOrgTaskForSession(id, QStringLiteral("The work has been merged."));
    if (changed && refreshUi) {
        if (!m_agentTableRefreshing)
            m_agentDiffRefreshPending = true;
        refreshAgentTable();
        refreshAgentModelSelectors();
        if (m_selectedAgentSessionId > 0)
            showAgentSession(m_selectedAgentSessionId);
    }
}

// Take back a merged verdict Git no longer supports. Only sweeps guess, and
// only their guesses are withdrawn: a branch that never left its base — one an
// idle session merely had fast-forwarded onto main — landed nothing of its own,
// yet older builds read those base commits as the session's work. The badge
// such a session deserves is the one it actually ended on: stopped, or failed.
void MainWindow::withdrawAgentSessionMerges(const QList<int> &sessionIds)
{
    if (!m_agentStore || sessionIds.isEmpty())
        return;
    bool changed = false;
    for (AgentSession &s : m_agentSessions) {
        if (!sessionIds.contains(s.id) || !s.merged || s.mergeAttested)
            continue;
        if (s.status == AgentStatus::Queued || s.status == AgentStatus::Running)
            continue; // re-run since the sweep snapshotted it; verdict is stale
        s.merged = false;
        s.mergedAtMs = 0;
        m_agentStore->saveSession(s);
        m_agentStore->appendLog(
            s, QStringLiteral("\n==> Not merged after all: %1 has landed nothing "
                              "of its own in %2.")
                   .arg(s.branchName, agentMergeBase(s)));
        changed = true;
    }
    if (!changed)
        return;
    if (!m_agentTableRefreshing)
        m_agentDiffRefreshPending = true;
    refreshAgentTable();
    refreshAgentModelSelectors();
    if (m_selectedAgentSessionId > 0)
        showAgentSession(m_selectedAgentSessionId);
}

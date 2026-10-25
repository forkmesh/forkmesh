
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SecretDiffBanner.h"
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
#include "WorkspaceLayout.h"

using namespace forkmesh::ui;

namespace {

static void buildStreamSideBuffers(const QList<QJsonObject> &events, QString *raw,
                                   QStringList *files)
{
    for (const QJsonObject &ev : events) {
        if (ev.value(QStringLiteral("type")).toString() == QLatin1String("_local_user"))
            continue; // synthetic user turn, never part of the raw CLI stream
        *raw += QString::fromUtf8(QJsonDocument(ev).toJson(QJsonDocument::Compact))
                + QStringLiteral("\n\n");
        if (ev.value(QStringLiteral("type")).toString() != QLatin1String("assistant"))
            continue;
        const QJsonArray content = ev.value(QStringLiteral("message")).toObject()
                                       .value(QStringLiteral("content")).toArray();
        for (const QJsonValue &bv : content) {
            const QJsonObject b = bv.toObject();
            if (b.value(QStringLiteral("type")).toString() != QLatin1String("tool_use"))
                continue;
            const QString name = b.value(QStringLiteral("name")).toString();
            if (name == QLatin1String("Edit") || name == QLatin1String("Write")
                || name == QLatin1String("MultiEdit") || name == QLatin1String("NotebookEdit")) {
                const QString p = b.value(QStringLiteral("input")).toObject()
                                      .value(QStringLiteral("file_path")).toString();
                if (!p.isEmpty() && !files->contains(p))
                    files->append(p);
            } else if (name == QLatin1String("FileChange")) {
                const QJsonArray changes = b.value(QStringLiteral("input")).toObject()
                                               .value(QStringLiteral("changes")).toArray();
                for (const QJsonValue &change : changes) {
                    const QString p =
                        change.toObject().value(QStringLiteral("path")).toString();
                    if (!p.isEmpty() && !files->contains(p))
                        files->append(p);
                }
            }
        }
    }
}

// `git diff` never names a file the index has not seen, so a file the agent just
// created would stay off the Code page until it was committed. Spell each one
// out the way `git diff` shows an added file. Reads the files: off-thread only.
static QString agentUntrackedFilesPatch(const QString &dir, const QStringList &paths)
{
    constexpr qint64 kMaxBytes = 256 * 1024;
    QString out;
    for (const QString &path : paths) {
        QFile file(QDir(dir).filePath(path));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        out += QStringLiteral("diff --git a/") + path + QStringLiteral(" b/") + path +
               QStringLiteral("\nnew file mode 100644\n");
        const bool tooBig = file.size() > kMaxBytes;
        const QByteArray bytes = tooBig ? QByteArray() : file.readAll();
        if (tooBig || bytes.left(8000).contains('\0')) {
            out += QStringLiteral("Binary files /dev/null and b/") + path +
                   QStringLiteral(" differ\n");
            continue;
        }
        if (bytes.isEmpty())
            continue;
        QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
        const bool finalNewline = bytes.endsWith('\n');
        if (finalNewline)
            lines.removeLast();
        out += QStringLiteral("--- /dev/null\n+++ b/") + path +
               QStringLiteral("\n@@ -0,0 +1");
        if (lines.size() != 1)
            out += QLatin1Char(',') + QString::number(lines.size());
        out += QStringLiteral(" @@\n");
        for (const QString &line : lines) {
            out += QLatin1Char('+');
            out += line;
            out += QLatin1Char('\n');
        }
        if (!finalNewline)
            out += QStringLiteral("\\ No newline at end of file\n");
    }
    return out;
}

} // namespace

bool MainWindow::ensureStreamEventsLoadedAsync(int sessionId)
{
    if (m_streamEvents.contains(sessionId))
        return true; // already loaded (or live-streaming)
    if (!m_agentStore || isExternalSession(sessionId) ||
        m_streamEventsAbsent.contains(sessionId) ||
        m_streamEventsLoading.contains(sessionId))
        return false;
    const AgentSession *s = findAgentSession(sessionId);
    if (!s)
        return false;
    m_streamEventsLoading.insert(sessionId);
    struct LoadedEvents {
        QList<QJsonObject> events;
        QString raw;
        QStringList files;
    };
    const AgentSession snapshot = *s;
    AgentStore *store = m_agentStore;
    runOffThread<LoadedEvents>(
        [store, snapshot] {
            LoadedEvents out;
            out.events = store->loadEvents(snapshot);
            buildStreamSideBuffers(out.events, &out.raw, &out.files);
            return out;
        },
        [this, sessionId](LoadedEvents out) {
            m_streamEventsLoading.remove(sessionId);
            if (out.events.isEmpty()) {
                m_streamEventsAbsent.insert(sessionId); // don't re-probe per click
            } else if (!m_streamEvents.contains(sessionId) &&
                       !m_streamSessions.contains(sessionId) &&
                       !m_codexStreams.contains(sessionId)) {
                m_streamEvents[sessionId] = std::move(out.events);
                m_streamRaw[sessionId] = std::move(out.raw);
                m_streamFiles[sessionId] = std::move(out.files);
            }
            if (sessionId == m_selectedAgentSessionId)
                showAgentSession(sessionId); // render the restored history
            if (m_agentQueueAwaitingEvents.contains(sessionId)) {
                const bool quiet = m_agentQueueAwaitingEvents.take(sessionId);
                const bool wasQuiet = m_agentQuietResume;
                m_agentQuietResume = quiet;
                processAgentQueue();
                m_agentQuietResume = wasQuiet;
            }
        });
    return false;
}

void MainWindow::renderTranscriptForSession(int sessionId)
{
    if (!m_agentTranscript)
        return;
    const bool openingSession = m_renderedTranscriptSession != sessionId;
    const bool sameTranscript =
        !openingSession || m_agentTranscriptRenderSession == sessionId;
    const bool rowsLandAbove = m_agentTranscriptScrollRestoreSession == sessionId;
    m_agentTranscriptScrollRestoreSession = -1;
    const quint64 generation = ++m_agentTranscriptRenderGeneration;
    m_agentTranscriptRenderSession = sessionId;
    m_agentTranscriptRenderCursor = 0;
    constexpr int kTranscriptRenderTail = 300;
    const int renderLimit = qMax(
        kTranscriptRenderTail,
        m_agentTranscriptRenderLimits.value(sessionId, kTranscriptRenderTail));
    m_agentTranscriptRenderSkipped =
        qMax(0, int(m_streamEvents.value(sessionId).size()) - renderLimit);
    m_agentTranscriptSkippedNoticeAdded = false;

    // Protect the bottom-follow latch before clear() collapses the old
    // scrollbar range. A newly opened session is always presented at its
    // latest turn; rebuilds of the current session retain the reader's
    // deliberate scroll position.
    m_agentTranscript->setBatchHoldsFrame(sameTranscript, rowsLandAbove);
    m_agentTranscript->setBulkPopulate(true);
    if (openingSession)
        m_agentTranscript->jumpToBottom();
    m_agentTranscript->clear();
    if (openingSession)
        m_agentTranscript->jumpToBottom();
    m_renderedTranscriptSession = -1;
    m_renderedTranscriptCount = -1;
    m_renderedExternalSession = -1; // the shared view no longer holds an external
    // When the whole Agents page is entering the widget tree, adding transcript
    // rows here would make QWidget::show() lay all of them out on the click
    // stack. Once visible, render one small slice immediately so short histories
    // still appear without a perceptible blank frame.
    if (m_agentsPage && m_agentsPage->isVisible())
        renderTranscriptSlice(generation);
    else
        QTimer::singleShot(0, this,
                           [this, generation] { renderTranscriptSlice(generation); });
}

// adhoc #2104: re-entering the Agents tab on a session that gained events while
// it was off screen used to clear the view and replay its whole (bounded)
// history — 830 ms of blocked GUI thread in one sampled stall, all of it
// rebuilding rows that were already correct. The stream only ever appends, so
// the rows on screen still stand: queue the new tail through the same
// time-sliced renderer. Returns false when the caller must rebuild after all.
bool MainWindow::renderTranscriptTail(int sessionId)
{
    if (!m_agentTranscript || sessionId < 0 ||
        m_renderedTranscriptSession != sessionId ||
        m_renderedTranscriptCount < 0)
        return false;
    if (m_streamEvents.value(sessionId).size() <= m_renderedTranscriptCount)
        return false; // shrank, or nothing new: not an append
    const quint64 generation = ++m_agentTranscriptRenderGeneration;
    m_agentTranscriptRenderSession = sessionId;
    m_agentTranscriptRenderCursor = m_renderedTranscriptCount;
    // The cursor already starts past every hidden event, so nothing takes the
    // stats-only path; carrying the count keeps the "load earlier" notice's
    // tally right when the slice finishes.
    m_agentTranscriptRenderSkipped = m_transcriptSkipped;
    m_agentTranscriptSkippedNoticeAdded = true; // the notice on screen stands
    m_agentTranscript->setBatchHoldsFrame(true);
    m_agentTranscript->setBulkPopulate(true);
    renderTranscriptSlice(generation);
    return true;
}

void MainWindow::renderTranscriptSlice(quint64 generation)
{
    if (!m_agentTranscript || generation != m_agentTranscriptRenderGeneration)
        return;
    const int sessionId = m_agentTranscriptRenderSession;
    if (sessionId < 0 || sessionId != m_selectedAgentSessionId) {
        m_agentTranscript->setBulkPopulate(false);
        m_agentTranscriptRenderSession = -1;
        return;
    }

    const QList<QJsonObject> events = m_streamEvents.value(sessionId);
    QElapsedTimer budget;
    budget.start();
    bool handled = false;
    while (m_agentTranscriptRenderCursor < events.size() &&
           (!handled || budget.elapsed() < 8)) {
        if (m_agentTranscriptRenderCursor < m_agentTranscriptRenderSkipped) {
            m_agentTranscript->accumulateStatsOnly(
                events.at(m_agentTranscriptRenderCursor));
        } else {
            if (!m_agentTranscriptSkippedNoticeAdded) {
                m_agentTranscriptSkippedNoticeAdded = true;
                if (m_agentTranscriptRenderSkipped > 0)
                    m_agentTranscript->addSkippedNotice(
                        m_agentTranscriptRenderSkipped);
            }
            const QJsonObject &ev = events.at(m_agentTranscriptRenderCursor);
            if (ev.value(QStringLiteral("type")).toString() ==
                QLatin1String("_local_user")) {
                m_agentTranscript->addUserTurn(
                    ev.value(QStringLiteral("text")).toString(),
                    ClaudeTranscriptView::eventTime(ev));
            } else {
                m_agentTranscript->handleEvent(ev);
            }
        }
        ++m_agentTranscriptRenderCursor;
        handled = true;
    }

    if (m_agentTranscriptRenderCursor < events.size()) {
        QTimer::singleShot(0, this,
                           [this, generation] { renderTranscriptSlice(generation); });
        return;
    }

    m_agentTranscript->setBulkPopulate(false);
    m_renderedTranscriptSession = sessionId;
    m_renderedTranscriptCount = events.size();
    m_transcriptSkipped = m_agentTranscriptRenderSkipped;
    m_agentTranscriptRenderSession = -1;
    // A rebuild replays the events, so the closing "Done" row comes back as the
    // CLI wrote it: re-mark it if this session is still finishing background work.
    m_agentTranscript->setBackgroundWorkNote(agentBackgroundWorkNote(sessionId));
    reapplyTranscriptSearch(); // re-highlight against the rebuilt transcript
}

void MainWindow::loadEarlierTranscriptEvents()
{
    if (m_agentTranscript &&
        m_agentTranscriptRenderSession == m_selectedAgentSessionId) {
        // The notice can become clickable before the current replay's last
        // slice. Reset its one-click guard; the completed view will accept the
        // request without racing two rebuilds.
        m_agentTranscript->prependEarlierEvents(
            {}, m_agentTranscriptRenderSkipped);
        return;
    }
    if (!m_agentTranscript || m_renderedTranscriptSession != m_selectedAgentSessionId)
        return;
    const int sessionId = m_renderedTranscriptSession;
    const QList<QJsonObject> &events = m_streamEvents.value(sessionId);
    if (m_transcriptSkipped <= 0 || m_transcriptSkipped > events.size()) {
        m_transcriptSkipped = 0;
        m_agentTranscript->prependEarlierEvents({}, 0); // clears a stale notice, if any
        return;
    }
    constexpr int kBatch = 300;
    const int currentlyShown = events.size() - m_transcriptSkipped;
    m_agentTranscriptRenderLimits.insert(
        sessionId, qMin(int(events.size()), currentlyShown + kBatch));
    m_agentTranscriptScrollRestoreSession = sessionId;
    // Rebuild through the same bounded slices as initial selection. The old
    // prepend helper constructed all 300 rich rows in one click handler.
    renderTranscriptForSession(sessionId);
}

void MainWindow::reapplyTranscriptSearch()
{
    if (!m_agentTranscript || !m_transcriptSearch)
        return;
    const QString q = m_transcriptSearch->text().trimmed();
    if (q.isEmpty())
        m_agentTranscript->clearSearch();
    else
        m_agentTranscript->search(q);
}

void MainWindow::refreshAgentFilesPanel(int sessionId)
{
    if (!m_agentFilesList)
        return;
    // The "Files changed" page these widgets live on is built but kept hidden
    // (adhoc #42 removed its tab). Its refresh was still running on every
    // non-streaming transcript event of the selected session: a 400 ms
    // debounce, then `git status`, `git log --cherry-pick`, `git rev-list` and
    // a `git diff`, plus a rich-text diff render into the hidden view — over
    // five thousand git processes a day on the source node for a page nobody
    // can open. Skip it all while the page is off-screen; whoever shows the
    // page again calls this (as the old tab's currentChanged handler did) and
    // gets a fresh probe.
    if (!m_agentDiffView || !m_agentDiffView->isVisible())
        return;
    if (m_agentDiffRenderedSession != sessionId) {
        populateAgentFilesPanel(sessionId, QStringList());
        // The last session's diff must not sit under this one's name until the
        // probe lands, nor come back out of the render memo.
        const QString empty =
            QStringLiteral("<p style='color:#8b949e'>No changes yet.</p>");
        if (m_agentDiffLastHtml != empty) {
            setDiffHtml(m_agentDiffView, empty);
            m_agentDiffLastHtml = empty;
            m_agentDiffRenderKey.clear();
            renderAgentCodeFileStrip(sessionId, {});
        }
    }
    scheduleAgentFilesDiff(sessionId);
}

void MainWindow::populateAgentFilesPanel(int sessionId, const QStringList &diffFiles)
{
    if (!m_agentFilesList || sessionId != m_selectedAgentSessionId)
        return;
    const QString repoPath = sessionWorkdir(sessionId); // the worktree, if any
    QStringList absPaths;
    auto addAbs = [&](const QString &p) {
        QString abs = QDir::isAbsolutePath(p) || repoPath.isEmpty()
                          ? p : QDir(repoPath).filePath(p);
        if (!absPaths.contains(abs))
            absPaths.append(abs);
    };
    for (const QString &p : m_streamFiles.value(sessionId))
        addAbs(p);
    for (const QString &p : diffFiles)
        addAbs(p);

    m_agentFilesList->clear();
    for (const QString &abs : absPaths) {
        const QString label = repoPath.isEmpty() ? abs
                                                  : QDir(repoPath).relativeFilePath(abs);
        auto *it = new QListWidgetItem(label);
        it->setIcon(themedOcticon(QStringLiteral("file-diff"), QColor("#d29922"), 14));
        it->setToolTip(abs);
        it->setData(Qt::UserRole, abs);
        m_agentFilesList->addItem(it);
    }
}

void MainWindow::scheduleAgentFilesDiff(int sessionId)
{
    if (sessionId != m_selectedAgentSessionId)
        return;
    const QString repoPath = sessionWorkdir(sessionId);
    if (repoPath.isEmpty())
        return;
    if (!m_agentFilesDiffTimer) {
        m_agentFilesDiffTimer = new QTimer(this);
        m_agentFilesDiffTimer->setSingleShot(true);
        m_agentFilesDiffTimer->setInterval(400);
        connect(m_agentFilesDiffTimer, &QTimer::timeout, this, [this] {
            const int sid = m_selectedAgentSessionId;
            const QString dir = sessionWorkdir(sid);
            if (sid <= 0 || dir.isEmpty())
                return;
            const forkmesh::BackgroundRoutine routine(
                QStringLiteral("agent files diff poll · agent #%1").arg(sid));
            const QString base = sessionDiffBase(sid, dir);
            auto probe = std::make_shared<AgentDiffProbe>();
            probe->pending = base.isEmpty() ? 1 : 3;
            const auto inputsDone = [this, sid, dir, base, probe] {
                if (--probe->pending > 0)
                    return;
                QSet<QString> visiblePaths = probe->ownedPaths;
                visiblePaths.unite(probe->uncommitted);
                if (!base.isEmpty() && visiblePaths.isEmpty()) {
                    probe->patchOk = true;
                    probe->patch.clear();
                    if (sid == m_selectedAgentSessionId)
                        renderAgentDiff(sid, *probe);
                    return;
                }
                QStringList args{QStringLiteral("diff")};
                if (!base.isEmpty()) {
                    args << base << QStringLiteral("--");
                    args.append(literalPathspecs(visiblePaths));
                }
                runGitDetached(
                    dir, args,
                    [this, sid, probe](bool ok, const QByteArray &out) {
                        probe->patchOk = ok;
                        if (ok)
                            probe->patch = out;
                        if (ok && sid == m_selectedAgentSessionId)
                            renderAgentDiff(sid, *probe);
                    });
            };
            runGitDetached(dir,
                           {QStringLiteral("status"), QStringLiteral("--porcelain"),
                            QStringLiteral("--untracked-files=all")},
                           [probe, inputsDone](bool ok, const QByteArray &out) {
                               if (ok)
                                   for (QString line : QString::fromUtf8(out).split(
                                            QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                                       QString p = line.mid(3);
                                       const int arrow =
                                           p.indexOf(QLatin1String(" -> "));
                                       if (arrow >= 0)
                                           p = p.mid(arrow + 4);
                                       if (p.startsWith(QLatin1Char('"')) &&
                                           p.endsWith(QLatin1Char('"')))
                                           p = p.mid(1, p.size() - 2);
                                       probe->uncommitted.insert(p.trimmed());
                                       if (line.startsWith(QLatin1String("??")))
                                           probe->untracked.insert(p.trimmed());
                                   }
                               inputsDone();
                           });
            if (!base.isEmpty()) {
                runGitDetached(dir,
                               {QStringLiteral("log"), QStringLiteral("--no-merges"),
                                QStringLiteral("--cherry-pick"),
                                QStringLiteral("--right-only"),
                                QStringLiteral("--format=%x1e%h %s"),
                                QStringLiteral("--name-only"),
                                base + QStringLiteral("...HEAD")},
                               [probe, inputsDone](bool ok, const QByteArray &out) {
                                   if (ok)
                                       parseAgentOwnedLog(out, &probe->ownedPaths,
                                                          &probe->commitLines);
                                   inputsDone();
                               });
                runGitDetached(dir,
                               {QStringLiteral("rev-list"), QStringLiteral("--count"),
                                QStringLiteral("HEAD..") + base},
                               [probe, inputsDone](bool ok, const QByteArray &out) {
                                   if (ok)
                                       probe->behind =
                                           QString::fromUtf8(out).trimmed().toInt();
                                   inputsDone();
                               });
            }
        });
    }
    // Throttle, not debounce: a run editing steadily would otherwise keep
    // pushing the probe back and the Code page would never catch up.
    if (!m_agentFilesDiffTimer->isActive())
        m_agentFilesDiffTimer->start();
}

void MainWindow::ignoreAgentDiffSecrets(const QList<RepoSecurityFinding> &findings)
{
    const int sessionId = m_selectedAgentSessionId;
    if (sessionId <= 0 || sessionId != m_agentDiffSecretsSession)
        return;
    const QString dir = sessionWorkdir(sessionId);
    if (dir.isEmpty())
        return;
    const QList<RepoSecurityFinding> marked = addSecretIgnoreMarkers(dir, findings);
    if (marked.isEmpty())
        return;
    m_agentDiffSecrets = forkmesh::ui::secretFindingsWithout(m_agentDiffSecrets, marked);
    if (m_agentSecretBanner)
        m_agentSecretBanner->removeFindings(marked);
    scheduleAgentFilesDiff(sessionId);
}

QString MainWindow::sessionBaseRef(int sessionId)
{
    if (const AgentSession *s = findAgentSession(sessionId); s && !s->baseRef.isEmpty())
        return s->baseRef;
    if (m_streamSessionInfo.contains(sessionId))
        return m_streamSessionInfo.value(sessionId).baseRef;
    return QString();
}

QString MainWindow::sessionBaseBranch(int sessionId)
{
    if (const AgentSession *s = findAgentSession(sessionId); s && !s->baseBranch.isEmpty())
        return s->baseBranch;
    if (m_streamSessionInfo.contains(sessionId))
        return m_streamSessionInfo.value(sessionId).baseBranch;
    return QString();
}

QString MainWindow::sessionDiffBase(int sessionId, const QString &dir)
{
    Q_UNUSED(dir);
    const QString baseBranch = sessionBaseBranch(sessionId);
    if (!baseBranch.isEmpty())
        return baseBranch;
    return sessionBaseRef(sessionId);
}

void MainWindow::renderAgentDiff(int sessionId, const AgentDiffProbe &probe)
{
    if (!m_agentDiffView || sessionId != m_selectedAgentSessionId)
        return;
    if (&probe != &m_agentDiffLastProbe)
        m_agentDiffLastProbe = probe;
    const QString dir = sessionWorkdir(sessionId);
    const QString base = sessionDiffBase(sessionId, dir);
    const QHash<QString, QString> viewedStamps = m_agentCodeViewed.value(sessionId);
    const QSet<QString> expanded = m_agentCodeExpanded.value(sessionId);
    constexpr int kMaxUntracked = 50;
    QStringList untracked(probe.untracked.cbegin(), probe.untracked.cend());
    untracked.sort();
    if (untracked.size() > kMaxUntracked)
        untracked = untracked.mid(0, kMaxUntracked);
    const bool split = diffSplitPref() &&
                       !(forkmesh::ui::workspaceLayoutNew() &&
                         m_agentDiffView->width() < 700);
    QString renderKey = QString::number(sessionId) + QLatin1Char('\n') + dir +
                        QLatin1Char('\n') + base +
                        (split ? QStringLiteral("\nsplit")
                               : QStringLiteral("\nunified")) +
                        (currentThemeIsDark() ? QStringLiteral("\ndark")
                                              : QStringLiteral("\nlight")) +
                        (forkmesh::ui::workspaceLayoutNew()
                             ? QStringLiteral("\nnext")
                             : QStringLiteral("\nclassic"));
    // Review state and new files' contents change the page without changing
    // the patch, so they key the memo too.
    QStringList viewedPaths = viewedStamps.keys();
    viewedPaths.sort();
    for (const QString &p : viewedPaths)
        renderKey += QStringLiteral("\nv ") + p;
    QStringList expandedPaths(expanded.cbegin(), expanded.cend());
    expandedPaths.sort();
    for (const QString &p : expandedPaths)
        renderKey += QStringLiteral("\ne ") + p;
    for (const QString &p : untracked) {
        const QFileInfo info(QDir(dir).filePath(p));
        renderKey += QStringLiteral("\nu ") + QString::number(info.size()) +
                     QLatin1Char(' ') +
                     QString::number(info.lastModified().toMSecsSinceEpoch()) +
                     QLatin1Char(' ') + p;
    }
    struct RenderState {
        QList<DiffFileEntry> files; // matches m_agentDiffLastHtml
        bool busy = false;          // a worker render is in flight
        bool queued = false;        // …and a newer probe arrived while it ran
        int queuedSession = -1;
        AgentDiffProbe queuedProbe;
    };
    static RenderState state;
    if (renderKey == m_agentDiffRenderKey && probe.patch == m_agentDiffRenderedPatch &&
        !m_agentDiffLastHtml.isEmpty()) {
        applyAgentDiff(sessionId, probe, state.files, m_agentDiffLastHtml);
        return;
    }
    if (state.busy) {
        state.queued = true;
        state.queuedSession = sessionId;
        state.queuedProbe = probe;
        return;
    }
    state.busy = true;
    // Everything the worker touches is captured by value (see the git-pump UAF
    // family: nothing shared with the GUI thread may be read off it).
    const DiffRenderAppearance appearance = currentDiffRenderAppearance();
    const QByteArray patch = probe.patch;
    struct AgentDiffRender {
        QString html;
        QList<DiffFileEntry> files;
        QHash<QString, QString> fingerprints;
        QList<RepoSecurityFinding> secrets;
    };
    runOffThread<AgentDiffRender>(
        [split, appearance, patch, dir, base, untracked, viewedStamps, expanded] {
            AgentDiffRender out;
            const QString text =
                QString::fromUtf8(patch) + agentUntrackedFilesPatch(dir, untracked);
            out.fingerprints = diffFileFingerprints(text);
            out.secrets = RepoSecurity::findSecretsInDiff(text);
            // A mark made on an older version of the file no longer describes it.
            QSet<QString> viewed;
            for (auto it = viewedStamps.cbegin(); it != viewedStamps.cend(); ++it) {
                const auto now = out.fingerprints.constFind(it.key());
                if (now != out.fingerprints.cend() &&
                    (it.value().isEmpty() || it.value() == *now))
                    viewed.insert(it.key());
            }
            out.html = renderDiffHtmlSplit(split, text, out.files, dir, base,
                                           QString(), QString(),
                                           QHash<QString, QString>(), viewed,
                                           expanded, appearance);
            return out;
        },
        [this, sessionId, probe, renderKey, patch](AgentDiffRender out) {
            RenderState &st = state;
            st.busy = false;
            if (m_agentDiffView && sessionId == m_selectedAgentSessionId) {
                // The same pruning the worker rendered with, applied to the marks
                // as they stand now (a tick may have landed meanwhile).
                auto marks = m_agentCodeViewed.find(sessionId);
                if (marks != m_agentCodeViewed.end()) {
                    for (auto it = marks->begin(); it != marks->end();) {
                        const auto now = out.fingerprints.constFind(it.key());
                        if (now == out.fingerprints.cend() ||
                            (!it.value().isEmpty() && it.value() != *now)) {
                            it = marks->erase(it);
                            continue;
                        }
                        it.value() = *now;
                        ++it;
                    }
                    if (marks->isEmpty())
                        m_agentCodeViewed.erase(marks);
                }
                m_agentCodeFingerprints.insert(sessionId, out.fingerprints);
                const QString shown =
                    out.html.isEmpty()
                        ? QStringLiteral("<p style='color:#8b949e'>No changes yet.</p>")
                        : out.html;
                m_agentDiffRenderKey = renderKey;
                m_agentDiffRenderedPatch = patch;
                m_agentDiffSecrets = out.secrets;
                m_agentDiffSecretsSession = sessionId;
                st.files = out.files;
                applyAgentDiff(sessionId, probe, st.files, shown);
            }
            if (st.queued) {
                st.queued = false;
                const int sid = st.queuedSession;
                const AgentDiffProbe next = st.queuedProbe;
                st.queuedSession = -1;
                st.queuedProbe = AgentDiffProbe();
                renderAgentDiff(sid, next);
            }
        });
}

void MainWindow::applyAgentDiff(int sessionId, const AgentDiffProbe &probe,
                                const QList<DiffFileEntry> &files,
                                const QString &shown)
{
    if (!m_agentDiffView || sessionId != m_selectedAgentSessionId)
        return;
    const QString dir = sessionWorkdir(sessionId);
    if (m_agentSecretBanner)
        m_agentSecretBanner->setFindings(m_agentDiffSecretsSession == sessionId
                                             ? m_agentDiffSecrets
                                             : QList<RepoSecurityFinding>(),
                                         !dir.isEmpty() && QDir(dir).exists());
    if (sessionId != m_agentDiffRenderedSession || shown != m_agentDiffLastHtml) {
        // A live run re-renders under the reader on every edit: hold their place.
        if (sessionId == m_agentDiffRenderedSession)
            if (QScrollBar *bar = m_agentDiffView->verticalScrollBar())
                m_diffRestoreScroll.insert(m_agentDiffView, bar->value());
        setDiffHtml(m_agentDiffView, shown);
        m_agentDiffLastHtml = shown;
    }

    const QSet<QString> &uncommitted = probe.uncommitted;
    const QHash<QString, QString> viewed = m_agentCodeViewed.value(sessionId);

    if (m_agentFilesList) {
        QString selectedAnchor;
        if (QListWidgetItem *current = m_agentFilesList->currentItem())
            selectedAnchor = current->data(Qt::UserRole + 1).toString();
        int selectedRow = -1;
        {
            QSignalBlocker block(m_agentFilesList);
            m_agentFilesList->clear();
            for (const DiffFileEntry &f : files) {
                const QString name = f.path.section(QLatin1Char('/'), -1);
                const bool isUncommitted = uncommitted.contains(f.path);
                const bool isViewed = viewed.contains(f.path);
                auto *item = new QListWidgetItem(
                    QString::fromUtf8("%1%2   +%3 \xE2\x88\x92%4%5")
                        .arg(isViewed ? QString::fromUtf8("\xE2\x9C\x93 ") : QString(),
                             name, QString::number(f.adds), QString::number(f.dels),
                             isUncommitted ? QString::fromUtf8("  \xE2\x97\x8F")
                                           : QString()));
                if (isViewed)
                    item->setForeground(Theme::remapColor(QColor("#8b949e")));
                QColor tint("#d29922");
                QString icon = "file-diff";
                if (f.status == QLatin1String("added")) {
                    icon = "diff";
                    tint = QColor("#3fb950");
                } else if (f.status == QLatin1String("deleted")) {
                    icon = "trash";
                    tint = QColor("#f85149");
                }
                item->setIcon(themedOcticon(icon, tint, 14));
                const QString abs =
                    dir.isEmpty() ? f.path : QDir(dir).filePath(f.path);
                item->setData(Qt::UserRole, abs);          // open on activate
                item->setData(Qt::UserRole + 1, f.anchor); // scroll diff on select
                item->setToolTip(
                    isUncommitted
                        ? QString::fromUtf8("%1 \xC2\xB7 %2 \xC2\xB7 uncommitted")
                              .arg(f.status, f.path)
                        : QString::fromUtf8("%1 \xC2\xB7 %2").arg(f.status, f.path));
                m_agentFilesList->addItem(item);
            }
            fitFileListToWidestEntry(m_agentFilesList);
            for (int row = 0; row < m_agentFilesList->count(); ++row) {
                if (m_agentFilesList->item(row)->data(Qt::UserRole + 1).toString() ==
                    selectedAnchor) {
                    selectedRow = row;
                    break;
                }
            }
        }
        if (selectedRow < 0 && m_agentFilesList->count() > 0)
            selectedRow = 0;
        if (selectedRow >= 0) {
            // Re-selecting the file the reader already had would scroll the
            // diff back to its top on every live refresh.
            const bool sameFile =
                sessionId == m_agentDiffRenderedSession && !selectedAnchor.isEmpty() &&
                m_agentFilesList->item(selectedRow)->data(Qt::UserRole + 1).toString() ==
                    selectedAnchor;
            const QSignalBlocker block(sameFile ? m_agentFilesList : nullptr);
            m_agentFilesList->setCurrentRow(selectedRow);
        }
    }
    if (!m_agentDiffNav && m_agentFilesList)
        m_agentDiffNav = new DiffFileNavigator(m_agentDiffView, m_agentFilesList,
                                               Qt::UserRole + 1, this);
    if (m_agentDiffNav)
        m_agentDiffNav->rebuild(files, m_diffFontPt);

    const int n = files.size();
    const QHash<QString, QString> fingerprints = m_agentCodeFingerprints.value(sessionId);
    int seen = 0;
    bool current = true;
    for (const DiffFileEntry &f : files) {
        const auto mark = viewed.constFind(f.path);
        if (mark == viewed.cend())
            continue;
        ++seen;
        if (mark->isEmpty() || *mark != fingerprints.value(f.path))
            current = false;
    }
    const QString viewedCount = diffViewedCountHtml(seen, n, current);
    if (m_agentCodeViewedLabel) {
        m_agentCodeViewedLabel->setText(viewedCount);
        m_agentCodeViewedLabel->setVisible(n > 0);
    }
    if (m_agentDetailTabs && m_agentFilesTabIndex >= 0)
        m_agentDetailTabs->setTabText(
            m_agentFilesTabIndex,
            n > 0 ? QStringLiteral("Files changed (%1)").arg(n)
                  : QStringLiteral("Files changed"));
    if (m_agentFilesChangedSummary) {
        int adds = 0, dels = 0;
        for (const DiffFileEntry &f : files) { adds += f.adds; dels += f.dels; }
        QStringList parts;
        const AgentSession *summarySession = findAgentSession(sessionId);
        const QString headBranch =
            summarySession ? summarySession->branchName : QString();
        const QString baseBranch = sessionBaseBranch(sessionId);
        if (!headBranch.isEmpty() && !baseBranch.isEmpty() && headBranch != baseBranch)
            parts << QString::fromUtf8("%1 \xE2\x86\x92 %2")
                         .arg(headBranch.toHtmlEscaped(), baseBranch.toHtmlEscaped());
        parts << QStringLiteral("%1 file%2 changed").arg(n).arg(n == 1 ? "" : "s");
        if (adds > 0 || dels > 0)
            parts << QString::fromUtf8("+%1 \xE2\x88\x92%2").arg(adds).arg(dels);
        if (seen > 0)
            parts << viewedCount;
        const int ahead = probe.commitLines.size();
        if (ahead > 0)
            parts << QStringLiteral("%1 commit%2").arg(ahead).arg(ahead == 1 ? "" : "s");
        if (probe.behind > 0 && !baseBranch.isEmpty())
            parts << QStringLiteral("%1 behind %2")
                         .arg(probe.behind)
                         .arg(baseBranch.toHtmlEscaped());
        m_agentFilesChangedSummary->setText(
            parts.join(QString::fromUtf8("&nbsp;&nbsp;\xC2\xB7&nbsp;&nbsp;")));
    }

    if (m_agentCommitsList && m_agentCommitsHeading) {
        m_agentCommitsList->clear();
        int commitCount = 0;
        for (const QString &line : probe.commitLines) {
            auto *item = new QListWidgetItem(line.trimmed());
            item->setIcon(themedOcticon(QStringLiteral("git-commit"),
                                        Theme::remapColor(QColor("#8b949e")), 14));
            m_agentCommitsList->addItem(item);
            ++commitCount;
        }
        const bool any = commitCount > 0;
        m_agentCommitsHeading->setVisible(any);
        m_agentCommitsList->setVisible(any);
        if (any)
            m_agentCommitsHeading->setText(
                QStringLiteral("Commits (%1)").arg(commitCount));
    }

    renderAgentCodeFileStrip(sessionId, files);
    m_agentDiffRenderedSession = sessionId;
}

void MainWindow::renderAgentCodeFileStrip(int sessionId,
                                          const QList<DiffFileEntry> &files)
{
    if (!m_agentCodeFileStrip)
        return;
    constexpr int kChips = 10;
    QList<DiffFileEntry> busiest = files;
    std::stable_sort(busiest.begin(), busiest.end(),
                     [](const DiffFileEntry &a, const DiffFileEntry &b) {
                         return a.adds + a.dels > b.adds + b.dels;
                     });
    const int shown = qMin(kChips, int(busiest.size()));
    const QHash<QString, QString> viewed = m_agentCodeViewed.value(sessionId);
    // Rebuilt only when a chip would read differently: a live run refreshes
    // this on every probe.
    QString signature = QString::number(sessionId) + QLatin1Char(' ') +
                        QString::number(busiest.size());
    for (int i = 0; i < shown; ++i) {
        const DiffFileEntry &f = busiest.at(i);
        signature += QLatin1Char('\n') + f.anchor + QLatin1Char(' ') +
                     QString::number(f.adds) + QLatin1Char(' ') +
                     QString::number(f.dels) +
                     (viewed.contains(f.path) ? QStringLiteral(" v ") : QStringLiteral(" - ")) +
                     f.path;
    }
    if (signature == m_agentCodeStripSignature)
        return;
    m_agentCodeStripSignature = signature;

    QLayout *flow = m_agentCodeFileStrip->layout();
    while (QLayoutItem *item = flow->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->setParent(nullptr); // off the strip now, not at the next idle
            w->deleteLater();
        }
        delete item;
    }
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    const QString nameColor = dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328");
    const QString dim = QStringLiteral("#8b949e");
    for (int i = 0; i < shown; ++i) {
        const DiffFileEntry &f = busiest.at(i);
        const bool isViewed = viewed.contains(f.path);
        auto *chip = new QLabel(m_agentCodeFileStrip);
        chip->setObjectName(QStringLiteral("agentCodeFileChip"));
        chip->setTextFormat(Qt::RichText);
        chip->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        chip->setText(
            QStringLiteral("<a href='") + f.anchor.toHtmlEscaped() +
            QStringLiteral("' style='text-decoration:none;color:") +
            (isViewed ? dim : nameColor) + QStringLiteral("'>") +
            (isViewed ? QString::fromUtf8("\xE2\x9C\x93 ") : QString()) +
            f.path.section(QLatin1Char('/'), -1).toHtmlEscaped() +
            QStringLiteral("</a>&nbsp;<span style='color:#3fb950'>+") +
            QString::number(f.adds) +
            QString::fromUtf8("</span>&nbsp;<span style='color:#f85149'>\xE2\x88\x92") +
            QString::number(f.dels) + QStringLiteral("</span>"));
        chip->setToolTip(f.path);
        connect(chip, &QLabel::linkActivated, this, [this](const QString &anchor) {
            if (m_agentFilesList) {
                for (int row = 0; row < m_agentFilesList->count(); ++row) {
                    if (m_agentFilesList->item(row)->data(Qt::UserRole + 1).toString() ==
                        anchor) {
                        m_agentFilesList->setCurrentRow(row);
                        break;
                    }
                }
            }
            if (m_agentDiffView)
                scrollDiffToAnchor(m_agentDiffView, anchor);
        });
        flow->addWidget(chip);
    }
    if (busiest.size() > kChips) {
        auto *more = new QLabel(
            QStringLiteral("+%1 more below").arg(busiest.size() - kChips),
            m_agentCodeFileStrip);
        more->setObjectName(QStringLiteral("agentCodeFileMore"));
        more->setStyleSheet(Theme::remapSheet(QStringLiteral("color:") + dim));
        flow->addWidget(more);
    }
    m_agentCodeFileStrip->setVisible(shown > 0);
}

void MainWindow::onAgentCodeAnchorClicked(const QUrl &url)
{
    const bool viewedPill = url.scheme() == QLatin1String("viewed");
    if (!viewedPill && url.scheme() != QLatin1String("expand"))
        return;
    const QString path = url.path();
    const int sid = m_selectedAgentSessionId;
    if (path.isEmpty() || sid <= 0 || !m_agentDiffView ||
        m_agentDiffRenderedSession != sid)
        return;
    QSet<QString> &expanded = m_agentCodeExpanded[sid];
    if (viewedPill) {
        // Either direction settles the fold, as the other diff panes do.
        expanded.remove(path);
        QHash<QString, QString> &marks = m_agentCodeViewed[sid];
        if (marks.remove(path) == 0)
            marks.insert(path, m_agentCodeFingerprints.value(sid).value(path));
        if (marks.isEmpty())
            m_agentCodeViewed.remove(sid);
    } else if (!expanded.remove(path)) {
        expanded.insert(path);
    }
    if (expanded.isEmpty())
        m_agentCodeExpanded.remove(sid);
    if (QScrollBar *bar = m_agentDiffView->verticalScrollBar())
        m_diffRestoreScroll.insert(m_agentDiffView, bar->value());
    renderAgentDiff(sid, m_agentDiffLastProbe);
}

void MainWindow::updateAgentFilesTabState(int sessionId)
{
    AgentSession *s = findAgentSession(sessionId);
    QString branch, wt, repoLocal;
    if (s) {
        branch = s->branchName;
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri >= 0) {
            repoLocal = m_repositories.at(ri).localPath;
            if (!branch.isEmpty())
                wt = cachedSessionWorktreeForUi(sessionId, repoLocal, branch);
        }
    }
    // This runs on every agent selection. The default-branch helper may shell out
    // twice when its cache is cold, so use only metadata already loaded for the
    // open repository; the background table probe will refresh that metadata.
    QString base = m_repoInfo.defaultBranch.trimmed();
    const QString gitDir = repoGitDir();
    if (base.isEmpty() && m_branchesCacheDir == gitDir) {
        if (m_branchesCache.contains(QStringLiteral("main")))
            base = QStringLiteral("main");
        else if (m_branchesCache.contains(QStringLiteral("master")))
            base = QStringLiteral("master");
    }
    if (base.isEmpty())
        base = m_repoBranch;
    const bool onDisk = !wt.isEmpty() && QDir(wt).exists();
    const bool isMain = !wt.isEmpty() && !repoLocal.isEmpty() &&
                        QDir(wt).absolutePath() == QDir(repoLocal).absolutePath();
    const bool feature = !branch.isEmpty() && branch != base && !isMain;
    const bool mergeAvailable = !m_worktreeMergeInProgress &&
                                !m_branchCleanupInProgress.contains(branch);
    if (m_agentMergeButton)
        m_agentMergeButton->setEnabled(feature && mergeAvailable &&
                                       repoHasWorkingTree());
    if (m_agentMergeDeleteButton)
        m_agentMergeDeleteButton->setEnabled(feature && mergeAvailable &&
                                             repoHasWorkingTree());
    if (m_agentUpdateButton)
        m_agentUpdateButton->setEnabled(feature && onDisk);
    if (m_agentWtDeleteButton)
        m_agentWtDeleteButton->setEnabled(feature && onDisk);
}

#ifdef FORKMESH_WINDOW_TESTS
QTextBrowser *MainWindow::testAgentDiffView() const
{
    return m_agentDiffView;
}

QString MainWindow::testAgentFilesSummary() const
{
    return m_agentFilesChangedSummary ? m_agentFilesChangedSummary->text() : QString();
}

QString MainWindow::testCurrentAgentMetaHtml() const
{
    return m_agentMeta ? m_agentMeta->text() : QString();
}

QString MainWindow::testPollAgentBackgroundWork(int sessionId, qint64 rootPid)
{
    if (rootPid <= 0) { // reset between fixtures
        m_agentCompletionChecks.remove(sessionId);
        m_agentBackgroundWork.remove(sessionId);
        applyAgentBackgroundWork();
        testSetAgentSessionProcessId(sessionId, 0);
        return QString();
    }
    testSetAgentSessionProcessId(sessionId, rootPid);
    m_agentCompletionChecks.insert(sessionId);
    // Scan here rather than through completeAgentSessionWhenSubprocessesExit(),
    // which hands the process walk to a worker: the verdict would not be back by
    // the time this returns. What is under test is the decision the scan feeds,
    // and that is exactly what the GUI-thread half below does.
    applyAgentSubprocessCompletionScan(
        sessionId, SystemStats::descendantProcesses(rootPid));
    QStringList parts;
    if (const AgentSession *session = findAgentSession(sessionId))
        parts << session->status << agentStatusLabel(*session)
              << agentStatusBadgeText(*session)
              << agentBackgroundWorkNote(sessionId);
    // Drop the pending check so the poll this queued has nothing to do; the
    // per-session tally stays, so a second call is seen as the next poll.
    m_agentCompletionChecks.remove(sessionId);
    applyAgentBackgroundWork();
    return parts.join(QLatin1Char('|'));
}

void MainWindow::testSetAgentSessionProcessId(int sessionId, qint64 processId)
{
    if (processId > 0)
        m_testAgentSessionProcessIds.insert(sessionId, processId);
    else
        m_testAgentSessionProcessIds.remove(sessionId);
}

int MainWindow::testAgentProcessMetaScanLaunches() const
{
    return m_testAgentProcessMetaScanLaunches;
}

bool MainWindow::testAgentProcessMetaScanInFlight() const
{
    return m_agentProcessMetaRefreshInFlight;
}

void MainWindow::testExpireAgentProcessMetaCache()
{
    m_agentProcessMetaSampledAtMs =
        QDateTime::currentMSecsSinceEpoch() - kAgentProcessMetaCacheMs - 1;
}

// The caption an action row button actually paints. Rail buttons draw their own
// label and mirror it into the accessible name, so QPushButton::text() can drift
// from what is on screen — reading the drawn caption is what makes these checks
// mean anything.
static QString agentActionCaption(const QPushButton *button)
{
    if (const auto *rail = dynamic_cast<const ActivityRailButton *>(button))
        return rail->accessibleName();
    return button ? button->text() : QString();
}

QString MainWindow::testAgentPrButtonText(int sessionId)
{
    showAgentSession(sessionId);
    return m_agentViewPrButton && m_agentViewPrButton->isVisible()
               ? agentActionCaption(m_agentViewPrButton)
               : QString();
}

QString MainWindow::testAgentIssueButtonText(int sessionId)
{
    showAgentSession(sessionId);
    return m_agentViewIssueButton && m_agentViewIssueButton->isVisible()
               ? agentActionCaption(m_agentViewIssueButton)
               : QString();
}

QString MainWindow::testAgentIssueButtonToolTip(int sessionId)
{
    showAgentSession(sessionId);
    return m_agentViewIssueButton && m_agentViewIssueButton->isVisible()
               ? m_agentViewIssueButton->toolTip()
               : QString();
}

bool MainWindow::testClickAgentIssueButton(int sessionId)
{
    showAgentSession(sessionId);
    if (!m_agentViewIssueButton || !m_agentViewIssueButton->isVisible())
        return false;
    m_agentViewIssueButton->click();
    return true;
}

QString MainWindow::testAgentMetaHtml(int sessionId)
{
    showAgentSession(sessionId);
    return m_agentMeta ? m_agentMeta->text() : QString();
}

QString MainWindow::testClickQuickAddContinue(int sessionId)
{
    if (!m_quickAddSendToAgentButton || !m_issueQuickAdd)
        return QString();
    const int restore = m_selectedAgentSessionId;
    m_selectedAgentSessionId = sessionId;
    m_issueQuickAdd->clear();
    clearQuickAddImages();
    m_quickAddSendToAgentButton->click();
    m_selectedAgentSessionId = restore;
    QString delivered = m_pendingSteerMessage.value(sessionId);
    if (delivered.isEmpty()) {
        const QList<QJsonObject> events = m_streamEvents.value(sessionId);
        for (auto it = events.crbegin(); it != events.crend(); ++it) {
            if (it->value(QStringLiteral("type")).toString() ==
                QLatin1String("_local_user")) {
                delivered = it->value(QStringLiteral("text")).toString();
                break;
            }
        }
    }
    m_agentQueue.removeAll(sessionId);
    return delivered;
}

QString MainWindow::testContinueAgentDuringBackgroundDrain(int sessionId)
{
    m_agentCompletionChecks.insert(sessionId);
    AgentBackgroundWork &work = m_agentBackgroundWork[sessionId];
    work.count = 1;
    work.summary = QStringLiteral("old-turn-child (PID 123)");
    applyAgentBackgroundWork();
    m_testLiveTransportSessions.insert(sessionId);
    const QString delivered = testClickQuickAddContinue(sessionId);
    m_testLiveTransportSessions.remove(sessionId);
    const AgentSession *session = findAgentSession(sessionId);
    return QStringLiteral("%1|%2|%3|%4")
        .arg(delivered, session ? session->status : QStringLiteral("missing"))
        .arg(m_agentCompletionChecks.contains(sessionId) ? 1 : 0)
        .arg(m_agentBackgroundWork.contains(sessionId) ? 1 : 0);
}

QString MainWindow::testQuickAddPlaceholder() const
{
    return m_issueQuickAdd ? m_issueQuickAdd->placeholderText() : QString();
}

QString MainWindow::testClickQuickAddContinueOnScreen(int expectSessionId)
{
    if (!m_quickAddSendToAgentButton || !m_issueQuickAdd)
        return QString();
    const int restore = m_selectedAgentSessionId;
    m_selectedAgentSessionId = -1;
    m_issueQuickAdd->clear();
    clearQuickAddImages();
    m_quickAddSendToAgentButton->click();
    m_selectedAgentSessionId = restore;
    QString delivered = m_pendingSteerMessage.value(expectSessionId);
    if (delivered.isEmpty()) {
        const QList<QJsonObject> events = m_streamEvents.value(expectSessionId);
        for (auto it = events.crbegin(); it != events.crend(); ++it) {
            if (it->value(QStringLiteral("type")).toString() ==
                QLatin1String("_local_user")) {
                delivered = it->value(QStringLiteral("text")).toString();
                break;
            }
        }
    }
    m_agentQueue.removeAll(expectSessionId);
    return delivered;
}

void MainWindow::testSendQuickAddFollowUp(const QString &text)
{
    if (!m_quickAddSendToAgentButton || !m_issueQuickAdd)
        return;
    clearQuickAddImages();
    m_issueQuickAdd->setPlainText(text);
    m_quickAddSendToAgentButton->click();
}
#endif

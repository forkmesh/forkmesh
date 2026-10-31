// MainWindowActions: MainWindow feature methods, split out of MainWindow.cpp.
// Actions (CI on push), notifications, and variables/secrets settings.
// These are MainWindow member functions defined in their own translation unit;
// the class itself is declared in MainWindow.h. Shared helpers live in
// MainWindowInternal.h / MainWindowShared.cpp (namespace forkmesh::ui).

#include "MainWindow.h"
#include "ProcessReaper.h"
#include "MainWindowInternal.h"
#include "DesktopPingForward.h"
#include "PingBatch.h"
#include "ReleaseManifestTag.h"
#include "ActionFailureSummary.h"
#include "ActionPushRange.h"
#include "CoveFileSupport.h"
#include "CrashHandler.h"
#include "ForkMeshVersion.h"
#include "KebabHeaderView.h"
#include "SettingsSectionsView.h"
#include "MirrorActionsConfiguration.h"
#include "MirrorActionsSummary.h"
#include "PrivateMirrorRuntime.h"
#include "PublicMirrorRuntime.h"
#include "UpstreamCheckoutSync.h"
#include "DesignTokens.h"
#include "FlowLayout.h"
#include "UiKit.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QSaveFile>
#include <QStyledItemDelegate>
#include <QVersionNumber>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

using namespace forkmesh::ui;


namespace {

QString actionRunLogPath(const ActionRun &run)
{
    return forkmesh::paths::dataPath(QStringLiteral("actions")) +
           QStringLiteral("/runs/") + run.repoKey() + QLatin1Char('/') +
           QString::number(run.id) + QStringLiteral("/log.txt");
}

constexpr int kActionStatusIconPx = 14;

// How many runs may stream beside the primary detail pane. Past this the
// columns are too narrow to read a log in, so the extras are dropped and the
// user is told which ones made it.
constexpr int kMaxParallelRunColumns = 5;

constexpr int kMaxLocalPings = 100;
constexpr int kWebAlertsPageSize = 40;

// How many `notify-send --wait` helpers may sit on the notification server at
// once waiting for a click. Past this a notification is still raised, just
// detached and unclickable, rather than growing a process per card.
constexpr int kMaxNotifyWaiters = 8;

constexpr qint64 kPingDedupeWindowMs = 5000;
constexpr int kPingDedupeChars = 100;
constexpr int kPingDedupeMinChars = 40;

bool sameAlertText(const QString &left, const QString &right)
{
    auto key = [](const QString &text) {
        QString flat = text.simplified();
        flat.remove(QChar(0x2026)); // …
        flat.remove(QStringLiteral("..."));
        return flat.simplified().left(kPingDedupeChars);
    };
    const QString a = key(left);
    const QString b = key(right);
    if (a.isEmpty() || b.isEmpty())
        return false;
    const qsizetype shared =
        std::min<qsizetype>({a.size(), b.size(), kPingDedupeMinChars});
    return a.left(shared) == b.left(shared);
}

QString actionRunStatusIconName(const QString &status)
{
    if (status == ActionStatus::Running)
        return QStringLiteral("sync");
    if (status == ActionStatus::Queued)
        return QStringLiteral("history");
    if (status == ActionStatus::AwaitingApproval)
        return QStringLiteral("alert");
    if (status == ActionStatus::Success)
        return QStringLiteral("check-circle");
    if (status == ActionStatus::Failed || status == ActionStatus::Rejected)
        return QStringLiteral("x");
    if (status == ActionStatus::Cancelled)
        return QStringLiteral("circle-slash");
    if (status == ActionStatus::Skipped)
        return QStringLiteral("stop");
    return QStringLiteral("terminal");
}

const char kCommitSignalMarker[] =
    "# forkmesh-commit-signal: spools a commit event so mirrors are told to "
    "update instantly";

const QRegularExpression kExternalActionCommit(
    QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
const QRegularExpression kExternalActionRef(
    QStringLiteral("^refs/heads/(?!/)(?!.*(?:\\.\\.|//))"
                   "[A-Za-z0-9._/-]{1,120}(?<!/)$"));

struct ServedMirrorHeadProbe {
    int index = -1;
    QString owner;
    QString name;
    QString mirrorPath;
};

struct ServedMirrorHeadResult {
    int index = -1;
    QString owner;
    QString name;
    QString mirrorPath;
    QString ref;
    QString commit;
};

struct ServedMirrorHeadPersistence {
    ServedMirrorHeadResult result;
    QString settingsKey;
};

struct PersistedServedMirrorHead {
    ServedMirrorHeadResult result;
    QString previousCommit;
};

QString externalActionHeadKey(const RepositoryRecord &repo)
{
    return QStringLiteral("actions/externalHeads/") +
           QString::fromLatin1(
               QCryptographicHash::hash(
                   (repo.owner + QLatin1Char('/') + repo.name +
                    QLatin1Char('/') + repo.externalActionsRef)
                       .toUtf8(),
                   QCryptographicHash::Sha256)
                   .toHex());
}

// Settings key holding the served mirror's default-branch tip this node has
// already queued push workflows for. Keyed by repository, not by mirror path:
// an encrypted mirror is re-materialized into a fresh temporary directory on
// every seal, so the path is not stable but the commit is.
QString servedActionHeadKey(const RepositoryRecord &repo)
{
    return QStringLiteral("actions/servedHeads/") +
           QString::fromLatin1(
               QCryptographicHash::hash(
                   (repo.owner + QLatin1Char('/') + repo.name).toUtf8(),
                   QCryptographicHash::Sha256)
                   .toHex());
}

// code-health: allow(blocking-wait) sync git primitive; only called from scanServedMirrorHeads' QtConcurrent::run probe
QString gitHeadRef(const QString &repository)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("--git-dir"), repository,
                   QStringLiteral("symbolic-ref"), QStringLiteral("--quiet"),
                   QStringLiteral("HEAD")});
    if (!process.waitForStarted(2000) ||
        !process.waitForFinished(5000) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        process.kill();
        return {};
    }
    const QString ref =
        QString::fromUtf8(process.readAllStandardOutput().left(512)).trimmed();
    return kExternalActionRef.match(ref).hasMatch() ? ref : QString();
}

// code-health: allow(blocking-wait) sync git primitive; callers are the QtConcurrent workers in scanServedMirrorHeads and scanExternalActionsSources
QString gitCommitAt(const QString &repository, const QString &ref)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        QStringLiteral("git"),
        {QStringLiteral("--git-dir"), repository,
         QStringLiteral("rev-parse"), QStringLiteral("--verify"),
         ref + QStringLiteral("^{commit}")});
    if (!process.waitForStarted(2000) ||
        !process.waitForFinished(5000) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        process.kill();
        return {};
    }
    const QString commit =
        QString::fromUtf8(process.readAllStandardOutput().left(256))
            .trimmed()
            .toLower();
    return kExternalActionCommit.match(commit).hasMatch()
               ? commit
               : QString();
}

// code-health: allow(blocking-wait) sync git primitive; only called from scanExternalActionsSources' QtConcurrent::run worker
bool fetchExternalActionRef(const RepositoryRecord &repo)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        QStringLiteral("git"),
        {QStringLiteral("--git-dir"), repo.mirrorPath,
         QStringLiteral("fetch"), QStringLiteral("--no-tags"),
         repo.externalActionsSource,
         QStringLiteral("+") + repo.externalActionsRef +
             QLatin1Char(':') + repo.externalActionsRef});
    if (!process.waitForStarted(2000) ||
        !process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

bool bindActionRepositoryState(ActionRun *run, const QString &mirror,
                               QString *error = nullptr)
{
    if (!run)
        return false;
    return ActionStore::repositoryStateDigest(
        mirror, run->commit, &run->repositoryTree, &run->executionDigest,
        error);
}

// code-health: allow(blocking-wait) one `git show <commit>:<workflow>` read from the local mirror, bounded (3 s start, 30 s finish, killed on failure). processActionQueue calls it on a runOffThread worker; approveActionRun, autoApproveAwaitingRuns and rerunSelectedRun call it on the GUI thread; the wait is un-pumped, which is what keeps autoApproveAwaitingRuns' by-reference walk over m_actionRuns safe (a pumped wait could let a reload rebuild the list), so switching to a keep-alive git helper is not a drop-in fix
QString workflowContentAt(const QString &mirror, const QString &commit,
                          const QString &path)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("-C"), mirror, QStringLiteral("show"),
                   commit + QLatin1Char(':') + path});
    if (!process.waitForStarted(3000) ||
        !process.waitForFinished(30000) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

constexpr qreal kEdgeGlowCornerRadius = 10.0;

void paintEdgeGlow(QWidget *widget, const QColor &edge, int steps,
                   int glowAlpha, int glowAlphaStep)
{
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    const QRectF bounds(widget->rect());
    painter.setPen(QPen(edge, 3));
    painter.drawRoundedRect(bounds.adjusted(1.5, 1.5, -1.5, -1.5),
                            kEdgeGlowCornerRadius - 1.5,
                            kEdgeGlowCornerRadius - 1.5);
    for (int step = 1; step <= steps; ++step) {
        const qreal inset = 2.5 + step * 3;
        const qreal radius = std::max<qreal>(0.0, kEdgeGlowCornerRadius - inset);
        QColor glow(edge);
        glow.setAlpha(glowAlpha - step * glowAlphaStep);
        painter.setPen(QPen(glow, 3));
        painter.drawRoundedRect(bounds.adjusted(inset, inset, -inset, -inset),
                                radius, radius);
    }
}

} // namespace

void MainWindow::initActions()
{
    initActionStore();
    restoreActionRuns();
}

// Cheap half: a path, four runner objects and their connections. Runs in the
// constructor so m_actionStore is never null for the ~50 call sites that
// dereference it without a guard.
void MainWindow::initActionStore()
{
    if (m_actionStore)
        return;
    const QString root =
        forkmesh::paths::dataPath(QStringLiteral("actions"));
    m_actionStore = std::make_shared<ActionStore>(root);
    const int concurrency = qBound(2, QThread::idealThreadCount(), 4);
    for (int i = 0; i < concurrency; ++i) {
        auto *runner = new ActionRunner(m_actionStore.get(), this);
        connect(runner, &ActionRunner::logLine, this, &MainWindow::onRunLog);
        connect(runner, &ActionRunner::statusChanged, this,
                &MainWindow::onRunStatusChanged);
        connect(runner, &ActionRunner::stepChanged, this,
                &MainWindow::onRunStepChanged);
        connect(runner, &ActionRunner::finished, this,
                &MainWindow::onRunFinished);
        connect(runner, &ActionRunner::interrupted, this,
                &MainWindow::onRunInterrupted);
        connect(runner, &ActionRunner::releaseMetadataPrepared, this,
                &MainWindow::onReleaseMetadataPrepared);
        connect(runner, &ActionRunner::releaseMetadataLanded, this,
                &MainWindow::onReleaseMetadataLanded);
        m_actionRunners.append(runner);
    }
}

// Expensive half: reads every run record on disk, repairs runs the last exit
// interrupted, installs push hooks and starts the spool watcher. The scan and
// legacy migration touch every persisted meta.json, so they run on a worker;
// only the state/UI updates below return to the GUI thread.
void MainWindow::restoreActionRuns()
{
    if (m_actionRunsRestored || m_actionRunsRestoreInFlight)
        return;
    initActionStore();
    if (!m_actionStore)
        return;
    m_actionRunsRestoreInFlight = true;
    const auto restoreTimer = std::make_shared<QElapsedTimer>();
    restoreTimer->start();
    forkmesh::logStartupTrace(
        QStringLiteral("BEGIN deferred warm-up: restore Actions runs "
                       "(disk scan on worker)"));
    const std::shared_ptr<ActionStore> store = m_actionStore;
#ifdef FORKMESH_WINDOW_TESTS
    const std::function<void()> testWorkerHook =
        m_testActionRunsRestoreWorkerHook;
#endif
    auto *watcher = new QFutureWatcher<QList<ActionRun>>(this);
    connect(watcher, &QFutureWatcher<QList<ActionRun>>::finished, this,
            [this, watcher, restoreTimer] {
                QList<ActionRun> loaded = watcher->result();
                watcher->deleteLater();
                // The watcher is a MainWindow child and this connection is
                // context-bound to it, so no completion callback survives
                // window teardown. The worker itself captures only `store`.
                deliverOffThreadResult<QList<ActionRun>>(
                    [this, restoreTimer](QList<ActionRun> loaded) {
                        if (m_actionRunsRestored || !m_actionStore) {
                            m_actionRunsRestoreInFlight = false;
                            return;
                        }

                        m_actionRuns = std::move(loaded);
                        m_actionRunsRestored = true;

                        for (int i = 0; i < m_actionRuns.size(); ++i) {
                            ActionRun &run = m_actionRuns[i];
                            if (run.requeueAfterDesktopInterruption()) {
                                m_actionStore->appendLog(
                                    run,
                                    QString::fromUtf8(
                                        "\n==> \xE2\x9A\xA0 RESTARTING: ForkMesh exited while this "
                                        "run was active. The same approved commit will run again "
                                        "from the beginning.\n"));
                                m_actionStore->saveRun(run);
                                m_actionQueue.append(run.id);
                                logSystem(QStringLiteral(
                                              "Actions: re-queued run #%1 \"%2\" for %3/%4 @ %5 "
                                              "after ForkMesh exited; it will restart from its "
                                              "approved snapshot. Run log: %6")
                                              .arg(run.id)
                                              .arg(run.workflowName, run.owner,
                                                   run.name, run.commit.left(8),
                                                   actionRunLogPath(run)));
                            } else if (run.status == ActionStatus::Queued) {
                                m_actionQueue.append(run.id);
                            }
                        }

                        for (const RepositoryRecord &repo :
                             std::as_const(m_repositories))
                            autoApproveAwaitingRuns(repo);

                        installAllPushHooks();

                        m_actionSpoolWatcher = new QFileSystemWatcher(this);
                        m_actionSpoolWatcher->addPath(m_actionStore->spoolDir());
                        connect(m_actionSpoolWatcher,
                                &QFileSystemWatcher::directoryChanged, this,
                                [this](const QString &) { scanActionSpool(); });

                        auto *poll = new QTimer(this);
                        poll->setInterval(4000);
                        connect(poll, &QTimer::timeout, this,
                                &MainWindow::scanActionSpool);
                        poll->start();

                        scanActionSpool();
                        processActionQueue();
                        refreshActionsTable();
                        updateNotificationButton();

                        // A release action may have finished just before the
                        // controller exited. Replaying a tagged run with a
                        // durable finalization intent is safe:
                        // onReleaseMetadataLanded rechecks the intent and
                        // authenticates the exact tag/commit manifest before
                        // any host is touched.
                        QSet<QString> resumedReleaseFinalizations;
                        // ActionStore returns newest-first. Keep that order so
                        // an older successful republish cannot claim the key
                        // before the manifest that is currently in the tree.
                        for (auto it = m_actionRuns.cbegin();
                             it != m_actionRuns.cend(); ++it) {
                            const ActionRun &run = *it;
                            if (!run.ref.startsWith(
                                    QLatin1String("refs/tags/"))) {
                                continue;
                            }
                            const QString tag = run.ref.mid(
                                QStringLiteral("refs/tags/").size());
                            const QString key = run.owner.toLower() +
                                                QLatin1Char('/') +
                                                run.name.toLower();
                            const bool rolloutPending =
                                releaseRolloutPending(run.owner, run.name,
                                                      tag);
                            const bool publicationPending =
                                releasePublicationPending(
                                    run.owner, run.name, tag);
                            const bool prunePending =
                                releaseArtifactPrunePending(
                                    run.owner, run.name, tag);
                            if (run.status != ActionStatus::Success &&
                                !publicationPending) {
                                continue;
                            }
                            if (resumedReleaseFinalizations.contains(key) ||
                                (!rolloutPending && !publicationPending &&
                                 !prunePending)) {
                                continue;
                            }
                            resumedReleaseFinalizations.insert(key);
                            // Old versions kept rollout, publication and
                            // prune retries independently. If they crashed
                            // between releases, only the newest pending tag
                            // may control this repository now.
                            discardSupersededReleaseIntents(
                                run.owner, run.name, tag);
                            if (rolloutPending) {
                                // Backfill the publication gate for rollouts
                                // created by older desktop versions, and make
                                // a crash between ref publication and install
                                // retry the idempotent seed step first.
                                rememberReleasePublication(
                                    run.owner, run.name, tag);
                            }
                            const int runId = run.id;
                            QTimer::singleShot(
                                0, this,
                                [this, runId] {
                                    onReleaseMetadataLanded(runId);
                                });
                        }

                        // Finalization used to resume only above, at startup,
                        // so a release whose signed metadata landed after its
                        // Actions run (the release host completing the Android
                        // APK, a manual promotion) left the mirrors unseeded
                        // until the next restart. Re-check pending publications
                        // periodically. The pre-check authenticates the landed
                        // manifest first, so a release that is still held back
                        // stays quiet, and each tag is retried at most every
                        // ten minutes so a slow seeding pass is not doubled.
                        auto *releaseFinalizationRetry = new QTimer(this);
                        releaseFinalizationRetry->setInterval(3 * 60 * 1000);
                        const auto lastRetryMs =
                            std::make_shared<QHash<QString, qint64>>();
                        connect(
                            releaseFinalizationRetry, &QTimer::timeout, this,
                            [this, lastRetryMs] {
                                const qint64 nowMs =
                                    QDateTime::currentMSecsSinceEpoch();
                                QSet<QString> checkedRepositories;
                                for (auto it = m_actionRuns.cbegin();
                                     it != m_actionRuns.cend(); ++it) {
                                    const ActionRun &run = *it;
                                    if (run.status != ActionStatus::Success ||
                                        !run.ref.startsWith(
                                            QLatin1String("refs/tags/")))
                                        continue;
                                    const QString repoKey =
                                        run.owner.toLower() + QLatin1Char('/') +
                                        run.name.toLower();
                                    if (checkedRepositories.contains(repoKey))
                                        continue;
                                    const QString tag = run.ref.mid(
                                        QStringLiteral("refs/tags/").size());
                                    // A rollout-only intent is an install in
                                    // progress; replaying it would redeploy.
                                    if (!releasePublicationPending(
                                            run.owner, run.name, tag))
                                        continue;
                                    checkedRepositories.insert(repoKey);
                                    const QString tagKey =
                                        repoKey + QLatin1Char('@') + tag;
                                    if (nowMs - lastRetryMs->value(tagKey, 0) <
                                        10 * 60 * 1000)
                                        continue;
                                    const int index =
                                        repoIndexFor(run.owner, run.name);
                                    if (index < 0)
                                        continue;
                                    PublishedReleaseDeployment published;
                                    QString notYet;
                                    if (!publishedReleaseDeployment(
                                            index, tag, run.commit, &published,
                                            &notYet))
                                        continue;
                                    lastRetryMs->insert(tagKey, nowMs);
                                    logSystem(QStringLiteral(
                                                  "Release: %1's signed artifact "
                                                  "set is now published; "
                                                  "finishing its mirror "
                                                  "publication.")
                                                  .arg(tag));
                                    const int runId = run.id;
                                    QTimer::singleShot(0, this, [this, runId] {
                                        onReleaseMetadataLanded(runId);
                                    });
                                }
                            });
                        releaseFinalizationRetry->start();

                        forkmesh::logStartupTrace(
                            QStringLiteral("DONE  deferred warm-up: restore Actions runs "
                                           "(worker, %1ms)")
                                .arg(restoreTimer->elapsed()));
                        m_actionRunsRestoreInFlight = false;
                        drainDeferredActionWorkflowScans();
                    },
                    std::move(loaded));
            });
    watcher->setFuture(QtConcurrent::run(
        [store
#ifdef FORKMESH_WINDOW_TESTS
         , testWorkerHook
#endif
        ] {
#ifdef FORKMESH_WINDOW_TESTS
            if (testWorkerHook)
                testWorkerHook();
#endif
            const forkmesh::BackgroundScope activity(
                QStringLiteral("actions"),
                QStringLiteral("load persisted Actions history"),
                forkmesh::ActionTelemetry::Execution::Worker);
            // The exit that interrupted the runs above also left their sandbox
            // trees — a checkout, a clone and whatever the job built, close to
            // a gigabyte each — behind, and no runner claims them any more.
            // Reclaim them here rather than waiting for the next push: the node
            // shares this disk with every other build on the box.
            ActionRunner::reclaimSandboxes(store->sandboxDir());
            if (store->legacySandboxDir() != store->sandboxDir())
                ActionRunner::reclaimSandboxes(store->legacySandboxDir());
            return store->loadAllRuns();
        }));
}

void MainWindow::ensurePushHook(const RepositoryRecord &repo) const
{
    if (repo.previewOnly || repo.externallyManagedActions)
        return;
    // A private repository's mirrorPath is an authenticated, owner-only
    // materialization of an encrypted replica. It is replaced as epochs are
    // resealed and its origin is the short-lived import bundle, so it must
    // never become the source checkout's push target. Commits in the source
    // still wake the normal encrypted sync through the local signal hook.
    if (PrivateMirrorStore::isOpaqueId(repo.privateReplicaId)) {
        ensureCommitSignalHook(repo);
        return;
    }
    if (!m_actionStore || repo.mirrorPath.isEmpty())
        return;
    if (!QDir(repo.mirrorPath).exists())
        return; // mirror not cloned yet; installed on the next sync
    const QString hooksDir = repo.mirrorPath + QStringLiteral("/hooks");
    QDir().mkpath(hooksDir);

    // A small POSIX-sh post-receive hook: it appends one event file per push to
    // the spool dir (atomically via a.tmp rename) for the app to pick up.
    const QString spool = m_actionStore->spoolDir();
    const QString script = QStringLiteral(
        "#!/bin/sh\n"
        "spool='%1'\n"
        "mkdir -p \"$spool\"\n"
        "f=\"$spool/$(date +%s)-$$.push\"\n"
        "{\n"
        "  echo 'owner %2'\n"
        "  echo 'name %3'\n"
        "  echo 'mirror %4'\n"
        "  while read old new ref; do echo \"ref $old $new $ref\"; done\n"
        "} > \"$f.tmp\" && mv \"$f.tmp\" \"$f\"\n")
        .arg(spool, repo.owner, repo.name, repo.mirrorPath);

    // Runs for every repository at startup, on each sync and on each detail
    // open. Rewriting an identical hook is wasted I/O, and the push-URL check
    // behind it is a git process per call: skip both when nothing can have
    // changed (same script on disk, same .git/config as last verified).
    QFile f(hooksDir + QStringLiteral("/post-receive"));
    const QByteArray wanted = script.toUtf8();
    bool hookCurrent = false;
    if (f.open(QIODevice::ReadOnly)) {
        hookCurrent = f.readAll() == wanted &&
                      (f.permissions() & QFileDevice::ExeOwner);
        f.close();
    }
    if (!hookCurrent) {
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(wanted);
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                         QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                         QFileDevice::ExeGroup | QFileDevice::ReadOther |
                         QFileDevice::ExeOther);
    }

    if (!repo.localPath.trimmed().isEmpty() &&
        QFileInfo::exists(repo.localPath + QStringLiteral("/.git"))) {
        // remote.origin.pushurl lives in the checkout's git config; if that
        // file has not changed since the URL was last confirmed, it still is.
        const QString gitDir = resolveGitDirPath(repo.localPath);
        const QString configStamp =
            QString::number(QFileInfo(gitDir + QStringLiteral("/config"))
                                .lastModified()
                                .toMSecsSinceEpoch()) +
            QLatin1Char('|') + repo.mirrorPath;
        const QString localPath = repo.localPath;
        const QString mirrorPath = repo.mirrorPath;
        if (m_pushUrlVerified.value(localPath) != configStamp) {
            if (m_pushUrlVerifying.contains(localPath)) {
                if (m_pushUrlVerifying.value(localPath) != mirrorPath)
                    m_pushUrlRecheck.insert(localPath, repo);
            } else {
                m_pushUrlVerifying.insert(localPath, mirrorPath);
                auto *self = const_cast<MainWindow *>(this);
                self->runOffThread<QString>(
                    [localPath, mirrorPath, gitDir] {
                        const forkmesh::BackgroundScope activity(
                            QStringLiteral("actions"),
                            QStringLiteral("point %1 push URL at its mirror")
                                .arg(localPath),
                            forkmesh::ActionTelemetry::Execution::Worker);
                        QByteArray current;
                        runGitCapture(localPath,
                                      {QStringLiteral("remote"),
                                       QStringLiteral("get-url"),
                                       QStringLiteral("--push"),
                                       QStringLiteral("origin")},
                                      &current, nullptr);
                        if (QString::fromUtf8(current).trimmed() != mirrorPath)
                            runGitCapture(localPath,
                                          {QStringLiteral("remote"),
                                           QStringLiteral("set-url"),
                                           QStringLiteral("--push"),
                                           QStringLiteral("origin"), mirrorPath},
                                          nullptr, nullptr);
                        return QString::number(
                                   QFileInfo(gitDir + QStringLiteral("/config"))
                                       .lastModified()
                                       .toMSecsSinceEpoch()) +
                               QLatin1Char('|') + mirrorPath;
                    },
                    [self, localPath](QString stamp) {
                        self->m_pushUrlVerifying.remove(localPath);
                        self->m_pushUrlVerified.insert(localPath, stamp);
                        if (self->m_pushUrlRecheck.contains(localPath))
                            self->ensurePushHook(
                                self->m_pushUrlRecheck.take(localPath));
                    });
            }
        }
    }

    ensureCommitSignalHook(repo);
}

void MainWindow::ensureCommitSignalHook(const RepositoryRecord &repo) const
{
    // A commit made straight in the source-of-truth working copy (terminal,
    // IDE, a coding agent — including linked worktrees, which run the main
    // checkout's hooks) never touches the served bare mirror, so it only
    // reached the mirror — and the "mirror-update" broadcast to peers — at the
    // next periodic auto-sync tick. These hooks spool a ".commit" event the
    // moment HEAD moves; scanActionSpool turns it into propagateRepoUpdate,
    // which syncs the bare mirror and pushes the ephemeral "mirror-update"
    // frame out over the relay websocket so every mirror node fetches
    // immediately instead of waiting for its next heartbeat.
    if (repo.previewOnly || !m_actionStore)
        return;
    const QString localPath = repo.localPath.trimmed();
    if (localPath.isEmpty() ||
        !QFileInfo(localPath + QStringLiteral("/.git")).isDir())
        return;
    const QString hooksDir = localPath + QStringLiteral("/.git/hooks");
    QDir().mkpath(hooksDir);

    const QString spool = m_actionStore->spoolDir();
    const QString script =
        QStringLiteral("#!/bin/sh\n"
                       "%1\n"
                       "spool='%2'\n"
                       "mkdir -p \"$spool\"\n"
                       "f=\"$spool/$(date +%s)-$$.commit\"\n"
                       "{\n"
                       "  echo 'owner %3'\n"
                       "  echo 'name %4'\n"
                       "} > \"$f.tmp\" && mv \"$f.tmp\" \"$f\"\n")
            .arg(QLatin1String(kCommitSignalMarker), spool, repo.owner,
                 repo.name);

    for (const char *hook : {"post-commit", "post-merge"}) {
        const QString path = hooksDir + QLatin1Char('/') + QLatin1String(hook);
        QFile existing(path);
        if (existing.exists()) {
            if (!existing.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            const QString body = QString::fromUtf8(existing.readAll());
            existing.close();
            if (!body.contains(QLatin1String(kCommitSignalMarker)))
                continue;
            if (body == script)
                continue; // already current
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            continue;
        f.write(script.toUtf8());
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                         QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                         QFileDevice::ExeGroup | QFileDevice::ReadOther |
                         QFileDevice::ExeOther);
    }
}

void MainWindow::removePushHook(const RepositoryRecord &repo) const
{
    if (!repo.externallyManagedActions && !repo.mirrorPath.isEmpty())
        QFile::remove(repo.mirrorPath + QStringLiteral("/hooks/post-receive"));
    const QString localPath = repo.localPath.trimmed();
    if (localPath.isEmpty())
        return;
    for (const char *hook : {"post-commit", "post-merge"}) {
        const QString path = localPath + QStringLiteral("/.git/hooks/") +
                             QLatin1String(hook);
        QFile f(path);
        if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString body = QString::fromUtf8(f.readAll());
        f.close();
        if (body.contains(QLatin1String(kCommitSignalMarker)))
            QFile::remove(path);
    }
}

void MainWindow::installAllPushHooks() const
{
    for (const RepositoryRecord &repo : m_repositories)
        if (!repo.previewOnly)
            ensurePushHook(repo);
}

int MainWindow::repoIndexFor(const QString &owner, const QString &name) const
{
    for (int i = 0; i < m_repositories.size(); ++i)
        if (!m_repositories.at(i).previewOnly &&
            m_repositories.at(i).owner == owner &&
            m_repositories.at(i).name == name &&
            repositoryBelongsToActiveRelay(m_repositories.at(i)))
            return i;
    return -1;
}

ActionRun *MainWindow::findRun(int runId)
{
    for (ActionRun &run : m_actionRuns)
        if (run.id == runId)
            return &run;
    return nullptr;
}

void MainWindow::scanActionSpool()
{
    if (!m_actionStore)
        return;
    if (deferredOutOfKeepAlivePump(m_actionSpoolSweepPending,
                                   [this] { scanActionSpool(); }))
        return;
    syncMirrorActionsConfiguration();
    scanExternalActionsSources();
    QDir dir(m_actionStore->spoolDir());

    // ".commit" events: the working copy's post-commit/post-merge hook saw HEAD
    // move (a commit landed on the source of truth outside the app — terminal,
    // IDE, or an agent worktree). propagateRepoUpdate syncs the served bare
    // mirror from the working copy and, on a detected change, broadcasts the
    // ephemeral "mirror-update" websocket frame so every mirror node fetches
    // right now instead of at its next heartbeat.
    const QStringList commitFiles =
        dir.entryList({QStringLiteral("*.commit")}, QDir::Files, QDir::Name);
    QSet<int> propagated;
    for (const QString &file : commitFiles) {
        const QString full = dir.filePath(file);
        QFile f(full);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        f.close();

        QString owner, name;
        const QStringList lines =
            text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1String("owner ")))
                owner = line.mid(6).trimmed();
            else if (line.startsWith(QLatin1String("name ")))
                name = line.mid(5).trimmed();
        }
        const int idx = repoIndexFor(owner, name);
        if (idx < 0 ||
            m_repositories.at(idx).localPath.trimmed().isEmpty()) {
            QFile::remove(full); // stale event for a repo we no longer hold
            continue;
        }
        if (m_syncingRepos.contains(idx))
            continue;
        QFile::remove(full);
        if (propagated.contains(idx))
            continue; // a burst of commits needs only one sync
        propagated.insert(idx);
        propagateRepoUpdate(idx);
    }

    const QStringList files =
        dir.entryList({QStringLiteral("*.push")}, QDir::Files, QDir::Name);
    QSet<int> reattested;
    QHash<int, QString> pushedCommits;
    for (const QString &file : files) {
        const QString full = dir.filePath(file);
        QFile f(full);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        QFile::remove(full);

        QString owner, name, ref, commit, before;
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1String("owner ")))
                owner = line.mid(6).trimmed();
            else if (line.startsWith(QLatin1String("name ")))
                name = line.mid(5).trimmed();
            else if (line.startsWith(QLatin1String("ref "))) {
                const QStringList p =
                    line.mid(4).split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (p.size() >= 3 &&
                    p.at(2).startsWith(QLatin1String("refs/heads/"))) {
                    before = p.at(0);
                    commit = p.at(1);
                    ref = p.at(2);
                }
            }
        }
        // A push landed on a local mirror. Refresh the managed gateway and
        // notify mirror services; only the headless mirror daemon publishes
        // catalog state.
        if (!owner.isEmpty() && !name.isEmpty()) {
            const int idx = repoIndexFor(owner, name);
            if (idx >= 0 && !reattested.contains(idx)) {
                reattested.insert(idx);
                pushToSshMirrorRemotes(idx);
            }
        }
        if (owner.isEmpty() || name.isEmpty() || commit.isEmpty())
            continue;
        if (commit.count(QLatin1Char('0')) == commit.size())
            continue;
        const int pushedIndex = repoIndexFor(owner, name);
        if (pushedIndex >= 0)
            pushedCommits.insert(pushedIndex, commit.toLower());
        enqueuePushEvent(owner, name, commit, ref, before);
    }
    scanServedMirrorHeads(pushedCommits);
    processActionQueue();
    updateMirrorActionsRuntimeState();
}

void MainWindow::scanServedMirrorHeads(const QHash<int, QString> &pushedCommits)
{
    if (!m_actionStore)
        return;

    for (auto it = pushedCommits.cbegin(); it != pushedCommits.cend(); ++it)
        m_servedMirrorHeadPushedCommits[it.key()].insert(it.value());
    m_servedMirrorHeadScanPending = true;
    if (m_servedMirrorHeadScanInFlight)
        return;

    m_servedMirrorHeadScanPending = false;
    QList<ServedMirrorHeadProbe> probes;
    for (int index = 0; index < m_repositories.size(); ++index) {
        const RepositoryRecord repo = m_repositories.at(index);
        if (repo.previewOnly || !repo.actionsEnabled ||
            repo.externallyManagedActions ||
            repo.localPath.trimmed().isEmpty() ||
            repo.mirrorPath.trimmed().isEmpty() ||
            !QDir(repo.mirrorPath).exists())
            continue;
        probes.append(
            ServedMirrorHeadProbe{index, repo.owner, repo.name, repo.mirrorPath});
    }
    if (probes.isEmpty()) {
        m_servedMirrorHeadPushedCommits.clear();
        return;
    }

    m_servedMirrorHeadScanInFlight = true;
    const quint64 generation = ++m_servedMirrorHeadScanGeneration;
    auto *watcher = new QFutureWatcher<QList<ServedMirrorHeadResult>>(this);
    connect(watcher, &QFutureWatcher<QList<ServedMirrorHeadResult>>::finished,
            this, [this, watcher, generation] {
        const QList<ServedMirrorHeadResult> results = watcher->result();
        watcher->deleteLater();
        if (generation != m_servedMirrorHeadScanGeneration || !m_actionStore) {
            finishServedMirrorHeadScan(false);
            return;
        }

        QList<ServedMirrorHeadPersistence> persistence;
        for (const ServedMirrorHeadResult &result : results) {
            if (result.ref.isEmpty() || result.commit.isEmpty())
                continue; // mid-reseal materialization, or an unborn branch
            if (result.index < 0 || result.index >= m_repositories.size())
                continue;
            const RepositoryRecord repo = m_repositories.at(result.index);
            if (repo.owner != result.owner || repo.name != result.name ||
                repo.mirrorPath != result.mirrorPath || repo.previewOnly ||
                !repo.actionsEnabled || repo.externallyManagedActions ||
                repo.localPath.trimmed().isEmpty()) {
                continue; // repository list/config changed while probing
            }
            persistence.append(
                ServedMirrorHeadPersistence{result, servedActionHeadKey(repo)});
        }
        if (persistence.isEmpty()) {
            finishServedMirrorHeadScan(false);
            return;
        }

        // QSettings::sync can reach fdatasync. Persist on the worker before
        // enqueueing so the record-before-run guarantee remains durable without
        // ever stalling the GUI callback.
        auto *persistWatcher =
            new QFutureWatcher<QList<PersistedServedMirrorHead>>(this);
        connect(persistWatcher,
                &QFutureWatcher<QList<PersistedServedMirrorHead>>::finished,
                this, [this, persistWatcher, generation] {
            const QList<PersistedServedMirrorHead> persisted =
                persistWatcher->result();
            persistWatcher->deleteLater();
            bool queued = false;
            if (generation == m_servedMirrorHeadScanGeneration && m_actionStore) {
                for (const PersistedServedMirrorHead &item : persisted) {
                    const ServedMirrorHeadResult &result = item.result;
                    if (result.index < 0 ||
                        result.index >= m_repositories.size()) {
                        continue;
                    }
                    const RepositoryRecord repo =
                        m_repositories.at(result.index);
                    if (repo.owner != result.owner || repo.name != result.name ||
                        repo.mirrorPath != result.mirrorPath || repo.previewOnly ||
                        !repo.actionsEnabled || repo.externallyManagedActions ||
                        repo.localPath.trimmed().isEmpty()) {
                        continue;
                    }
                    if (item.previousCommit.isEmpty() ||
                        item.previousCommit == result.commit) {
                        continue;
                    }
                    if (m_servedMirrorHeadPushedCommits
                            .value(result.index)
                            .contains(result.commit)) {
                        continue;
                    }
                    enqueuePushEvent(repo.owner, repo.name, result.commit,
                                     result.ref, item.previousCommit);
                    queued = true;
                }
            }
            finishServedMirrorHeadScan(queued);
        });
        persistWatcher->setFuture(QtConcurrent::run(
            [persistence = std::move(persistence)] {
                QList<PersistedServedMirrorHead> persisted;
                persisted.reserve(persistence.size());
                QSettings settings;
                bool changed = false;
                for (const ServedMirrorHeadPersistence &item : persistence) {
                    const QString previous =
                        settings.value(item.settingsKey)
                            .toString()
                            .trimmed()
                            .toLower();
                    if (previous != item.result.commit) {
                        settings.setValue(item.settingsKey, item.result.commit);
                        changed = true;
                    }
                    persisted.append(
                        PersistedServedMirrorHead{item.result, previous});
                }
                if (changed)
                    settings.sync();
                return persisted;
            }));
    });
    watcher->setFuture(QtConcurrent::run([probes = std::move(probes)] {
        QList<ServedMirrorHeadResult> results;
        results.reserve(probes.size());
        for (const ServedMirrorHeadProbe &probe : probes) {
            ServedMirrorHeadResult result{probe.index, probe.owner, probe.name,
                                          probe.mirrorPath, {}, {}};
            result.ref = gitHeadRef(probe.mirrorPath);
            if (!result.ref.isEmpty())
                result.commit = gitCommitAt(probe.mirrorPath, result.ref);
            results.append(std::move(result));
        }
        return results;
    }));
}

void MainWindow::finishServedMirrorHeadScan(bool queued)
{
    m_servedMirrorHeadScanInFlight = false;
    if (queued) {
        processActionQueue();
        updateMirrorActionsRuntimeState();
    }
    if (m_servedMirrorHeadScanPending) {
        QTimer::singleShot(0, this, [this] { scanServedMirrorHeads({}); });
    } else {
        m_servedMirrorHeadPushedCommits.clear();
    }
}

void MainWindow::syncMirrorActionsConfiguration()
{
    // Startup could not recover an interrupted transaction: the settings may
    // be half-applied, so mirror Actions stay off until Retry/Reset succeeds.
    if (forkmesh::mirror_actions::actionsSuspendedByRecoveryFailure()) {
        for (RepositoryRecord &repo : m_repositories) {
            if (repo.externallyManagedActions)
                repo.actionsEnabled = false;
        }
        m_mirrorActionsConfigGeneration.clear();
        return;
    }
    QSettings settings;
    const QString generation =
        settings
            .value(QString::fromLatin1(
                forkmesh::mirror_actions::kGenerationSetting))
            .toString()
            .trimmed();
    if (generation.isEmpty() ||
        generation == m_mirrorActionsConfigGeneration)
        return;
    static const QRegularExpression validGeneration(
        QStringLiteral("^[a-f0-9]{32}$"));
    if (!validGeneration.match(generation).hasMatch())
        return;

    loadRepositories();
    installAllPushHooks();
    const bool enabled =
        settings
            .value(QString::fromLatin1(
                       forkmesh::mirror_actions::kEnabledSetting),
                   false)
            .toBool();
    for (RepositoryRecord &repo : m_repositories) {
        if (repo.externallyManagedActions)
            repo.actionsEnabled = enabled;
    }
    m_mirrorActionsConfigGeneration = generation;
    m_mirrorActionsRuntimeState.clear();
    m_mirrorActionsRuntimeStateWrittenAtMs = 0;
    m_mirrorActionsSummaryAttemptedAtMs = 0;
    scheduleMirrorActionsSummary(0);
    logSystem(
        QStringLiteral("Actions: mirror executor configuration %1.")
            .arg(enabled ? QStringLiteral("enabled")
                         : QStringLiteral("disabled")));
}

void MainWindow::scanExternalActionsSources()
{
    if (forkmesh::mirror_actions::actionsSuspendedByRecoveryFailure())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastExternalActionsScanMs < 3500)
        return;
    struct ExternalActionsProbe {
        RepositoryRecord repo;
        QString settingsKey;
    };
    struct ExternalActionsPush {
        QString owner;
        QString name;
        QString commit;
        QString ref;
        QString before;
    };
    QList<ExternalActionsProbe> probes;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (!repo.externallyManagedActions || !repo.actionsEnabled ||
            repo.previewOnly ||
            !QFileInfo(repo.externalActionsSource).isDir() ||
            QFileInfo(repo.externalActionsSource).isSymLink() ||
            !QFileInfo(repo.mirrorPath).isDir() ||
            QFileInfo(repo.mirrorPath).isSymLink() ||
            !kExternalActionRef.match(repo.externalActionsRef).hasMatch()) {
            continue;
        }
        probes.append(ExternalActionsProbe{repo, externalActionHeadKey(repo)});
    }
    if (probes.isEmpty()) {
        m_lastExternalActionsScanMs = now;
        return;
    }

    // The rev-parses and the fetch (up to 30 s) run on a worker. Holding the
    // throttle stamp in the future keeps a slow scan from overlapping the next
    // spool pass; the result restores the stamp to this scan's start.
    m_lastExternalActionsScanMs = std::numeric_limits<qint64>::max();
    runOffThread<QList<ExternalActionsPush>>(
        [probes = std::move(probes)]() -> QList<ExternalActionsPush> {
            QList<ExternalActionsPush> pushes;
            QSettings settings;
            for (const ExternalActionsProbe &probe : probes) {
                const RepositoryRecord &repo = probe.repo;
                const QString sourceCommit =
                    gitCommitAt(repo.externalActionsSource,
                                repo.externalActionsRef);
                if (sourceCommit.isEmpty())
                    continue;
                const QString previous = settings.value(probe.settingsKey)
                                             .toString()
                                             .trimmed()
                                             .toLower();
                if (previous.isEmpty()) {
                    settings.setValue(probe.settingsKey, sourceCommit);
                    continue;
                }
                if (previous == sourceCommit)
                    continue;
                if (!fetchExternalActionRef(repo) ||
                    gitCommitAt(repo.mirrorPath, repo.externalActionsRef) !=
                        sourceCommit) {
                    continue;
                }
                settings.setValue(probe.settingsKey, sourceCommit);
                pushes.append(ExternalActionsPush{repo.owner, repo.name,
                                                  sourceCommit,
                                                  repo.externalActionsRef,
                                                  previous});
            }
            settings.sync();
            return pushes;
        },
        [this, now](QList<ExternalActionsPush> pushes) {
            m_lastExternalActionsScanMs = now;
            for (const ExternalActionsPush &push : std::as_const(pushes))
                enqueuePushEvent(push.owner, push.name, push.commit, push.ref,
                                 push.before);
            if (!pushes.isEmpty()) {
                processActionQueue();
                updateMirrorActionsRuntimeState();
            }
        });
}

void MainWindow::updateMirrorActionsRuntimeState()
{
    if (m_mirrorActionsConfigGeneration.isEmpty())
        return;
    QSettings settings;
    const bool enabled =
        settings
            .value(QString::fromLatin1(
                       forkmesh::mirror_actions::kEnabledSetting),
                   false)
            .toBool();
    bool running = false;
    for (const ActionRunner *runner : std::as_const(m_actionRunners)) {
        if (runner && runner->busy()) {
            running = true;
            break;
        }
    }
    const QString state =
        !enabled ? QStringLiteral("disabled")
                 : running ? QStringLiteral("running")
                           : QStringLiteral("enabled");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_mirrorActionsSummaryAttemptedAtMs >=
        3 * 60 * 1000) {
        scheduleMirrorActionsSummary(0);
    }
    const bool changed = state != m_mirrorActionsRuntimeState;
    if (!changed &&
        now - m_mirrorActionsRuntimeStateWrittenAtMs < 3 * 60 * 1000)
        return;
    const QString path =
        settings.value(QStringLiteral("actions/mirrorStatePath"))
            .toString()
            .trimmed();
    const QString node =
        settings
            .value(QString::fromLatin1(
                forkmesh::mirror_actions::kNodeSetting))
            .toString()
            .trimmed();
    if (!forkmesh::mirror_actions::writeStateFile(
            path, node, state, now))
        return;
    m_mirrorActionsRuntimeState = state;
    m_mirrorActionsRuntimeStateWrittenAtMs = now;

#if defined(Q_OS_UNIX)
    if (changed) {
        forkmesh::processreaper::startReleased(
            QStringLiteral("/usr/bin/systemctl"),
            {QStringLiteral("--no-block"), QStringLiteral("start"),
             QStringLiteral("forkmesh-mirror-renew.service")});
    }
#endif
}

void MainWindow::scheduleMirrorActionsSummary(int delayMs)
{
    if (m_mirrorActionsConfigGeneration.isEmpty() || !m_actionStore)
        return;
    if (!m_mirrorActionsSummaryTimer) {
        m_mirrorActionsSummaryTimer = new QTimer(this);
        m_mirrorActionsSummaryTimer->setSingleShot(true);
        connect(m_mirrorActionsSummaryTimer, &QTimer::timeout, this,
                &MainWindow::writeMirrorActionsSummary);
    }
    const int boundedDelay = qBound(0, delayMs, 3000);
    if (m_mirrorActionsSummaryTimer->isActive()) {
        const int remaining = m_mirrorActionsSummaryTimer->remainingTime();
        if (remaining >= 0 && remaining <= boundedDelay)
            return;
    }
    m_mirrorActionsSummaryTimer->start(boundedDelay);
}

void MainWindow::writeMirrorActionsSummary()
{
    if (m_mirrorActionsConfigGeneration.isEmpty() || !m_actionStore)
        return;
    QSettings settings;
    const QString path =
        settings
            .value(QString::fromLatin1(
                forkmesh::mirror_actions::kSummaryPathSetting))
            .toString()
            .trimmed();
    const QString node =
        settings
            .value(QString::fromLatin1(
                forkmesh::mirror_actions::kNodeSetting))
            .toString()
            .trimmed();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_mirrorActionsSummaryAttemptedAtMs = now;
    forkmesh::mirror_actions::writeSummaryFile(
        path, node, m_actionRuns, *m_actionStore, now);
}

// code-health: allow(blocking-wait) the subject lookup's git show runs in runOffThread
void MainWindow::enqueuePushEvent(const QString &owner, const QString &name,
                                  const QString &commit, const QString &ref,
                                  const QString &before)
{
    const int lookupIndex = repoIndexFor(owner, name);
    if (lookupIndex < 0)
        return;
    const QString mirrorPath = m_repositories.at(lookupIndex).mirrorPath;

    // The commit subject only decorates the log line and the alert, but
    // `git show` on the GUI thread is a stall; read it on a worker and carry on
    // once it is back. The repository list may be rebuilt meanwhile, so the
    // record is looked up again there.
    runOffThread<QString>(
        [mirrorPath, commit]() -> QString {
            QProcess s;
            s.start(QStringLiteral("git"),
                    {QStringLiteral("-C"), mirrorPath, QStringLiteral("show"),
                     QStringLiteral("-s"), QStringLiteral("--format=%s"), commit});
            s.waitForFinished(5000);
            return QString::fromUtf8(s.readAllStandardOutput()).trimmed();
        },
        [this, owner, name, commit, ref, before](QString subject) {
            const int repoIndex = repoIndexFor(owner, name);
            if (repoIndex < 0)
                return;
            const RepositoryRecord repo = m_repositories.at(repoIndex);

            const QString branch = ref.startsWith(QLatin1String("refs/heads/"))
                                       ? ref.mid(11)
                                       : ref;

            logSystem(
                QString::fromUtf8("Push to %1/%2 on %3 \xE2\x86\x92 %4%5")
                    .arg(owner, name, branch, commit.left(8),
                         subject.isEmpty()
                             ? QString()
                             : QString::fromUtf8(" \xE2\x80\x94 ") +
                                   subject));

            if (QSettings().value(kPushAlertSetting, false).toBool()) {
                const QString body =
                    QString::fromUtf8("%1/%2 \xC2\xB7 %3 \xC2\xB7 %4%5")
                        .arg(owner, name, branch, commit.left(8),
                             subject.isEmpty()
                                 ? QString()
                                 : QStringLiteral("\n") + subject);
                postNotification(QStringLiteral("Push received"), body,
                                 false, QStringLiteral("emblem-synchronizing"));
            }

            if (repoIndex == m_repoDetailIndex)
                scheduleOpenRepoDetailRefresh();

            const QString explicitKey =
                owner + QLatin1Char('\x1f') + name + QLatin1Char('\x1f') +
                commit.trimmed().toLower();
            if (m_explicitActionPushes.remove(explicitKey)) {
                logSystem(QStringLiteral(
                              "Actions: checks for %1/%2 @ %3 were already "
                              "queued by pull-request creation.")
                              .arg(owner, name, commit.left(8)));
                return;
            }

            // Push detection only; no workflow execution for this repo.
            if (!repo.actionsEnabled)
                return;

            queueWorkflowsForCommit(repoIndex, owner, name, commit, ref,
                                    WorkflowTrigger::Push, ActionPullContext(),
                                    before);
        });
}

void MainWindow::queueWorkflowsForCommit(int repoIndex, const QString &owner,
                                         const QString &name,
                                         const QString &commit,
                                         const QString &ref,
                                         WorkflowTrigger trigger)
{
    queueWorkflowsForCommit(repoIndex, owner, name, commit, ref, trigger,
                            ActionPullContext());
}

// code-health: allow(blocking-wait) the git capture waits are inside the runOffThread workflow scan
void MainWindow::queueWorkflowsForCommit(int repoIndex, const QString &owner,
                                         const QString &name, const QString &commit,
                                         const QString &ref,
                                         WorkflowTrigger trigger,
                                         ActionPullContext pull,
                                         const QString &before)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size() || !m_actionStore)
        return;
    const QString mirrorPath = m_repositories.at(repoIndex).mirrorPath;
    const QString ownerKey = owner;
    const QString nameKey = name;
    const QString commitKey = commit;
    const QString refKey = ref;
    const QString beforeKey =
        trigger == WorkflowTrigger::Push ? before.trimmed().toLower()
                                         : QString();

    // Only a branch name: it is interpolated into a git revision, and it comes
    // from a signed pull request written by somebody else.
    static const QRegularExpression safeBranch(
        QStringLiteral("^[A-Za-z0-9._/-]{1,200}$"));
    const QString baseRef =
        (trigger == WorkflowTrigger::PullRequest ||
         trigger == WorkflowTrigger::PullRequestManual) &&
                safeBranch.match(pull.base.trimmed()).hasMatch()
            ? QStringLiteral("refs/heads/") + pull.base.trimmed()
            : QString();

    runOffThread<WorkflowScan>(
        [mirrorPath, commitKey, refKey, trigger, baseRef, beforeKey]() -> WorkflowScan {
            WorkflowScan scan;
            scan.pushBefore = beforeKey;
            if (!QDir(mirrorPath).exists()) {
                scan.mirrorMissing = true;
                return scan;
            }
            const QStringList base{QStringLiteral("-C"), mirrorPath};
            auto capture = [&base](const QStringList &args) -> QString {
                QProcess process;
                process.setProcessChannelMode(QProcess::SeparateChannels);
                process.start(QStringLiteral("git"), base + args);
                if (!process.waitForStarted(3000) ||
                    !process.waitForFinished(10000)) {
                    process.kill();
                    process.waitForFinished(1000);
                    return {};
                }
                if (process.exitCode() != 0)
                    return {};
                return QString::fromUtf8(process.readAllStandardOutput());
            };
            if (trigger == WorkflowTrigger::Push) {
                scan.changedPaths = forkmesh::actions::pushRangeChangedPaths(
                    mirrorPath, beforeKey, commitKey);
                if (forkmesh::actions::isMetadataOnlyChange(
                        scan.changedPaths)) {
                    scan.metadataOnly = true;
                    return scan;
                }
            }

            QString defaultRef;
            if (trigger == WorkflowTrigger::Push) {
                defaultRef = capture({QStringLiteral("symbolic-ref"),
                                      QStringLiteral("--quiet"),
                                      QStringLiteral("HEAD")})
                                 .trimmed();
                if (defaultRef == refKey ||
                    !defaultRef.startsWith(QLatin1String("refs/heads/")))
                    defaultRef.clear();
            }

            const QStringList paths =
                capture({QStringLiteral("ls-tree"), QStringLiteral("-r"),
                         QStringLiteral("--name-only"), commitKey,
                         QStringLiteral("--"), QStringLiteral(".forkmesh")})
                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &path : paths) {
                if (!(path.endsWith(QLatin1String(".yml")) ||
                      path.endsWith(QLatin1String(".yaml"))))
                    continue;
                const QString content =
                    capture({QStringLiteral("show"),
                             commitKey + QLatin1Char(':') + path});
                if (content.isEmpty())
                    continue;
                WorkflowScanEntry entry;
                entry.path = path;
                entry.content = content;
                if (!baseRef.isEmpty()) {
                    // What the workflow *does* is still the head's copy — that
                    // is the tree the runner checks out and the maintainer
                    // approves. Only the "does this run on pull requests?"
                    // answer is taken from the base branch.
                    const QString baseContent =
                        capture({QStringLiteral("show"),
                                 baseRef + QLatin1Char(':') + path});
                    entry.baseTriggersPullRequest =
                        !baseContent.isEmpty() &&
                        ActionFile::parse(path, baseContent)
                            .triggersOnPullRequest();
                }
                if (!defaultRef.isEmpty()) {
                    const QString defaultContent =
                        capture({QStringLiteral("show"),
                                 defaultRef + QLatin1Char(':') + path});
                    entry.defaultBranchRetiresPush =
                        ActionFile::defaultBranchRetiresPush(path, defaultContent);
                }
                scan.workflows.append(entry);
            }
            if (!scan.workflows.isEmpty()) {
                scan.snapshotOk = ActionStore::repositoryStateDigest(
                    mirrorPath, commitKey, &scan.repositoryTree,
                    &scan.executionDigest, &scan.snapshotError);
            }
            return scan;
        },
        [this, ownerKey, nameKey, commitKey, refKey, trigger,
         pull](WorkflowScan scan) {
            applyWorkflowScan(ownerKey, nameKey, commitKey, refKey, trigger,
                              pull, scan);
        });
}

void MainWindow::applyWorkflowScan(const QString &owner, const QString &name,
                                   const QString &commit, const QString &ref,
                                   WorkflowTrigger trigger,
                                   const ActionPullContext &pull,
                                   const WorkflowScan &scan)
{
    if (!m_actionStore)
        return;
    if (m_actionRunsRestoreInFlight) {
        // ActionStore holds its cache mutex while the history worker migrates
        // and scans every run directory. Applying this result would call
        // createRun() on the GUI thread, so retain it until that scan is done.
        DeferredActionWorkflowScan deferred;
        deferred.owner = owner;
        deferred.name = name;
        deferred.commit = commit;
        deferred.ref = ref;
        deferred.trigger = trigger;
        deferred.pull = pull;
        deferred.scan = scan;
        m_deferredActionWorkflowScans.append(std::move(deferred));
        return;
    }
    const int repoIndex = repoIndexFor(owner, name);
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(repoIndex);

    if (scan.mirrorMissing) {
        logSystem(QString::fromUtf8(
                      "Actions: served mirror %1 for %2/%3 is missing \xE2\x80\x94 "
                      "cannot look up workflows at %4.")
                      .arg(repo.mirrorPath, owner, name, commit.left(8)));
        return;
    }
    if (scan.metadataOnly) {
        logSystem(QString::fromUtf8(
                      "Actions: %1/%2 @ %3 only touches issues/PRs \xE2\x80\x94 "
                      "skipping workflows.")
                      .arg(owner, name, commit.left(8)));
        return;
    }
    if (!scan.workflows.isEmpty() && !scan.snapshotOk &&
        forkmesh::terminationSignalReceived()) {
        const bool spooled =
            trigger == WorkflowTrigger::Push &&
            m_actionStore->spoolPushEvent(owner, name, commit, ref,
                                          scan.pushBefore);
        logSystem(spooled
                      ? QStringLiteral(
                            "Actions: ForkMesh is stopping, so the push to "
                            "%1/%2 on %3 @ %4 was not bound (%5); it is saved "
                            "and its workflows are queued when ForkMesh starts "
                            "again.")
                            .arg(owner, name, ref, commit.left(8),
                                 scan.snapshotError)
                      : QStringLiteral(
                            "Actions: ForkMesh is stopping, so workflows for "
                            "%1/%2 @ %3 on %4 were not queued (%5). Use Run "
                            "workflow to run them after it starts again.")
                            .arg(owner, name, commit.left(8), ref,
                                 scan.snapshotError));
        return;
    }

    bool added = false;
    QList<QPair<QString, int>> held;
    for (const WorkflowScanEntry &entry : scan.workflows) {
        const QString &path = entry.path;
        const QString &content = entry.content;
        const ActionWorkflow wf = ActionFile::parse(path, content);
        const bool runsOnPullRequests =
            wf.triggersOnPullRequest() || entry.baseTriggersPullRequest;
        bool matchesTrigger = false;
        switch (trigger) {
        case WorkflowTrigger::Release:
            matchesTrigger = wf.triggersOnRelease();
            break;
        case WorkflowTrigger::PullRequest:
            matchesTrigger = runsOnPullRequests;
            break;
        case WorkflowTrigger::PullRequestManual:
            matchesTrigger = runsOnPullRequests || wf.triggersOnPush();
            break;
        case WorkflowTrigger::Push:
            matchesTrigger = wf.triggersOnPush();
            break;
        }
        if (!wf.valid || !matchesTrigger)
            continue;
        if (trigger == WorkflowTrigger::Push && entry.defaultBranchRetiresPush) {
            logSystem(QString::fromUtf8(
                "Actions: \xE2\x80\x9C%1\xE2\x80\x9D no longer runs on push on"
                " the default branch, so this push to %2 does not run it.")
                .arg(wf.name, ref));
            continue;
        }
        if (trigger == WorkflowTrigger::Push &&
            !wf.matchesPushChangedPaths(scan.changedPaths)) {
            logSystem(QString::fromUtf8(
                "Actions: \xE2\x80\x9C%1\xE2\x80\x9D did not match the"
                " workflow's on.push.paths filter for this push (%2).")
                .arg(wf.name,
                     forkmesh::actions::isKnownPushBase(scan.pushBefore)
                         ? scan.pushBefore.left(8) + QStringLiteral("..") +
                               commit.left(8)
                         : commit.left(8) +
                               QStringLiteral(" against its parents")));
            continue;
        }
        if (repo.disabledWorkflows.contains(path)) {
            logSystem(QString::fromUtf8("Actions: \xE2\x80\x9C%1\xE2\x80\x9D is "
                                        "disabled for %2/%3 \xE2\x80\x94 skipping.")
                          .arg(wf.name, owner, name));
            continue;
        }
        if (!workflowRunsOnThisNode(repo, wf)) {
            logSystem(QString::fromUtf8(
                          "Actions: \xE2\x80\x9C%1\xE2\x80\x9D is dedicated to "
                          "%2 \xE2\x80\x94 this node (%3) is skipping it.")
                          .arg(wf.name, workflowDedicationLabel(repo, wf),
                               actionNodeLabels().join(QStringLiteral(", "))));
            continue;
        }

        ActionRun run;
        run.owner = owner;
        run.name = name;
        run.workflowPath = path;
        run.workflowName = wf.name;
        run.workflowContent = content;
        run.commit = commit;
        run.ref = ref;
        run.pullNumber = pull.number;
        run.pullAuthor = pull.author;
        run.firstTimePullAuthor = pull.number > 0 && pull.firstTimeAuthor;
        if (!scan.snapshotOk) {
            logSystem(QStringLiteral(
                          "Actions: could not bind \"%1\" to repository state "
                          "for %2/%3 @ %4: %5")
                          .arg(wf.name, owner, name, commit.left(8),
                               scan.snapshotError));
            continue;
        }
        run.repositoryTree = scan.repositoryTree;
        run.executionDigest = scan.executionDigest;
        const bool approved = resolveActionApproval(repo, run);
        run.status =
            approved ? ActionStatus::Queued : ActionStatus::AwaitingApproval;

        const ActionRun created = m_actionStore->createRun(run);
        m_actionRuns.prepend(created);
        scheduleMirrorActionsSummary(0);
        cancelSupersededRuns(created);
        if (approved)
            m_actionQueue.append(created.id);
        else
            held.append({wf.name, created.id});
        added = true;
        logSystem(QStringLiteral("Actions: %1 \"%2\" for %3/%4 @ %5%6")
                      .arg(approved ? QStringLiteral("queued")
                                    : QStringLiteral("awaiting approval of"),
                           wf.name, owner, name, commit.left(8),
                           run.pullNumber > 0
                               ? QStringLiteral(" (pull request #%1)")
                                     .arg(run.pullNumber)
                               : QString()));
    }
    if (!held.isEmpty())
        notifyHeldActionRuns(owner, name, commit, pull, held);
    if (added) {
        refreshActionsTable();
        if (m_actionWorkflowList && repoIndex == m_repoDetailIndex)
            refreshRepoActions();
        updateNotificationButton();
        refreshOpenPullChecks(); // the PR's Checks tab, if it is the open one
        processActionQueue(); // a manual run isn't driven by the push pipeline
    } else {
        QString event = QStringLiteral("push");
        if (trigger == WorkflowTrigger::Release)
            event = QStringLiteral("release");
        else if (trigger == WorkflowTrigger::PullRequest)
            event = QStringLiteral("pull_request");
        else if (trigger == WorkflowTrigger::PullRequestManual)
            event = QStringLiteral("pull_request' or 'push");
        logSystem(QString::fromUtf8(
                      "Actions: no .forkmesh/ workflow with 'on: %1' at %2 for "
                      "%3/%4 \xE2\x80\x94 nothing to run.")
                      .arg(event, commit.left(8), owner, name));
    }
}

void MainWindow::drainDeferredActionWorkflowScans()
{
    if (m_actionRunsRestoreInFlight || m_deferredActionWorkflowScans.isEmpty())
        return;
    QList<DeferredActionWorkflowScan> deferred;
    deferred.swap(m_deferredActionWorkflowScans);
    for (const DeferredActionWorkflowScan &pending :
         std::as_const(deferred)) {
        applyWorkflowScan(pending.owner, pending.name, pending.commit,
                          pending.ref, pending.trigger, pending.pull,
                          pending.scan);
    }
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testQueueActionWorkflowScanDuringRestore()
{
    WorkflowScan scan;
    applyWorkflowScan(QStringLiteral("test-owner"),
                      QStringLiteral("test-repository"),
                      QStringLiteral("test-commit"), QString(),
                      WorkflowTrigger::Push, ActionPullContext(), scan);
}

int MainWindow::testDeferredActionWorkflowScanCount() const
{
    return m_deferredActionWorkflowScans.size();
}

void MainWindow::testSeedActionRuns(const QList<ActionRun> &runs)
{
    m_actionRuns = runs;
    m_actionsTableSignature.clear(); // the seeded rows are new to the table
    refreshActionsTable();
}

void MainWindow::testSelectActionRunRows(const QList<int> &rows)
{
    if (!m_actionsTable || !m_actionsTable->selectionModel())
        return;
    // Signals stay live here: the point is to drive the same handler a
    // ctrl-click runs through.
    m_actionsTable->clearSelection();
    for (int row : rows) {
        m_actionsTable->selectionModel()->select(
            m_actionsTable->model()->index(row, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
}

QStringList MainWindow::testParallelRunControls(int runId) const
{
    QStringList shown;
    for (const ParallelRunColumn &column : m_parallelRunColumns) {
        if (column.runId != runId)
            continue;
        const QList<QPair<QPushButton *, QString>> buttons{
            {column.stopButton, QStringLiteral("stop")},
            {column.skipButton, QStringLiteral("skip")},
            {column.copyLogButton, QStringLiteral("copy-log")},
            {column.copyLogPathButton, QStringLiteral("copy-path")},
            {column.sendToAgentButton, QStringLiteral("send-to-agent")},
            {column.rerunButton, QStringLiteral("rerun")}};
        for (const auto &button : buttons) {
            if (button.first && !button.first->isHidden())
                shown.append(button.second);
        }
    }
    return shown;
}

QStringList MainWindow::testActionRunControls() const
{
    QStringList shown;
    const QList<QPair<QPushButton *, QString>> buttons{
        {m_actionStopButton, QStringLiteral("stop")},
        {m_actionSkipButton, QStringLiteral("skip")},
        {m_actionCopyLogButton, QStringLiteral("copy-log")},
        {m_actionCopyLogPathButton, QStringLiteral("copy-path")},
        {m_actionSendToAgentButton, QStringLiteral("send-to-agent")},
        {m_actionRerunButton, QStringLiteral("rerun")}};
    for (const auto &button : buttons) {
        if (button.first && !button.first->isHidden())
            shown.append(button.second);
    }
    return shown;
}

bool MainWindow::testActionRunSpinning(int runId) const
{
    if (runId == m_selectedRunId)
        return m_actionRunSpinner && !m_actionRunSpinner->isHidden();
    for (const ParallelRunColumn &column : m_parallelRunColumns) {
        if (column.runId == runId)
            return column.spinner && !column.spinner->isHidden();
    }
    return false;
}

bool MainWindow::testActionRunControlsUnderTitle(int runId) const
{
    const QWidget *title = nullptr;
    QList<const QWidget *> controls;
    if (runId == m_selectedRunId) {
        title = m_actionRunTitle;
        controls = {m_actionStopButton, m_actionCopyLogButton,
                    m_actionCopyLogPathButton, m_actionRerunButton};
    } else {
        for (const ParallelRunColumn &column : m_parallelRunColumns) {
            if (column.runId != runId)
                continue;
            title = column.title;
            controls = {column.stopButton, column.copyLogButton,
                        column.copyLogPathButton, column.rerunButton};
        }
    }
    if (!title)
        return false;
    for (const QWidget *control : controls) {
        if (!control || control->isHidden())
            return false;
        if (control->geometry().top() < title->geometry().bottom())
            return false;
    }
    return true;
}

bool MainWindow::testActionSendToAgentOffered() const
{
    return m_actionSendToAgentButton && !m_actionSendToAgentButton->isHidden();
}

QString MainWindow::testActionRunLogLocationText() const
{
    return m_actionRunLogPath && !m_actionRunLogPath->isHidden()
               ? m_actionRunLogPath->fullText()
               : QString();
}

QString MainWindow::testActionRunLogText(int runId) const
{
    if (runId == m_selectedRunId)
        return m_actionLog ? m_actionLog->toPlainText() : QString();
    const QPlainTextEdit *log = parallelRunLog(runId);
    return log ? log->toPlainText() : QString();
}

bool MainWindow::testCancelledActionRunCleanup(bool expectDeleted)
{
    if (!m_actionStore)
        return false;

    ActionRun run;
    run.owner = QStringLiteral("test-cleanup");
    run.name = QStringLiteral("cancelled-run");
    run.workflowPath = QStringLiteral(".forkmesh/cleanup.yml");
    run.workflowName = QStringLiteral("Cancelled cleanup");
    run.status = ActionStatus::Cancelled;
    const ActionRun created = m_actionStore->createRun(run);
    m_actionRuns = m_actionStore->loadAllRuns();

    const bool deleted = deleteCancelledRunsAutomatically({created});
    bool stillPresent = false;
    for (const ActionRun &stored : m_actionStore->loadAllRuns()) {
        if (stored.id == created.id) {
            stillPresent = true;
            break;
        }
    }
    if (stillPresent) {
        // The opt-out assertion leaves its fixture in storage deliberately;
        // clean it up synchronously so the rest of this long-lived window
        // fixture has the same history it started with.
        m_actionStore->deleteRun(created);
        m_actionRuns = m_actionStore->loadAllRuns();
    }
    return expectDeleted ? deleted && !stillPresent
                         : !deleted && stillPresent;
}

bool MainWindow::testSupersedeKeepsOtherPullsHeld()
{
    if (!m_actionStore)
        return false;

    auto held = [this](int pullNumber, const QString &commit) {
        ActionRun run;
        run.owner = QStringLiteral("test-supersede");
        run.name = QStringLiteral("held-pulls");
        run.workflowPath = QStringLiteral(".forkmesh/ci.yml");
        run.workflowName = QStringLiteral("CI tests");
        run.commit = commit;
        run.pullNumber = pullNumber;
        run.status = ActionStatus::AwaitingApproval;
        const ActionRun created = m_actionStore->createRun(run);
        m_actionRuns.prepend(created);
        cancelSupersededRuns(created);
        return created;
    };
    auto stillHeld = [this](const ActionRun &run) {
        for (const ActionRun &stored : m_actionStore->loadAllRuns())
            if (stored.id == run.id)
                return stored.status == ActionStatus::AwaitingApproval;
        return false;
    };

    const ActionRun first = held(85, QStringLiteral("aaaa"));
    const ActionRun other = held(90, QStringLiteral("bbbb"));
    const bool otherPullKept = stillHeld(first) && stillHeld(other);
    const ActionRun newer = held(85, QStringLiteral("cccc"));
    const bool samePullReplaced =
        !stillHeld(first) && stillHeld(other) && stillHeld(newer);

    for (const ActionRun &run : m_actionStore->loadAllRuns())
        if (run.owner == QLatin1String("test-supersede"))
            m_actionStore->deleteRun(run);
    m_actionRuns = m_actionStore->loadAllRuns();
    return otherPullKept && samePullReplaced;
}
#endif

namespace {

// How many open pull requests one pass resolves, and how many fresh heads it
// hands to the runner. A repository adopting `on: pull_request` for the first
// time has every open PR to catch up on; queueing them all at once would put
// dozens of checkouts on the node in one go, so later passes take the rest.
constexpr int kPullCheckScanLimit = 25;
constexpr int kPullCheckQueueLimit = 4;
// Replaying a signed patch onto the base costs a scratch worktree and a
// `git am`, so one pass rebuilds at most this many missing heads and later
// passes take the rest.
constexpr int kPullCheckMaterializeLimit = 2;

} // namespace

MainWindow::PullCheckCandidate
MainWindow::pullCheckCandidate(const PullRequest &pr,
                               const QList<PullRequest> &history,
                               const RepositoryRecord &repo) const
{
    PullCheckCandidate candidate;
    candidate.number = pr.number;
    candidate.pull = pr;
    QString head = pr.head.trimmed();
    const int separator = head.indexOf(QLatin1Char(':'));
    if (separator > 0)
        head = head.mid(separator + 1).trimmed(); // "fork:branch"
    candidate.head = head;
    candidate.base = pr.base.trimmed();
    candidate.branchBacked = pr.branchBacked;
    candidate.author = pr.authorName.trimmed().isEmpty()
                           ? pr.author.left(10)
                           : pr.authorName.trimmed();
    // Trust is decided here, from the pull list already in memory: this node's
    // own signing key and the repository's own name are established by
    // definition, everyone else has to have landed a merge here.
    candidate.firstTimeAuthor = !PullAuthorTrust::isEstablished(
        pr, history,
        {m_profileIdentity.publicKey(), accountOwner(), repo.owner, m_userName});
    candidate.authorPullOrdinal = PullAuthorTrust::pullOrdinal(pr, history);
    return candidate;
}

// Automatic pull-request checks. A pull request is not a push: its head lives
// on a branch nobody pushed into the served mirror (and, for a submission that
// arrived through the relay, on no branch at all until PullStore materializes
// one), so the push pipeline never saw it and the Checks tab stayed empty
// unless somebody pressed "Run checks against this PR" by hand.
void MainWindow::queueChecksForOpenPulls(RepositoryRecord repo,
                                         QList<PullRequest> pulls)
{
    if (m_pullChecksScanInFlight)
        return; // the next pull reload picks up whatever this pass misses

    QList<PullCheckCandidate> candidates;
    QString fingerprint;
    for (const PullRequest &pr : pulls) {
        if (pr.number <= 0 || pr.status != QLatin1String("open"))
            continue;
        candidates.append(pullCheckCandidate(pr, pulls, repo));
        fingerprint += QString::number(pr.number) + QLatin1Char(':') + pr.head +
                       QLatin1Char('\x1f');
    }
    if (candidates.isEmpty())
        return;
    // A pull reload happens on every push, review and agent event; the heads
    // rarely moved. Skip the git work unless the open pulls changed or the
    // floor has passed — a branch-backed head can advance with no metadata
    // change at all, so this cannot be fingerprint-only.
    const QString scanKey = repo.owner.trimmed().toLower() + QLatin1Char('/') +
                            repo.name.trimmed().toLower();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kRescanFloorMs = 60 * 1000;
    if (m_pullChecksScanFingerprint.value(scanKey) == fingerprint &&
        nowMs - m_pullChecksScanAtMs.value(scanKey, 0) < kRescanFloorMs)
        return;
    if (m_pullChecksScanFingerprint.value(scanKey) != fingerprint) {
        // The open pulls changed: give the ones that did not replay before
        // another chance, since one of them is probably why.
        const QString prefix =
            repo.owner + QLatin1Char('\x1f') + repo.name + QLatin1Char('\x1f');
        for (auto it = m_pullChecksNoHead.begin();
             it != m_pullChecksNoHead.end();) {
            it = it->startsWith(prefix) ? m_pullChecksNoHead.erase(it)
                                        : std::next(it);
        }
    }
    m_pullChecksScanFingerprint.insert(scanKey, fingerprint);
    m_pullChecksScanAtMs.insert(scanKey, nowMs);
    for (PullCheckCandidate &candidate : candidates) {
        candidate.mayMaterialize = !m_pullChecksNoHead.contains(
            repo.owner + QLatin1Char('\x1f') + repo.name + QLatin1Char('\x1f') +
            QString::number(candidate.number));
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const PullCheckCandidate &a, const PullCheckCandidate &b) {
                  return a.number > b.number; // newest pull requests first
              });
    if (candidates.size() > kPullCheckScanLimit)
        candidates.erase(candidates.begin() + kPullCheckScanLimit,
                         candidates.end());
    resolvePullCheckHeads(repo, candidates, WorkflowTrigger::PullRequest,
                          /*force=*/false);
}

// Publish each candidate's head into the served mirror — the runner checks out
// of the mirror, and a pull request's head is exactly what never got there —
// then queue its workflows. The git work runs on a worker: this is called from
// every pull reload, and the GUI thread's git helpers pump the event loop.
// code-health: allow(blocking-wait) every git wait is inside the runOffThread worker that resolves the heads
void MainWindow::resolvePullCheckHeads(RepositoryRecord repo,
                                       QList<PullCheckCandidate> candidates,
                                       WorkflowTrigger trigger, bool force)
{
    if (!m_actionStore || repo.previewOnly || !repo.actionsEnabled ||
        repo.externallyManagedActions || candidates.isEmpty())
        return;
    const QString workTree = repo.localPath.trimmed();
    const QString mirror = repo.mirrorPath.trimmed();
    if (workTree.isEmpty() || mirror.isEmpty() || !QDir(mirror).exists())
        return;

    // The flag tracks the automatic pass only: a maintainer's click must not be
    // dropped because a background scan happens to be running.
    if (!force)
        m_pullChecksScanInFlight = true;
    const QString owner = repo.owner;
    const QString name = repo.name;
    const PullStore store(workTree, mirror, &m_profileIdentity, m_userName);
    runOffThread<QList<PullCheckHead>>(
        [workTree, mirror, candidates, store]() -> QList<PullCheckHead> {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("pulls"),
                QStringLiteral("publish pull request check heads"),
                forkmesh::ActionTelemetry::Execution::Worker);
            auto git = [](const QString &dir, const QStringList &args,
                          QString *error = nullptr,
                          QString *fullError = nullptr) -> QString {
                QProcess process;
                process.setProcessChannelMode(QProcess::SeparateChannels);
                process.start(QStringLiteral("git"),
                              QStringList{QStringLiteral("-C"), dir} + args);
                if (!process.waitForStarted(3000) ||
                    !process.waitForFinished(20000)) {
                    process.kill();
                    process.waitForFinished(1000);
                    if (error)
                        *error = QStringLiteral("git did not finish");
                    return {};
                }
                if (process.exitCode() != 0) {
                    const QString stderrText =
                        QString::fromUtf8(process.readAllStandardError())
                            .trimmed();
                    if (fullError)
                        *fullError = stderrText;
                    if (error)
                        *error = stderrText.right(200);
                    return {};
                }
                return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
            };
            auto resolve = [&git](const QString &dir, const QString &ref) {
                return git(dir, {QStringLiteral("rev-parse"),
                                 QStringLiteral("--verify"),
                                 QStringLiteral("--quiet"),
                                 QStringLiteral("--end-of-options"),
                                 ref + QStringLiteral("^{commit}")});
            };
            auto tree = [&git](const QString &dir, const QString &commit) {
                return git(dir, {QStringLiteral("rev-parse"),
                                 QStringLiteral("--verify"),
                                 QStringLiteral("--quiet"),
                                 QStringLiteral("--end-of-options"),
                                 commit + QStringLiteral("^{tree}")});
            };
            auto mergeBase = [&git](const QString &dir, const QString &left,
                                    const QString &right) {
                return git(dir, {QStringLiteral("merge-base"), left, right});
            };

            QList<PullCheckHead> heads;
            int materialized = 0;
            for (const PullCheckCandidate &candidate : candidates) {
                PullCheckHead head;
                head.number = candidate.number;
                head.author = candidate.author;
                head.base = candidate.base;
                head.firstTimeAuthor = candidate.firstTimeAuthor;
                head.authorPullOrdinal = candidate.authorPullOrdinal;
                head.ref = QStringLiteral("refs/heads/pr/%1").arg(candidate.number);
                QStringList refs{
                    QStringLiteral("refs/pr/%1/head").arg(candidate.number),
                    head.ref};
                // Reading a stored-patch pull's named head branch would check
                // out a tree the pull no longer describes, so only a
                // branch-backed pull is resolved through its branch — and there
                // it comes first, because that branch *is* the change.
                if (candidate.branchBacked && !candidate.head.isEmpty())
                    refs.prepend(QStringLiteral("refs/heads/") + candidate.head);
                const auto firstResolved = [&]() {
                    for (const QString &ref : std::as_const(refs)) {
                        const QString commit = resolve(workTree, ref);
                        if (!commit.isEmpty())
                            return commit;
                    }
                    return QString();
                };
                head.commit = firstResolved();
                if (head.commit.isEmpty() && candidate.mayMaterialize &&
                    materialized < kPullCheckMaterializeLimit) {
                    // A submission that arrived through the relay is a signed
                    // patch, not a branch: nothing to check out until its
                    // commits are replayed onto the base. That is what left
                    // cross-node pull requests with no checks at all.
                    ++materialized;
                    store.ensurePullHeadRef(candidate.pull, &head.error);
                    head.commit = firstResolved();
                    if (!head.commit.isEmpty())
                        head.error.clear();
                }
                if (head.commit.isEmpty()) {
                    if (head.error.isEmpty())
                        head.error = QStringLiteral(
                            "its signed commits do not replay onto the base");
                    heads.append(head);
                    continue;
                }
                // Reverse convergence may have bridged two equivalent pull
                // histories with a deterministic merge commit. Its tree is
                // still exactly the proposed tree, and it makes both lineages
                // fast-forwardable. Keep that bridge as the check head instead
                // of force-rewinding this compatibility ref on every startup;
                // the rewind was the recurring +commits/+branches mirror drift.
                const QString publishedHead = resolve(mirror, head.ref);
                const QString requestedTree = tree(workTree, head.commit);
                const QString publishedTree = tree(mirror, publishedHead);
                if (!publishedHead.isEmpty() &&
                    publishedHead != head.commit &&
                    ((!requestedTree.isEmpty() &&
                      publishedTree == requestedTree) ||
                     mergeBase(mirror, head.commit, publishedHead) ==
                         head.commit)) {
                    head.commit = publishedHead;
                }
                // An object with nothing pointing at it is one garbage
                // collection away from vanishing mid-run, so the mirror gets the
                // head under its own ref rather than just the objects.
                if (resolve(mirror, head.ref) != head.commit) {
                    head.published = true;
                    // Compare-and-swap the derived ref. Concurrent startup and
                    // pull refresh scans may both reach this point; a scan that
                    // observed no ref must never overwrite one another scan
                    // just published, and a stale observation must never win.
                    const QString lease =
                        QStringLiteral("--force-with-lease=%1:%2")
                            .arg(head.ref, publishedHead);
                    const QStringList pushArgs{
                        QStringLiteral("push"), QStringLiteral("--quiet"),
                        lease, mirror,
                        QStringLiteral("+%1:%2").arg(head.commit, head.ref)};
                    QString pushErrors;
                    git(workTree, pushArgs, &head.error, &pushErrors);
                    if (resolve(mirror, head.ref) != head.commit &&
                        !forkmesh::upstream::dropUnreadableRefs(mirror,
                                                                pushErrors)
                             .isEmpty()) {
                        head.error.clear();
                        git(workTree, pushArgs, &head.error);
                    }
                    if (resolve(mirror, head.ref) != head.commit) {
                        if (head.error.isEmpty())
                            head.error = QStringLiteral(
                                "the served mirror did not accept the head");
                        head.commit.clear();
                    } else {
                        head.error.clear();
                    }
                }
                heads.append(head);
            }
            return heads;
        },
        [this, owner, name, mirror, trigger, force](QList<PullCheckHead> heads) {
            if (!force)
                m_pullChecksScanInFlight = false;
            applyPullCheckHeads(owner, name, mirror, trigger, force, heads);
        });
}

// GUI-thread half of resolvePullCheckHeads: the repository list may have been
// rebuilt while the heads were resolving, so everything is re-looked-up here.
void MainWindow::applyPullCheckHeads(const QString &owner, const QString &name,
                                     const QString &mirror,
                                     WorkflowTrigger trigger, bool force,
                                     const QList<PullCheckHead> &heads)
{
    const int repoIndex = repoIndexFor(owner, name);
    if (repoIndex < 0 || !m_actionStore)
        return;
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    if (repo.previewOnly || !repo.actionsEnabled || repo.externallyManagedActions)
        return;
    if (repo.mirrorPath.trimmed() != mirror)
        return; // the heads went to a mirror this record no longer serves from

    int queued = 0;
    int deferred = 0;
    for (const PullCheckHead &head : heads) {
        if (head.commit.isEmpty()) {
            if (!head.published)
                m_pullChecksNoHead.insert(owner + QLatin1Char('\x1f') + name +
                                          QLatin1Char('\x1f') +
                                          QString::number(head.number));
            if (force || head.published)
                logSystem(QString::fromUtf8(
                              "Actions: could not publish pull request #%1's "
                              "head for %2/%3 \xE2\x80\x94 %4")
                              .arg(head.number)
                              .arg(owner, name, head.error));
            continue;
        }
        const QString key = owner + QLatin1Char('\x1f') + name +
                            QLatin1Char('\x1f') + QString::number(head.number);
        if (!force) {
            if (m_pullChecksQueued.value(key) == head.commit)
                continue;
            bool alreadyRan = false;
            for (const ActionRun &run : std::as_const(m_actionRuns)) {
                if (run.owner == owner && run.name == name &&
                    run.commit == head.commit) {
                    alreadyRan = true; // this head was checked before, or by hand
                    break;
                }
            }
            if (alreadyRan) {
                m_pullChecksQueued.insert(key, head.commit);
                continue;
            }
            if (queued >= kPullCheckQueueLimit) {
                ++deferred;
                continue;
            }
        }

        m_pullChecksQueued.insert(key, head.commit);
        if (head.published) {
            // Publishing the head fires the mirror's post-receive hook. Claim
            // the commit so the push pipeline does not also queue this
            // repository's `on: push` workflows — a deploy must not run a
            // proposed head just because somebody opened a pull request.
            const QString explicitKey = owner + QLatin1Char('\x1f') + name +
                                        QLatin1Char('\x1f') +
                                        head.commit.toLower();
            m_explicitActionPushes.insert(explicitKey);
            QTimer::singleShot(60000, this, [this, explicitKey] {
                m_explicitActionPushes.remove(explicitKey);
            });
        }
        ActionPullContext context;
        context.number = head.number;
        context.author = head.author;
        context.base = head.base;
        context.firstTimeAuthor = head.firstTimeAuthor;
        context.authorPullOrdinal = head.authorPullOrdinal;
        queueWorkflowsForCommit(repoIndex, owner, name, head.commit, head.ref,
                                trigger, context);
        ++queued;
    }
    if (deferred > 0)
        logSystem(QStringLiteral(
                      "Actions: %1 more open pull request(s) on %2/%3 still need "
                      "checks; the next pull refresh queues them.")
                      .arg(deferred)
                      .arg(owner, name));
}

void MainWindow::scheduleOpenRepoDetailRefresh()
{
    if (!m_openRepoRefreshTimer) {
        m_openRepoRefreshTimer = new QTimer(this);
        m_openRepoRefreshTimer->setSingleShot(true);
        m_openRepoRefreshTimer->setInterval(300);
        connect(m_openRepoRefreshTimer, &QTimer::timeout, this,
                &MainWindow::refreshOpenRepoDetail);
    }
    m_openRepoRefreshTimer->start(); // restart: collapses a burst into one refresh
}

void MainWindow::refreshOpenRepoDetail()
{
    if (m_openRepoRefreshTimer)
        m_openRepoRefreshTimer->stop(); // a direct refresh subsumes any pending one
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    // Re-entrancy guard: this fires on a debounce timer and from many
    // push/sync paths, each running synchronous git reads under a GitKeepAlive that
    // pumps the event loop. A second heavy refresh firing *during* that pump (the
    // periodic refreshRepositoryList, or this timer again) would nest its git work
    // inside the first one's pump and compound into a multi-second stall. Re-arm the
    // debounce so it runs on a fresh event-loop turn once the in-flight one unwinds.
    if (m_heavyRefreshInFlight) {
        scheduleOpenRepoDetailRefresh();
        return;
    }
    // A push or sync path that already named its routine keeps the more
    // specific name; the debounce timer's own firings get this one.
    const forkmesh::BackgroundRoutine routine(
        forkmesh::BackgroundRoutine::unlessNamed(
            QStringLiteral("open repo detail refresh")));
    const ScopedFlag refreshGuard(m_heavyRefreshInFlight);
    QPointer<QWidget> typingFocus;
    if (QWidget *focused = QApplication::focusWidget()) {
        if ((qobject_cast<QLineEdit *>(focused) ||
             qobject_cast<QPlainTextEdit *>(focused) ||
             qobject_cast<QTextEdit *>(focused)) &&
            focused->window() == this)
            typingFocus = focused;
    }
    const int visibleTab =
        m_repoDetailStack ? m_repoDetailStack->currentIndex() : -1;
    const bool refsJustArrived = m_repoBranch.isEmpty();
    if (refsJustArrived || visibleTab == 0 ||
        (m_branchesTabIndex >= 0 && visibleTab == m_branchesTabIndex))
        loadBranchesAndTags();
    if (m_commitsTable && m_commitsTable->isVisibleTo(this))
        loadCommits();
    if (visibleTab == 3)
        reloadAgents();
    if (refsJustArrived)
        refreshRepoTabCounts(); // the repo only just materialized: every badge is empty
    else if (visibleTab == 2)
        reloadIssuesInBackground();
    reloadPullsInBackground();
    if (visibleTab == 6)
        refreshRepoActions();
    updateRepoDetailStatus();
    updateRepoActionMenus();
    updateRepoCodeSize();
    refreshRepoSyncIndicators();
    refreshRepoChangeBadge(); // a sync/merge/commit moves the working tree too
    refreshRepoPinBanner(); // a sync may have advanced refs past the pinned hash
    m_treeLoadedForIndex = -1; // force the explorer tree to rebuild on next use
    if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 2)
        loadCoveExplorer();
    if (visibleTab == 0)
        loadRepoOverview(m_overviewPath);
    if (mirrorNodesOnScreen())
        loadMirrorNodesPanel();
    if (typingFocus && typingFocus->isVisibleTo(this) &&
        typingFocus->isEnabled() && QApplication::focusWidget() != typingFocus)
        typingFocus->setFocus(Qt::OtherFocusReason);
}

QStringList MainWindow::actionNodeLabels() const
{
    QSettings settings;
    return ActionFile::nodeLabels(
        machineNodeName(),
        settings
            .value(QString::fromLatin1(
                forkmesh::mirror_actions::kNodeSetting))
            .toString(),
        settings.value(QString::fromLatin1(kActionNodeLabelsSetting))
            .toString());
}

QString MainWindow::workflowNodePin(const RepositoryRecord &repo,
                                    const QString &path) const
{
    return ActionFile::pinnedNode(repo.workflowNodes, path);
}

QStringList
MainWindow::workflowRunsOnLabels(const RepositoryRecord &repo,
                                 const ActionWorkflow &workflow) const
{
    const QString pin = workflowNodePin(repo, workflow.path);
    if (!pin.isEmpty())
        return QStringList{pin};
    return workflow.runsOn;
}

bool MainWindow::workflowRunsOnThisNode(const RepositoryRecord &repo,
                                        const ActionWorkflow &workflow) const
{
    return workflowRunsOnThisNode(repo, workflow, actionNodeLabels());
}

bool MainWindow::workflowRunsOnThisNode(const RepositoryRecord &repo,
                                        const ActionWorkflow &workflow,
                                        const QStringList &nodeLabels) const
{
    ActionWorkflow probe;
    probe.runsOn = workflowRunsOnLabels(repo, workflow);
    return probe.runsOnNode(nodeLabels);
}

QString MainWindow::workflowDedicationLabel(const RepositoryRecord &repo,
                                            const ActionWorkflow &workflow) const
{
    return workflowRunsOnLabels(repo, workflow).join(QStringLiteral(", "));
}

QStringList MainWindow::actionNodeCandidates() const
{
    QStringList out;
    const auto add = [&out](const QString &raw) {
        const QString node = raw.trimmed();
        if (node.isEmpty() || out.contains(node, Qt::CaseInsensitive))
            return;
        out.append(node);
    };
    add(machineNodeName()); // this machine first: the common answer
    for (const MemberInfo &node : m_homeRoster)
        add(node.nodeName);
    for (const ActionWorkflow &wf : m_repoWorkflows) {
        for (const QString &label : wf.runsOn) {
            if (label.compare(QLatin1String("any"), Qt::CaseInsensitive) != 0)
                add(label);
        }
    }
    return out;
}

void MainWindow::saveActionNodeLabels(const QString &labels)
{
    const QStringList clean = ActionFile::parseLabelList(labels);
    QSettings settings;
    if (clean.isEmpty())
        settings.remove(QString::fromLatin1(kActionNodeLabelsSetting));
    else
        settings.setValue(QString::fromLatin1(kActionNodeLabelsSetting),
                          clean.join(QStringLiteral(", ")));
    logSystem(QStringLiteral("Actions: this node answers to %1.")
                  .arg(actionNodeLabels().join(QStringLiteral(", "))));
    if (m_actionWorkflowList)
        refreshRepoActions();
}

std::shared_ptr<void> MainWindow::pinActionMirror(const RepositoryRecord &repo,
                                                  QString *mirrorPath) const
{
    const auto publicPin =
        m_publicMirrorMaterializations.value(repo.publicArchiveId);
    if (publicPin && publicPin->isValid()) {
        if (mirrorPath)
            *mirrorPath = publicPin->repositoryPath();
        return publicPin;
    }
    const auto privatePin =
        m_privateMirrorMaterializations.value(repo.privateReplicaId);
    if (privatePin && privatePin->isValid()) {
        if (mirrorPath)
            *mirrorPath = privatePin->repositoryPath();
        return privatePin;
    }
    return {};
}

void MainWindow::releaseActionMirrorPin(int runId)
{
    const ActionMirrorPin pin = m_actionMirrorPins.take(runId);
    if (!pin.materialization)
        return;
    const int index = repoIndexFor(pin.owner, pin.name);
    if (index < 0)
        return;
    const int carried =
        carryMirrorReleaseCas(pin.path, m_repositories.at(index).mirrorPath);
    if (carried > 0)
        logSystem(QStringLiteral(
                      "Actions: moved %1 release artifact blob(s) from run #%2 "
                      "into the mirror now serving %3/%4.")
                      .arg(carried)
                      .arg(runId)
                      .arg(pin.owner, pin.name));
}

ActionNeeds::State MainWindow::actionRunNeedsState(int runId, QString *detail)
{
    const ActionRun *run = findRun(runId);
    if (!run)
        return ActionNeeds::State::Ready; // vanished; the queue drops it
    const ActionWorkflow wf =
        ActionFile::parse(run->workflowPath, run->workflowContent);
    if (wf.needs.isEmpty())
        return ActionNeeds::State::Ready;
    return ActionNeeds::resolve(*run, wf.needs, m_actionRuns, detail);
}

// Why a queued run has not started yet. "Queued" beside an empty log pane
// reads like a stall, and the two things that actually hold a run back — a
// `needs:` gate on another workflow, and a runner pool with nothing free —
// are otherwise only visible in the system log.
QString MainWindow::actionRunQueueReason(int runId)
{
    const ActionRun *run = findRun(runId);
    if (!run || run->status != ActionStatus::Queued)
        return QString();
    QString detail;
    switch (actionRunNeedsState(runId, &detail)) {
    case ActionNeeds::State::Waiting:
        return detail.isEmpty()
                   ? QStringLiteral("needs another workflow to pass first")
                   : QStringLiteral("needs another workflow to pass first: "
                                    "%1")
                         .arg(detail);
    case ActionNeeds::State::Blocked:
        return detail.isEmpty()
                   ? QStringLiteral("will not start: a workflow it needs "
                                    "did not pass")
                   : QStringLiteral("will not start: a workflow it needs "
                                    "did not pass (%1)")
                         .arg(detail);
    case ActionNeeds::State::Ready:
        break;
    }
    const auto verifying = m_actionVerifyingRuns.constFind(runId);
    if (verifying != m_actionVerifyingRuns.cend()) {
        const qint64 seconds =
            qMax<qint64>(0, (QDateTime::currentMSecsSinceEpoch() -
                             verifying.value()) / 1000);
        return seconds < 5
                   ? QStringLiteral("starting: checking its approved "
                                    "repository snapshot")
                   : QStringLiteral("checking its approved repository "
                                    "snapshot (%1s)")
                         .arg(seconds);
    }
    int idle = 0;
    QStringList busyWith;
    for (const ActionRunner *runner : std::as_const(m_actionRunners)) {
        if (!runner->busy()) {
            ++idle;
            continue;
        }
        if (const ActionRun *active = findRun(runner->currentRunId())) {
            const QString label =
                QStringLiteral("\"%1\"").arg(active->workflowName);
            if (!busyWith.contains(label))
                busyWith.append(label);
        }
    }
    const int reserved = reservedActionRunnerCount();
    if (idle > reserved)
        return QStringLiteral("starting");
    if (idle > 0)
        return QStringLiteral("waiting for a runner: %1 of %2 set aside for "
                              "runs checking their snapshot")
            .arg(reserved)
            .arg(m_actionRunners.size());
    return busyWith.isEmpty()
               ? QStringLiteral("waiting for a runner: all %1 are busy")
                     .arg(m_actionRunners.size())
               : QStringLiteral("waiting for a runner: all %1 are busy "
                                "with %2")
                     .arg(m_actionRunners.size())
                     .arg(busyWith.join(QStringLiteral(", ")));
}

// The line under a run's title, shared by the primary pane and the parallel
// columns so a queued run explains itself wherever it is watched.
QString MainWindow::actionRunMetaText(const ActionRun &run,
                                      const QString &statusOverride)
{
    const QString status =
        statusOverride.isEmpty() ? run.status : statusOverride;
    QString meta = QString::fromUtf8("%1/%2 \xC2\xB7 %3 \xC2\xB7 %4")
                       .arg(run.owner, run.name, run.commit.left(8),
                            actionStatusText(status));
    if (run.startedAtMs > 0 && run.finishedAtMs > run.startedAtMs)
        meta += QString::fromUtf8(" \xC2\xB7 %1s")
                    .arg((run.finishedAtMs - run.startedAtMs) / 1000);
    if (status == ActionStatus::Queued) {
        const QString reason = actionRunQueueReason(run.id);
        if (!reason.isEmpty())
            meta += QString::fromUtf8(" \xC2\xB7 %1").arg(reason);
    }
    return meta;
}

// Shown in a queued run's still-empty log pane, which otherwise looks stalled.
// The placeholder disappears by itself once the run writes its first line.
QString MainWindow::actionRunLogPlaceholder(const ActionRun &run)
{
    if (run.status != ActionStatus::Queued)
        return QString();
    const QString reason = actionRunQueueReason(run.id);
    return reason.isEmpty()
               ? QStringLiteral("Queued: this run has not started yet.")
               : QString::fromUtf8("Queued \xE2\x80\x94 %1.").arg(reason);
}

void MainWindow::noteActionRunWaiting(int runId, const QString &detail)
{
    if (m_actionWaitingRuns.contains(runId))
        return; // already announced; the queue is swept every few seconds
    m_actionWaitingRuns.insert(runId);
    const ActionRun *run = findRun(runId);
    if (!run)
        return;
    logSystem(QString::fromUtf8(
                  "Actions: \xE2\x80\x9C%1\xE2\x80\x9D for %2/%3 @ %4 is "
                  "waiting \xE2\x80\x94 %5.")
                  .arg(run->workflowName, run->owner, run->name,
                       run->commit.left(8), detail));
}

// A snapshot check takes seconds. One that has not reported back in this long
// is treated as lost: the run is checked again, and whichever result lands
// first starts it (ActionRunner::start marks it Running synchronously, so the
// other finds nothing left to do).
static constexpr qint64 kActionVerificationTimeoutMs = 2 * 60 * 1000;

int MainWindow::reservedActionRunnerCount() const
{
    return m_actionVerifyingRuns.size();
}

// A Queued run is only ever started from m_actionQueue. Any exit that takes an
// id out without starting, skipping or failing the run strands it: the table
// keeps saying "Queued" and nothing will pick it up again. Put such runs back
// on every sweep instead of trusting each exit to remember.
void MainWindow::requeueOrphanedActionRuns()
{
    if (!m_actionRunsRestored)
        return; // the restore pass queues everything it loads
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_actionVerifyingRuns.begin();
         it != m_actionVerifyingRuns.end();) {
        if (now - it.value() < kActionVerificationTimeoutMs) {
            ++it;
            continue;
        }
        if (const ActionRun *run = findRun(it.key())) {
            logSystem(QString::fromUtf8(
                          "Actions: the snapshot check for \xE2\x80\x9C%1"
                          "\xE2\x80\x9D for %2/%3 @ %4 has not reported back "
                          "after %5s \xE2\x80\x94 checking again.")
                          .arg(run->workflowName, run->owner, run->name,
                               run->commit.left(8))
                          .arg((now - it.value()) / 1000));
        }
        it = m_actionVerifyingRuns.erase(it);
    }
    const QSet<int> queued(m_actionQueue.cbegin(), m_actionQueue.cend());
    for (const ActionRun &run : std::as_const(m_actionRuns)) {
        if (run.status != ActionStatus::Queued || queued.contains(run.id) ||
            m_actionVerifyingRuns.contains(run.id) || runnerForRun(run.id))
            continue;
        m_actionQueue.append(run.id);
        logSystem(QString::fromUtf8(
                      "Actions: re-queued \xE2\x80\x9C%1\xE2\x80\x9D for "
                      "%2/%3 @ %4 \xE2\x80\x94 it was Queued but had dropped "
                      "out of the run queue.")
                      .arg(run.workflowName, run.owner, run.name,
                           run.commit.left(8)));
    }
}

// The meta line of a queued run says what it is waiting for, and that changes
// without the run's own status changing: a `needs:` gate passes, a runner
// frees up, a snapshot check starts. Refresh it on every queue pass.
void MainWindow::refreshQueuedRunReasons()
{
    if (m_actionRunMeta) {
        const ActionRun *run = findRun(m_selectedRunId);
        if (run && run->status == ActionStatus::Queued) {
            const QString meta = actionRunMetaText(*run);
            if (m_actionRunMeta->text() != meta)
                m_actionRunMeta->setText(meta);
            const QString placeholder = actionRunLogPlaceholder(*run);
            if (m_actionLog && m_actionLog->placeholderText() != placeholder)
                m_actionLog->setPlaceholderText(placeholder);
        }
    }
    for (const ParallelRunColumn &column : std::as_const(m_parallelRunColumns)) {
        const ActionRun *run = findRun(column.runId);
        if (!run || run->status != ActionStatus::Queued || !column.meta)
            continue;
        const QString meta = actionRunMetaText(*run);
        if (column.meta->fullText() != meta)
            column.meta->setFullText(meta);
        const QString placeholder = actionRunLogPlaceholder(*run);
        if (column.log && column.log->placeholderText() != placeholder)
            column.log->setPlaceholderText(placeholder);
    }
}

void MainWindow::processActionQueue()
{
    if (m_actionRunners.isEmpty())
        return;
    requeueOrphanedActionRuns();
    if (!m_actionWaitingRuns.isEmpty()) {
        const QSet<int> queued(m_actionQueue.cbegin(), m_actionQueue.cend());
        m_actionWaitingRuns.intersect(queued);
    }
    while (!m_actionQueue.isEmpty()) {
        int idleRunners = 0;
        for (const ActionRunner *runner : std::as_const(m_actionRunners)) {
            if (!runner->busy())
                ++idleRunners;
        }
        // Each snapshot check in flight has a runner set aside. Beyond those,
        // an idle runner takes the next ready run straight away, so a deploy
        // whose gate has passed starts beside a long desktop build instead of
        // waiting for it.
        if (idleRunners <= reservedActionRunnerCount())
            break; // a finishing run or snapshot check resumes the queue
        int queueIndex = -1;
        ActionNeeds::State needsState = ActionNeeds::State::Ready;
        QString needsDetail;
        for (int i = 0; i < m_actionQueue.size(); ++i) {
            QString detail;
            const ActionNeeds::State state =
                actionRunNeedsState(m_actionQueue.at(i), &detail);
            if (state == ActionNeeds::State::Waiting) {
                noteActionRunWaiting(m_actionQueue.at(i), detail);
                continue;
            }
            queueIndex = i;
            needsState = state;
            needsDetail = detail;
            break;
        }
        if (queueIndex < 0)
            break; // everything queued is waiting on another workflow
        const int runId = m_actionQueue.takeAt(queueIndex);
        m_actionWaitingRuns.remove(runId);
        ActionRun *run = findRun(runId);
        if (!run || run->status != ActionStatus::Queued ||
            m_actionVerifyingRuns.contains(runId))
            continue;
        if (needsState == ActionNeeds::State::Blocked) {
            run->status = ActionStatus::Skipped;
            run->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
            m_actionStore->saveRun(*run);
            scheduleMirrorActionsSummary(0);
            logSystem(QString::fromUtf8(
                          "Actions: skipped \xE2\x80\x9C%1\xE2\x80\x9D for "
                          "%2/%3 @ %4 \xE2\x80\x94 it needs %5.")
                          .arg(run->workflowName, run->owner, run->name,
                               run->commit.left(8), needsDetail));
            continue;
        }
        const int repoIndex = repoIndexFor(run->owner, run->name);
        if (repoIndex < 0) {
            run->status = ActionStatus::Failed;
            m_actionStore->saveRun(*run);
            scheduleMirrorActionsSummary(0);
            continue;
        }
        QString mirror = m_repositories.at(repoIndex).mirrorPath;
        const QString workTree = m_repositories.at(repoIndex).localPath;
        // Encrypted mirrors live in a temporary materialization that every
        // sealing pass replaces and deletes. Resolve the one that is live right
        // now and hold it open for the whole run, so a re-seal (publishing a
        // release kicks one off) cannot pull the checkout source out from under
        // an in-flight build.
        const std::shared_ptr<void> mirrorPin =
            pinActionMirror(m_repositories.at(repoIndex), &mirror);
        const ActionRun snapshot = *run;
        m_actionVerifyingRuns.insert(runId, QDateTime::currentMSecsSinceEpoch());
        runOffThread<ActionQueueVerification>(
            [runId, repoIndex, snapshot, mirror, workTree, mirrorPin] {
                const forkmesh::BackgroundScope activity(
                    QStringLiteral("actions"),
                    QStringLiteral("verify queued workflow snapshot"),
                    forkmesh::ActionTelemetry::Execution::Worker);
                ActionQueueVerification result;
                result.runId = runId;
                result.repoIndex = repoIndex;
                result.verified = snapshot;
                result.mirror = mirror;
                result.workTree = workTree;
                result.mirrorPin = mirrorPin;
                result.repositoryWorkflow =
                    workflowContentAt(mirror, result.verified.commit,
                                      result.verified.workflowPath);
                bindActionRepositoryState(&result.verified, mirror,
                                          &result.snapshotError);
                return result;
            },
            [this](ActionQueueVerification result) {
                finishActionQueueVerification(std::move(result));
            });
    }
    refreshQueuedRunReasons();
}

void MainWindow::finishActionQueueVerification(ActionQueueVerification result)
{
    m_actionVerifyingRuns.remove(result.runId);
    ActionRun *run = findRun(result.runId);
    if (!run || run->status != ActionStatus::Queued) {
        processActionQueue();
        return;
    }
    if (result.repoIndex < 0 || result.repoIndex >= m_repositories.size()) {
        // The repository list shrank while the worker ran. Dropping the id
        // here used to leave the run Queued with nothing left to start it;
        // resolve it again (a repository that is really gone fails the run).
        m_actionQueue.prepend(result.runId);
        processActionQueue();
        return;
    }
    const RepositoryRecord &repository = m_repositories.at(result.repoIndex);
    const bool stale = repository.owner != result.verified.owner ||
                       repository.name != result.verified.name ||
                       repository.mirrorPath != result.mirror ||
                       run->commit != result.verified.commit ||
                       run->workflowPath != result.verified.workflowPath ||
                       run->workflowContent != result.verified.workflowContent;
    if (stale) {
        // Repository lists and queued records can refresh while the worker is
        // hashing. Re-resolve and verify the current record instead of starting
        // a run against a snapshot whose index or identity has gone stale.
        m_actionQueue.prepend(result.runId);
        processActionQueue();
        return;
    }

    const bool approved =
        !result.repositoryWorkflow.isNull() &&
        result.repositoryWorkflow == result.verified.workflowContent &&
        result.snapshotError.isEmpty() &&
        !result.verified.repositoryTree.isEmpty() &&
        !result.verified.executionDigest.isEmpty() &&
        result.verified.repositoryTree == run->repositoryTree &&
        result.verified.executionDigest == run->executionDigest &&
        ActionStore::isApproved(
            result.verified.repoKey(), result.verified.workflowPath,
            result.verified.workflowContent, result.verified.repositoryTree,
            result.verified.executionDigest);
    if (!approved) {
        run->status = ActionStatus::AwaitingApproval;
        m_actionStore->saveRun(*run);
        scheduleMirrorActionsSummary(0);
        logSystem(QStringLiteral(
                      "Actions: blocked \"%1\" for %2/%3 @ %4 because its "
                      "approved repository state could not be reverified%5.")
                      .arg(run->workflowName, run->owner, run->name,
                           run->commit.left(8),
                           result.snapshotError.isEmpty()
                               ? QString()
                               : QStringLiteral(": ") + result.snapshotError));
        processActionQueue();
        return;
    }

    const ActionWorkflow workflow =
        ActionFile::parse(run->workflowPath, run->workflowContent);
    if (!workflow.valid) {
        run->status = ActionStatus::Failed;
        m_actionStore->saveRun(*run);
        scheduleMirrorActionsSummary(0);
        processActionQueue();
        return;
    }
    if (!workflowRunsOnThisNode(repository, workflow)) {
        run->status = ActionStatus::Skipped;
        m_actionStore->saveRun(*run);
        scheduleMirrorActionsSummary(0);
        logSystem(QString::fromUtf8(
                      "Actions: skipped \xE2\x80\x9C%1\xE2\x80\x9D for "
                      "%2/%3 \xE2\x80\x94 it is dedicated to %4.")
                      .arg(run->workflowName, run->owner, run->name,
                           workflowDedicationLabel(
                               repository, workflow)));
        processActionQueue();
        return;
    }

    ActionRunner *idle = nullptr;
    for (ActionRunner *runner : std::as_const(m_actionRunners)) {
        if (!runner->busy()) {
            idle = runner;
            break;
        }
    }
    if (!idle) {
        // A runner changed state while the verification worker was active.
        // Put the still-queued run back without holding its temporary mirror;
        // the next pass resolves and pins the then-current materialization.
        m_actionQueue.prepend(result.runId);
        refreshQueuedRunReasons();
        return;
    }

    run->machine = machineNodeName();
    const ActionRun snapshot = *run;
    if (result.mirrorPin) {
        m_actionMirrorPins.insert(
            result.runId,
            {result.mirrorPin, result.mirror, snapshot.owner, snapshot.name});
    }
    idle->start(snapshot, workflow, result.mirror, result.workTree,
                ActionStore::variables());
    processActionQueue();
}

ActionRunner *MainWindow::runnerForRun(int runId) const
{
    for (ActionRunner *runner : m_actionRunners)
        if (runner->currentRunId() == runId)
            return runner;
    return nullptr;
}

void MainWindow::cancelSupersededRuns(const ActionRun &newRun)
{
    // Snapshot the matching run ids before acting on any of them. Cancelling a
    // Running run calls ActionRunner::stop(), which spins a nested event loop
    // (QProcess::waitForFinished) that can synchronously deliver the process's
    // finished() signal → onRunStatusChanged/onRunFinished, both of which do
    // `m_actionRuns = m_actionStore->loadAllRuns()`. Reassigning the vector
    // mid-iteration would invalidate a range-for reference into it and crash on
    // the next comparison, so we re-find each run by id instead of holding a
    // reference across that reentrancy.
    QList<int> supersededIds;
    for (const ActionRun &run : m_actionRuns) {
        if (run.id == newRun.id || run.owner != newRun.owner ||
            run.name != newRun.name || run.workflowPath != newRun.workflowPath ||
            run.pullNumber != newRun.pullNumber)
            continue;
        if (run.status == ActionStatus::Running ||
            run.status == ActionStatus::Queued ||
            run.status == ActionStatus::AwaitingApproval)
            supersededIds.append(run.id);
    }

    QList<ActionRun> cancelledRuns;
    for (int runId : supersededIds) {
        const ActionRun *found = findRun(runId);
        if (!found)
            continue; // reloaded away underneath us
        const ActionRun run = *found; // copy: acting below may reload m_actionRuns
        if (run.status == ActionStatus::Running) {
            if (ActionRunner *runner = runnerForRun(run.id)) {
                logSystem(QString::fromUtf8(
                              "Actions: aborting \"%1\" for %2/%3 @ %4 \xE2\x80\x94 "
                              "superseded by a newer run of the same workflow.")
                              .arg(run.workflowName, run.owner, run.name,
                                   run.commit.left(8)));
                runner->stop(); // records Cancelled once torn down
            }
        } else if (run.status == ActionStatus::Queued ||
                   run.status == ActionStatus::AwaitingApproval) {
            const bool wasPending = run.status == ActionStatus::AwaitingApproval;
            m_actionQueue.removeAll(run.id);
            if (ActionRun *live = findRun(runId)) {
                live->status = ActionStatus::Cancelled;
                live->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
                m_actionStore->saveRun(*live);
                scheduleMirrorActionsSummary(0);
                cancelledRuns.append(*live);
            }
            logSystem(QString::fromUtf8(
                          "Actions: cancelled %1 \"%2\" for %3/%4 @ %5 \xE2\x80\x94 "
                          "superseded by a newer run of the same workflow.")
                          .arg(wasPending ? QStringLiteral("pending")
                                          : QStringLiteral("queued"),
                               run.workflowName, run.owner, run.name,
                               run.commit.left(8)));
        }
    }
    deleteCancelledRunsAutomatically(cancelledRuns);
}

void MainWindow::onRunLog(int runId, const QString &text)
{
    scheduleMirrorActionsSummary(2000);
    QPlainTextEdit *view =
        runId == m_selectedRunId ? m_actionLog : parallelRunLog(runId);
    if (!view)
        return;
    view->moveCursor(QTextCursor::End);
    view->insertPlainText(displaySafePlainLog(text));
    view->moveCursor(QTextCursor::End);
}

void MainWindow::onRunStepChanged(int runId, int stepIndex,
                                  const QString &status)
{
    Q_UNUSED(runId)
    Q_UNUSED(stepIndex)
    Q_UNUSED(status)
    m_actionRuns = m_actionStore->loadAllRuns();
    scheduleMirrorActionsSummary(0);
    refreshWorkflowProgress();
    refreshActionsTable();
    refreshParallelRunColumn(runId);
    ringLocalControlRepos();
}

void MainWindow::onRunStatusChanged(int runId, const QString &status)
{
    m_actionRuns = m_actionStore->loadAllRuns();
    scheduleMirrorActionsSummary(0);
    refreshActionsTable();
    refreshCommitStatusGlyphs();
    if (runId == m_selectedRunId && m_actionRunMeta) {
        if (const ActionRun *run = findRun(runId))
            m_actionRunMeta->setText(actionRunMetaText(*run, status));
    }
    refreshParallelRunColumn(runId);
    updateNotificationButton();
    refreshOpenPullChecks();
    if (status == ActionStatus::Running) {
        if (const ActionRun *run = findRun(runId))
            notifyActionEvent(ActionPingEvent::Started,
                              QStringLiteral("Action started"),
                              QString::fromUtf8("%1 \xC2\xB7 %2/%3")
                                  .arg(run->workflowName, run->owner, run->name),
                              false, run->id);
    }
    updateMirrorActionsRuntimeState();
    ringLocalControlRepos();
}

void MainWindow::onRunFinished(int runId, bool ok)
{
    ringLocalControlRepos();
    Q_UNUSED(ok)
    m_actionRuns = m_actionStore->loadAllRuns();
    const ActionRun *stored = findRun(runId);
    if (!stored || (stored->status != ActionStatus::Success &&
                    stored->status != ActionStatus::Failed &&
                    stored->status != ActionStatus::Cancelled)) {
        // A runner may deliver a late completion while a replacement carrying
        // the same persisted run is already active. The store is authoritative:
        // do not announce a failure, release its mirror pin, or drain more work
        // while that run still says it is running.
        logSystem(QStringLiteral(
                      "Actions: ignored stale completion for run #%1; the "
                      "persisted run is still %2.")
                      .arg(runId)
                      .arg(stored ? actionStatusText(stored->status)
                                  : QStringLiteral("missing")));
        return;
    }
    const ActionRun run = *stored;
    releaseActionMirrorPin(runId);
    scheduleMirrorActionsSummary(0);
    refreshActionsTable();
    refreshCommitStatusGlyphs();
    updateNotificationButton();
    notePingFixRunFinished(run);
    {
        const bool succeeded = run.status == ActionStatus::Success;
        const bool cancelled = run.status == ActionStatus::Cancelled;
        const bool autoDeleteCancelledRun =
            cancelled &&
            QSettings()
                .value(kAutoDeleteCancelledRunsSetting, true)
                .toBool();
        const QString title = succeeded ? QStringLiteral("Action succeeded")
                                 : cancelled ? QStringLiteral("Action stopped")
                                             : QStringLiteral("Action failed");
        const QString heading =
            QString::fromUtf8("%1 \xC2\xB7 %2/%3")
                .arg(run.workflowName, run.owner, run.name);
        const QString body =
            succeeded || cancelled || !m_actionStore
                ? heading
                : ActionFailureSummary::pingBody(heading,
                                                 m_actionStore->readLog(run));
        const QString logLocation =
            autoDeleteCancelledRun
                ? QStringLiteral("Cancelled run will be deleted automatically.")
                : QStringLiteral("Run log: %1").arg(actionRunLogPath(run));
        logSystem(QStringLiteral("Actions: %1 run #%2 \"%3\" for %4/%5 @ %6. %7")
                      .arg(succeeded ? QStringLiteral("succeeded")
                              : cancelled ? QStringLiteral("stopped")
                                          : QStringLiteral("failed"))
                      .arg(run.id)
                      .arg(run.workflowName, run.owner, run.name,
                           run.commit.left(8), logLocation));
        // A cancelled run that is deleted automatically has no page left to
        // reopen, so its ping links nowhere.
        notifyActionEvent(succeeded ? ActionPingEvent::Succeeded
                          : cancelled ? ActionPingEvent::Stopped
                                      : ActionPingEvent::Failed,
                          title, body, !succeeded && !cancelled,
                          autoDeleteCancelledRun ? -1 : run.id);
        if (!succeeded && !cancelled)
            maybeAutoFixFailedRun(run);
    }
    if (runId == m_selectedRunId)
        showRun(runId); // finished: reload the complete log from disk
    else
        refreshParallelRunColumn(runId, true);
    refreshOpenPullChecks();
    processActionQueue();
    updateMirrorActionsRuntimeState();
    if (run.status == ActionStatus::Cancelled)
        deleteCancelledRunsAutomatically({run});
}

void MainWindow::onRunInterrupted(int runId)
{
    m_actionRuns = m_actionStore->loadAllRuns();
    const ActionRun *stored = findRun(runId);
    if (!stored)
        return;
    ActionRun run = *stored;
    releaseActionMirrorPin(runId);
    if (run.requeueAfterDesktopInterruption()) {
        m_actionStore->saveRun(run);
        if (!m_actionQueue.contains(run.id))
            m_actionQueue.append(run.id);
        m_actionRuns = m_actionStore->loadAllRuns();
    }
    logSystem(QStringLiteral(
                  "Actions: run #%1 \"%2\" for %3/%4 @ %5 was interrupted by a "
                  "stop signal to ForkMesh; it is queued again and restarts from "
                  "its approved snapshot. Run log: %6")
                  .arg(run.id)
                  .arg(run.workflowName, run.owner, run.name, run.commit.left(8),
                       actionRunLogPath(run)));
    scheduleMirrorActionsSummary(0);
    refreshActionsTable();
    refreshCommitStatusGlyphs();
    if (runId == m_selectedRunId)
        showRun(runId);
    else
        refreshParallelRunColumn(runId, true);
    QTimer::singleShot(60000, this, [this] { processActionQueue(); });
    updateMirrorActionsRuntimeState();
}

void MainWindow::onReleaseMetadataPrepared(int runId)
{
    const ActionRun *run = findRun(runId);
    if (!run || !run->ref.startsWith(QLatin1String("refs/tags/")))
        return;
    const QString tag =
        run->ref.mid(QStringLiteral("refs/tags/").size()).trimmed();
    const bool managedProduct =
        run->owner.trimmed().compare(QLatin1String("forkmesh"),
                                     Qt::CaseInsensitive) == 0 &&
        run->name.trimmed().compare(QLatin1String("forkmesh"),
                                    Qt::CaseInsensitive) == 0;
    if (!tag.isEmpty() && managedProduct) {
        // The release metadata commit has landed and its post-commit event is
        // now queued. Persist the gate synchronously before returning to the
        // event loop, so that ordinary commit processing cannot publish those
        // refs ahead of CAS seeding.
        discardSupersededReleaseIntents(run->owner, run->name, tag);
        rememberReleasePublication(run->owner, run->name, tag);
    }
}

void MainWindow::onReleaseMetadataLanded(int runId)
{
    const ActionRun *stored = findRun(runId);
    if (!stored)
        return;
    // The sync and Git helpers below can pump the event loop; keep a value copy
    // rather than a pointer into ActionStore's mutable run list.
    const ActionRun run = *stored;
    int index = repoIndexFor(run.owner, run.name);
    if (index < 0)
        return;
    const RepositoryRecord releaseRepo = m_repositories.at(index);
    // Land the small metadata commit into the served bare mirror immediately.
    // Do not start refreshRepositoryMirror() here: its normal completion path
    // fans refs out to managed hosts, while a release must seed the out-of-Git
    // CAS first or those refs authorize blobs the hosts do not have.
    bool servedMirrorReady = true;
    QString servedMirrorError;
    if (!releaseRepo.mirrorPath.trimmed().isEmpty() &&
        releaseRepo.localPath != releaseRepo.mirrorPath &&
        QDir(releaseRepo.mirrorPath).exists()) {
        servedMirrorReady = runGitCapture(
            releaseRepo.mirrorPath,
            {QStringLiteral("fetch"), releaseRepo.localPath,
             QStringLiteral("+refs/heads/*:refs/heads/*"),
             QStringLiteral("+refs/tags/*:refs/tags/*")},
            nullptr, &servedMirrorError);
        index = repoIndexFor(run.owner, run.name);
        if (index < 0)
            return;
    }
    logSystem(QStringLiteral(
                  "Release: refreshing artifact metadata for %1/%2 online.")
                  .arg(run.owner, run.name));
    refreshRepositoryList();
    if (index == m_repoDetailIndex && m_releasesTabIndex >= 0 &&
        m_repoDetailStack &&
        m_repoDetailStack->currentIndex() == m_releasesTabIndex)
        loadReleasesPanel();
    refreshRepoSyncIndicators();

    const QString tagPrefix = QStringLiteral("refs/tags/");
    if (!run.ref.startsWith(tagPrefix))
        return;
    const QString tag = run.ref.mid(tagPrefix.size()).trimmed();
    if (tag.isEmpty())
        return;
    const bool rolloutPending =
        releaseRolloutPending(run.owner, run.name, tag);
    bool publicationPending =
        releasePublicationPending(run.owner, run.name, tag);
    const bool prunePending =
        releaseArtifactPrunePending(run.owner, run.name, tag);
    if (!rolloutPending && !publicationPending && !prunePending)
        return;
    if (rolloutPending && !publicationPending) {
        // Compatibility with a rollout intent written by a desktop version
        // that predated the separate publication gate.
        rememberReleasePublication(run.owner, run.name, tag);
        publicationPending = true;
    }
    if (!servedMirrorReady) {
        const QString reason =
            servedMirrorError.trimmed().left(300).isEmpty()
                ? QStringLiteral("the local Git fetch failed")
                : servedMirrorError.trimmed().left(300);
        logSystem(QStringLiteral(
                      "Release: %1 remains pending because its metadata could "
                      "not be landed in the served mirror: %2")
                      .arg(tag, reason));
        if (index == m_repoDetailIndex) {
            setRepoDetailNotice(
                QStringLiteral(
                    "Release %1 is built, but its served mirror could not be "
                    "updated yet. The rollout remains pending.")
                    .arg(tag),
                true);
        }
        return;
    }

    PublishedReleaseDeployment publishedRelease;
    QString releaseError;
    if (!publishedReleaseDeployment(index, tag, run.commit,
                                    &publishedRelease, &releaseError)) {
        QFile landed(QDir(releaseRepo.localPath)
                         .filePath(QStringLiteral(
                             ".forkmesh/releases/latest/release.json")));
        const QByteArray landedBytes =
            landed.size() <= 1024 * 1024 && landed.open(QIODevice::ReadOnly)
                ? landed.readAll()
                : QByteArray();
        if (forkmesh::releasemanifest::stillNamesAnotherRelease(landedBytes,
                                                                tag)) {
            logSystem(QStringLiteral(
                          "Release: %1 remains pending: the landed latest "
                          "manifest still names %2, so its signed artifact set "
                          "is not published yet.")
                          .arg(tag, forkmesh::releasemanifest::landedTag(
                                        landedBytes)));
            return;
        }
        logSystem(QStringLiteral(
                      "Release: %1 landed, but its signed artifact set could "
                      "not be authenticated: %2")
                      .arg(tag, releaseError));
        if (index == m_repoDetailIndex)
            setRepoDetailNotice(releaseError, true);
        return; // keep the durable intent so a corrected rerun can finish it
    }

    if (!rolloutPending && !publicationPending) {
        pruneReleaseArtifacts(index, tag,
                              publishedRelease.artifactSha256s);
        clearReleaseArtifactPrune(run.owner, run.name, tag);
        return;
    }

    // Ordinary outbound ref fan-out is now gated for this metadata commit.
    // Seed every signed blob to managed hosts first; that method advances their
    // refs only after every transfer verifies. Once the managed fleet is
    // coherent, wake peers/configured gateways and finally deploy the exact
    // native artifact.
    const int managedTargets = pushReleaseToManagedHosts(
        index, tag, publishedRelease.artifactSha256s,
        [this, owner = run.owner, name = run.name, publishedRelease,
         rolloutPending](int succeeded, int failed) {
            if (failed > 0) {
                logSystem(QStringLiteral(
                              "Release: %1 remains pending because its signed "
                              "artifacts and refs did not reach %2 managed "
                              "host%3; deployment has not started.")
                              .arg(publishedRelease.tag)
                              .arg(failed)
                              .arg(failed == 1 ? QString()
                                               : QStringLiteral("s")));
                return;
            }
            Q_UNUSED(succeeded);
            if (!releasePublicationPending(
                    owner, name, publishedRelease.tag)) {
                logSystem(QStringLiteral(
                              "Release: %1 completed an obsolete mirror "
                              "fan-out; a newer publication remains in "
                              "control of refs and deployment.")
                              .arg(publishedRelease.tag));
                return;
            }
            const int currentIndex = repoIndexFor(owner, name);
            if (currentIndex < 0)
                return;
            propagateRepoUpdate(currentIndex,
                                /*includeManagedHosts=*/false);
            const int configuredTargets = pushToSshMirrorRemotes(
                currentIndex, /*userInitiated=*/true,
                publishedRelease.tag);
            // runGitCapture inside configured fan-out pumps the event loop. A
            // newer release can therefore supersede this one before its exact
            // push begins; never let the older callback prune or deploy then.
            if (!releasePublicationPending(
                    owner, name, publishedRelease.tag)) {
                return;
            }
            clearReleasePublication(owner, name, publishedRelease.tag);
            if (releaseArtifactPrunePending(
                    owner, name, publishedRelease.tag)) {
                pruneReleaseArtifacts(
                    currentIndex, publishedRelease.tag,
                    publishedRelease.artifactSha256s);
                clearReleaseArtifactPrune(owner, name,
                                          publishedRelease.tag);
            }
            logSystem(
                (rolloutPending
                     ? QStringLiteral(
                           "Release: %1 signed artifacts and refs are live on "
                           "every managed host; notified %2 configured mirror "
                           "target%3 and started deployment.")
                     : QStringLiteral(
                           "Release: %1 signed artifacts and refs are live on "
                           "every managed host; notified %2 configured mirror "
                           "target%3."))
                    .arg(publishedRelease.tag)
                    .arg(configuredTargets)
                    .arg(configuredTargets == 1
                             ? QString()
                             : QStringLiteral("s")));
            if (rolloutPending) {
                deployPendingReleaseRollout(owner, name, publishedRelease,
                                            /*managedRefsReady=*/true);
            }

            // Keep this source node on the code it just published: rebuild and
            // restart through the normal restart countdown. Running agent
            // sessions do not hold it back, because the desktop re-queues them
            // when it starts again; it waits only for a rollout still
            // installing on managed hosts and for a restart already under way.
            // A node already on this version stays put.
            const QSettings updateSettings;
            const bool autoRestart =
                updateSettings.value(kAutoUpdateSetting, false).toBool() &&
                updateSettings.value(kAutoUpdateRestartSetting, true).toBool();
            const QVersionNumber running =
                QVersionNumber::fromString(QStringLiteral(FORKMESH_VERSION));
            const QVersionNumber released =
                QVersionNumber::fromString(publishedRelease.version);
            if (autoRestart && !released.isNull() && running < released) {
                logSystem(QStringLiteral(
                              "Release: %1 is live on every managed mirror; "
                              "this node restarts onto it once its rollout "
                              "finishes.")
                              .arg(publishedRelease.tag));
                const QString releaseTag = publishedRelease.tag;
                const qint64 queuedAtMs = QDateTime::currentMSecsSinceEpoch();
                const auto attempt = std::make_shared<std::function<void()>>();
                const std::weak_ptr<std::function<void()>> weakAttempt = attempt;
                *attempt = [this, owner, name, releaseTag, queuedAtMs,
                            weakAttempt] {
                    // A rollout that cannot finish must not pin this node on
                    // the old build forever; its durable intent survives the
                    // restart and resumes afterwards.
                    const bool rolloutBusy =
                        QDateTime::currentMSecsSinceEpoch() - queuedAtMs <
                            30 * 60 * 1000 &&
                        (releaseRolloutPending(owner, name, releaseTag) ||
                         m_hostDeployRemaining > 0 ||
                         (m_hostInstallProcess &&
                          m_hostInstallProcess->state() !=
                              QProcess::NotRunning));
                    if (restartInProgress() || rolloutBusy) {
                        if (const auto next = weakAttempt.lock())
                            QTimer::singleShot(60 * 1000, this,
                                               [next] { (*next)(); });
                        return;
                    }
                    QPushButton *button =
                        m_navRebuildButton     ? m_navRebuildButton
                        : m_profileRebuildButton ? m_profileRebuildButton
                                                 : m_rebuildButton;
                    logSystem(QStringLiteral("Release: restarting onto %1.")
                                  .arg(releaseTag));
                    if (button)
                        startRestartSpin(button);
                    quickRebuildRestart();
                };
                QTimer::singleShot(0, this, [attempt] { (*attempt)(); });
            }
        });
    logSystem(QStringLiteral(
                  "Release: %1 artifact set landed; seeding %2 signed blob%3 "
                  "to %4 managed mirror target%5 before refs advance.")
                  .arg(tag)
                  .arg(publishedRelease.artifactSha256s.size())
                  .arg(publishedRelease.artifactSha256s.size() == 1
                           ? QString()
                           : QStringLiteral("s"))
                  .arg(managedTargets)
                  .arg(managedTargets == 1
                           ? QString()
                           : QStringLiteral("s")));
}

void MainWindow::deployPendingReleaseRollout(
    const QString &owner, const QString &name,
    const PublishedReleaseDeployment &publishedRelease,
    bool managedRefsReady)
{
    if (!publishedRelease.isValid() ||
        !releaseRolloutPending(owner, name, publishedRelease.tag)) {
        return;
    }
    // Do not turn a release into a second overlapping SSH fleet operation.
    // The durable intent survives both this short wait and an app restart.
    if ((m_hostInstallProcess &&
         m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0) {
        QTimer::singleShot(
            1000, this,
            [this, owner, name, publishedRelease, managedRefsReady] {
                deployPendingReleaseRollout(owner, name, publishedRelease,
                                            managedRefsReady);
            });
        return;
    }

    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    int activeHosts = 0;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (forkmesh::control::savedHostMayBeActive(host) &&
            !host.value(QStringLiteral("name")).toString().trimmed().isEmpty() &&
            !host.value(QStringLiteral("ip")).toString().trimmed().isEmpty() &&
            !host.value(QStringLiteral("user")).toString().trimmed().isEmpty()) {
            ++activeHosts;
        }
    }
    if (activeHosts == 0) {
        clearReleaseRollout(owner, name, publishedRelease.tag);
        logSystem(QStringLiteral(
                      "Release: %1 has no installed saved mirror hosts to "
                      "deploy; repository peers were still notified.")
                      .arg(publishedRelease.tag));
        return;
    }

    if (m_hostInstallStatus) {
        m_hostInstallStatus->setText(QStringLiteral(
            "Release %1 is ready; deploying its verified mirror-node artifact…")
                                         .arg(publishedRelease.tag));
    }
    m_hostDeployReleaseOwner = owner;
    m_hostDeployReleaseName = name;
    m_hostDeployReleaseTag = publishedRelease.tag;
    m_hostDeployReleaseRefsReady = managedRefsReady;
    runHostDeployAllParallel(FleetDeployMode::InstallBinary,
                             publishedRelease);
}

void MainWindow::refreshOpenPullChecks()
{
    if (m_currentPullNumber < 0)
        return;
    for (const PullRequest &it : std::as_const(m_currentPulls)) {
        if (it.number != m_currentPullNumber)
            continue;
        const PullRequest pr = it;
        renderPullChecks(pr);
        renderPullChecksSummary(pr);
        updatePullSubTabCounts(pr);
        return;
    }
}

void MainWindow::notifyHeldActionRuns(const QString &owner, const QString &name,
                                      const QString &commit,
                                      const ActionPullContext &pull,
                                      const QList<QPair<QString, int>> &held)
{
    QStringList workflows;
    for (const auto &run : held)
        workflows << run.first;
    const QString names =
        workflows.size() == 1
            ? workflows.first()
            : QStringList(workflows.mid(0, workflows.size() - 1))
                      .join(QStringLiteral(", ")) +
                  QStringLiteral(" and ") + workflows.last();
    const QString hold = held.size() == 1
                             ? QStringLiteral("this run stays held until you "
                                              "approve it")
                             : QStringLiteral("these %1 runs stay held until "
                                              "you approve them")
                                   .arg(held.size());
    const bool firstTime = pull.number > 0 && pull.firstTimeAuthor;
    QString title;
    QString body;
    if (firstTime && pull.authorPullOrdinal > 1) {
        title = QStringLiteral("%1 pull request needs your approval")
                    .arg(PullAuthorTrust::ordinal(pull.authorPullOrdinal));
        body = QString::fromUtf8(
                   "%1 \xC2\xB7 %2/%3 #%4 by %5 \xE2\x80\x94 their %6 pull "
                   "request here and none merged yet, so %7")
                   .arg(names, owner, name)
                   .arg(pull.number)
                   .arg(pull.author,
                        PullAuthorTrust::ordinal(pull.authorPullOrdinal), hold);
    } else if (firstTime) {
        title = QStringLiteral("First pull request needs your approval");
        body = QString::fromUtf8(
                   "%1 \xC2\xB7 %2/%3 #%4 by %5 \xE2\x80\x94 their first "
                   "pull request here, so %6")
                   .arg(names, owner, name)
                   .arg(pull.number)
                   .arg(pull.author, hold);
    } else {
        title = held.size() == 1
                    ? QStringLiteral("Action waiting for approval")
                    : QStringLiteral("%1 actions waiting for approval")
                          .arg(held.size());
        body = QString::fromUtf8("%1 \xC2\xB7 %2/%3 at %4")
                   .arg(names, owner, name, commit.left(8));
    }
    notifyActionEvent(ActionPingEvent::Approval, title, body, false,
                      held.first().second);
}

void MainWindow::notifyActionEvent(ActionPingEvent event, const QString &title,
                                   const QString &body, bool warning, int runId)
{
    // Settings > Pings > ACTION PINGS. A switched-off event files nothing —
    // no Pings row, no sound, no pop-up; the run's own page and the Actions
    // log still record it.
    if (!actionPingEnabled(event))
        return;
    addNotification(title, body, warning, runId);
    if (!actionPopupEnabled(event))
        return;
    const QString icon = warning ? QStringLiteral("dialog-error")
                         : event == ActionPingEvent::Started
                             ? QStringLiteral("system-run")
                         : event == ActionPingEvent::Approval
                             ? QStringLiteral("dialog-question")
                             : QStringLiteral("emblem-default");
    // Keep the OS card and the Pings row pointed at the run whose state
    // changed.  The Pings list already keeps runId, but without this href a
    // click on the alert itself was a dead end.
    const QString activateHref =
        runId > 0 ? QStringLiteral("fm:action:%1").arg(runId) : QString();
    postNotification(title, body, warning, icon, activateHref);
}

void MainWindow::addNotification(const QString &title, const QString &body,
                                 bool warning, int runId)
{
    AppNotification item;
    item.title = title;
    item.body = body;
    item.warning = warning;
    item.runId = runId;
    if (runId > 0) {
        item.kind = QStringLiteral("action");
        if (const ActionRun *run = findRun(runId))
            item.repo = run->owner + QLatin1Char('/') + run->name;
    }
    recordNotification(item);
}

void MainWindow::addNotification(const QString &title, const QString &body,
                                 bool warning, const NotificationLink &link)
{
    addNotification(title, body, warning, link, QString(), QString());
}

void MainWindow::addNotification(const QString &title, const QString &body,
                                 bool warning, const NotificationLink &link,
                                 const QString &kind, const QString &actor)
{
    AppNotification item;
    item.title = title;
    item.body = body;
    item.warning = warning;
    item.link = link;
    item.kind = kind;
    item.actor = actor;
    recordNotification(item);
}

qint64 MainWindow::recordNotification(AppNotification item)
{
    if (item.title.simplified().isEmpty() && item.body.simplified().isEmpty())
        return 0;
    item.id = m_nextNotificationId++;
    if (item.timestampMs <= 0)
        item.timestampMs = QDateTime::currentMSecsSinceEpoch();
    if (item.kind.isEmpty())
        item.kind = item.link.isValid() ? item.link.kind
                                        : QStringLiteral("desktop");
    if (item.repo.isEmpty() && !item.link.owner.isEmpty())
        item.repo = item.link.owner + QLatin1Char('/') + item.link.name;
    if (!item.syncDecided) {
        QString reason;
        item.sync = initialPingSync(item, &reason);
        item.syncReason = reason;
    }
    if (!item.quiet || item.forward)
        item.phonePingId = forwardPingToPhones(item);
    m_notifications.prepend(item);
    trimLocalPings();
    updateNotificationButton();
    scheduleNotificationJournalSave();
    if (!item.quiet)
        flashNotification(item);
    if (m_notificationsTable && m_sectionStack &&
        m_sectionStack->currentIndex() == 3)
        refreshNotificationsTable();
    notePingForFixes(item);
    return item.id;
}

void MainWindow::trimLocalPings()
{
    while (m_notifications.size() > kMaxLocalPings) {
        int victim = -1;
        for (int i = m_notifications.size() - 1; i >= 0; --i) {
            const AppNotification &item = m_notifications.at(i);
            if (item.dismissOnly && !item.acknowledged)
                continue;
            if (!item.warning || !forkmesh::pingSyncIsUnsynced(item.sync)) {
                victim = i;
                break;
            }
        }
        // A backlog of acknowledgement-required pings is allowed to exceed
        // the ordinary local history cap. Dropping one would make it silently
        // disappear before its required click.
        if (victim < 0)
            break;
        m_notifications.removeAt(victim);
    }
}

// What a ping's cloud state is the moment it is filed, before anything has been
// attempted.
// Only a failure has anywhere to go: it becomes a signed report in the relay's
// error log (reportUserVisibleError), which is how an error on a headless node
// or an unwatched machine is ever seen. Everything else is this machine's own
// history and says so, rather than implying a sync that was never going to
// happen. An alert raised with no relay to reach, or no account to sign with,
// is Offline: nothing was sent and nothing is queued, and the Pings page is the
// only record there will ever be of it.
// Deliberately never Pending: only the reporter knows whether a report is
// actually going out for this row, and it moves the row there itself. A ping
// nothing reports — a quiet OS notification, an alert raised with the in-app
// cards switched off — would otherwise sit at "Syncing…" forever waiting on a
// request nobody sent.
forkmesh::PingSync MainWindow::initialPingSync(const AppNotification &item,
                                               QString *reasonOut) const
{
    auto answer = [reasonOut](forkmesh::PingSync state, const QString &why) {
        if (reasonOut)
            *reasonOut = why;
        return state;
    };
    if (!item.warning)
        return answer(forkmesh::PingSync::LocalOnly,
                      QStringLiteral("a desktop event, not a failure — the "
                                     "cloud is never told about it"));
    if (forkmesh::ClientErrorReports::echoesRelayAlert(
            item.kind, item.title + QLatin1Char(' ') + item.body))
        return answer(forkmesh::PingSync::LocalOnly,
                      QStringLiteral("the relay raised this alert itself, so "
                                     "it is never reported back to it"));
    if (forkmesh::ClientErrorReports::describesLocalOperation(item.kind))
        return answer(forkmesh::PingSync::LocalOnly,
                      QStringLiteral("an operation on this machine refused — "
                                     "its own outcome, not a fault the cloud "
                                     "is told about"));
    if (!QSettings()
             .value(forkmesh::kReportUserVisibleErrorsSetting, false)
             .toBool())
        return answer(forkmesh::PingSync::LocalOnly,
                      QString::fromUtf8("sending errors to ForkMesh is turned "
                                     "off (Settings \xE2\x86\x92 General)"));
    if (!m_networkAccess)
        return answer(forkmesh::PingSync::Offline,
                      QStringLiteral("this node has no network access, so "
                                     "nothing was sent"));
    const QString owner = accountOwner();
    if (owner.isEmpty() || !m_profileIdentity.isValid()
        || !hasOwnerSigningCapability(owner))
        return answer(forkmesh::PingSync::Offline,
                      QStringLiteral("no signed-in account to sign a report "
                                     "with, so nothing was sent"));
    return answer(forkmesh::PingSync::LocalOnly,
                  QStringLiteral("kept on this machine unless it is reported "
                                 "to the relay"));
}

void MainWindow::setPingSync(qint64 pingId, forkmesh::PingSync state,
                             const QString &reason)
{
    if (pingId <= 0)
        return;
    for (AppNotification &item : m_notifications) {
        if (item.id != pingId)
            continue;
        if (item.sync == state && item.syncReason == reason)
            return;
        item.sync = state;
        item.syncReason = reason;
        scheduleNotificationJournalSave();
        if (m_notificationsTable && m_sectionStack &&
            m_sectionStack->currentIndex() == 3)
            refreshNotificationsTable();
        return;
    }
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testPingStatusFor(const QString &needle)
{
    constexpr int kTitleColumn = 2;
    constexpr int kStatusColumn = 6;
    if (m_notificationsTable) {
        refreshNotificationsTable();
        for (int row = 0; row < m_notificationsTable->rowCount(); ++row) {
            QTableWidgetItem *title =
                m_notificationsTable->item(row, kTitleColumn);
            QTableWidgetItem *status =
                m_notificationsTable->item(row, kStatusColumn);
            if (title && status && title->text().contains(needle))
                return status->text();
        }
        return {};
    }
    for (const AppNotification &item : std::as_const(m_notifications)) {
        if (item.title.contains(needle) || item.body.contains(needle))
            return forkmesh::pingSyncLabel(item.sync);
    }
    return {};
}
#endif


QString MainWindow::notificationJournalPath() const
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/pings.json");
}

void MainWindow::scheduleNotificationJournalSave()
{
    if (!m_notificationJournalTimer) {
        m_notificationJournalTimer = new QTimer(this);
        m_notificationJournalTimer->setSingleShot(true);
        m_notificationJournalTimer->setInterval(1500);
        connect(m_notificationJournalTimer, &QTimer::timeout, this,
                &MainWindow::saveNotificationJournal);
    }
    m_notificationJournalTimer->start();
}

void MainWindow::saveNotificationJournal()
{
    const QString path = notificationJournalPath();
    if (path.isEmpty())
        return;
    QJsonArray rows;
    for (const AppNotification &item : std::as_const(m_notifications)) {
        QJsonObject obj{
            {QStringLiteral("title"), item.title},
            {QStringLiteral("body"), item.body},
            {QStringLiteral("ts"), item.timestampMs},
            {QStringLiteral("warning"), item.warning},
            {QStringLiteral("kind"), item.kind},
            {QStringLiteral("actor"), item.actor},
            {QStringLiteral("repo"), item.repo},
            {QStringLiteral("sync"), forkmesh::pingSyncToken(item.sync)},
            {QStringLiteral("syncReason"), item.syncReason},
            {QStringLiteral("dismissOnly"), item.dismissOnly},
            {QStringLiteral("acknowledged"), item.acknowledged}};
        if (item.runId > 0)
            obj.insert(QStringLiteral("runId"), item.runId);
        if (!item.phonePingId.isEmpty())
            obj.insert(QStringLiteral("phonePingId"), item.phonePingId);
        if (item.link.isValid()) {
            obj.insert(QStringLiteral("link"),
                       QJsonObject{
                           {QStringLiteral("kind"), item.link.kind},
                           {QStringLiteral("owner"), item.link.owner},
                           {QStringLiteral("name"), item.link.name},
                           {QStringLiteral("number"), item.link.number},
                           {QStringLiteral("ref"), item.link.ref}});
        }
        rows.append(obj);
    }
    // Every notification rewrites the whole journal, and QSaveFile::commit()
    // fsyncs and renames — sampled blocking the GUI thread while an action was
    // finishing. The rows are serialised here and written on a worker; the
    // sequence number keeps a slow write from landing on top of a newer one.
    static QMutex writeMutex;
    static quint64 written = 0;      // guarded by writeMutex
    static std::atomic<quint64> next{0};
    const quint64 sequence = ++next;
    (void)QtConcurrent::run(
        [path, bytes = QJsonDocument(rows).toJson(QJsonDocument::Compact),
         sequence] {
            QMutexLocker locker(&writeMutex);
            if (sequence < written)
                return; // a newer journal already reached the disk
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return;
            file.write(bytes);
            if (file.commit())
                written = sequence;
        });
}

void MainWindow::loadNotificationJournal()
{
    const QString path = notificationJournalPath();
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonArray rows =
        QJsonDocument::fromJson(file.readAll()).array();
    QList<AppNotification> restored;
    for (const QJsonValue &value : rows) {
        const QJsonObject obj = value.toObject();
        AppNotification item;
        item.id = m_nextNotificationId++;
        item.title = obj.value(QStringLiteral("title")).toString();
        item.body = obj.value(QStringLiteral("body")).toString();
        item.timestampMs = qint64(obj.value(QStringLiteral("ts")).toDouble());
        item.warning = obj.value(QStringLiteral("warning")).toBool();
        item.kind = obj.value(QStringLiteral("kind")).toString();
        item.actor = obj.value(QStringLiteral("actor")).toString();
        item.repo = obj.value(QStringLiteral("repo")).toString();
        item.sync = forkmesh::pingSyncFromToken(
            obj.value(QStringLiteral("sync")).toString());
        item.syncReason = obj.value(QStringLiteral("syncReason")).toString();
        item.dismissOnly = obj.value(QStringLiteral("dismissOnly")).toBool();
        item.acknowledged = obj.value(QStringLiteral("acknowledged")).toBool();
        item.runId = obj.value(QStringLiteral("runId")).toInt(-1);
        item.phonePingId = obj.value(QStringLiteral("phonePingId")).toString();
        const forkmesh::PingSync settled =
            forkmesh::pingSyncAfterRestart(item.sync);
        if (settled != item.sync) {
            item.sync = settled;
            item.syncReason = forkmesh::pingSyncRestartReason();
        }
        const QJsonObject link = obj.value(QStringLiteral("link")).toObject();
        if (!link.isEmpty()) {
            item.link.kind = link.value(QStringLiteral("kind")).toString();
            item.link.owner = link.value(QStringLiteral("owner")).toString();
            item.link.name = link.value(QStringLiteral("name")).toString();
            item.link.number = link.value(QStringLiteral("number")).toInt();
            item.link.ref = link.value(QStringLiteral("ref")).toString();
        }
        if (item.title.isEmpty() && item.body.isEmpty())
            continue;
        restored.append(item);
    }
    m_notifications.append(restored);
    purgePrivateChatPings(savedPrivateChannels());
    trimLocalPings();
    updateNotificationButton();
    if (m_notificationsTable)
        refreshNotificationsTable();
}

void MainWindow::acknowledgePing(qint64 pingId)
{
    if (pingId <= 0)
        return;
    for (AppNotification &item : m_notifications) {
        if (item.id != pingId || !item.dismissOnly || item.acknowledged)
            continue;
        item.acknowledged = true;
        // A click is the point at which this ping becomes settled. Start its
        // durable write now instead of leaving a fresh acknowledgement parked
        // behind the ordinary debounce timer.
        saveNotificationJournal();
        if (m_notificationsTable && m_sectionStack &&
            m_sectionStack->currentIndex() == 3) {
            refreshNotificationsTable();
        }
        return;
    }
}

void MainWindow::flashNotification(const AppNotification &item)
{
    if (!item.dismissOnly && pingsPaused())
        return;
    if (!forkmesh::pings::pingKindEnabled(item.kind))
        return;
    QString text = item.title.simplified();
    const QString detail = item.body.simplified();
    if (!detail.isEmpty())
        text += QString::fromUtf8(" \xE2\x80\x94 ") + detail;
    if (text.isEmpty())
        return;
    const qint64 previousAnnouncingPingId = m_announcingPingId;
    m_announcingPingId = item.id;
    const QString clickHref =
        item.kind == QLatin1String("action") && item.runId > 0
            ? QStringLiteral("fm:action:%1").arg(item.runId)
            : QString();
    flashPingMessage(text, item.warning, clickHref, item.kind, item.runId);
    if (item.warning)
        flashErrorBorder();
    m_announcingPingId = previousAnnouncingPingId;
}

void MainWindow::flashErrorBorder()
{
    if (!m_errorBorderOverlay) {
        class ErrorBorderWidget : public QWidget
        {
        public:
            explicit ErrorBorderWidget(QWidget *parent) : QWidget(parent)
            {
                setAttribute(Qt::WA_TransparentForMouseEvents);
                setAttribute(Qt::WA_NoSystemBackground);
                setAttribute(Qt::WA_TranslucentBackground);
            }

        protected:
            void paintEvent(QPaintEvent *) override
            {
                paintEdgeGlow(this, QColor(248, 81, 73, 184), 6, 56, 8);
            }
        };
        m_errorBorderOverlay = new ErrorBorderWidget(this);
        m_errorBorderTimer = new QTimer(this);
        m_errorBorderTimer->setSingleShot(true);
        connect(m_errorBorderTimer, &QTimer::timeout, this, [this] {
            if (m_errorBorderOverlay)
                m_errorBorderOverlay->hide();
        });
    }
    m_errorBorderOverlay->setGeometry(rect());
    m_errorBorderOverlay->show();
    m_errorBorderOverlay->raise();
    m_errorBorderTimer->start(1500); // world-admin-error-arrival's 1.5s
}

void MainWindow::flashCelebrationBorder()
{
    if (!m_celebrationBorderOverlay) {
        class CelebrationBorderWidget : public QWidget
        {
        public:
            explicit CelebrationBorderWidget(QWidget *parent) : QWidget(parent)
            {
                setAttribute(Qt::WA_TransparentForMouseEvents);
                setAttribute(Qt::WA_NoSystemBackground);
                setAttribute(Qt::WA_TranslucentBackground);
                setObjectName(QStringLiteral("celebrationBorderOverlay"));
            }

        protected:
            void paintEvent(QPaintEvent *) override
            {
                paintEdgeGlow(this, QColor(63, 185, 80, 190), 6, 58, 8);
            }
        };
        m_celebrationBorderOverlay = new CelebrationBorderWidget(this);
        m_celebrationBorderTimer = new QTimer(this);
        m_celebrationBorderTimer->setSingleShot(true);
        connect(m_celebrationBorderTimer, &QTimer::timeout, this, [this] {
            if (m_celebrationBorderOverlay)
                m_celebrationBorderOverlay->hide();
        });
    }
    m_celebrationBorderOverlay->setGeometry(rect());
    m_celebrationBorderOverlay->show();
    m_celebrationBorderOverlay->raise();
    m_celebrationBorderTimer->start(2000);
}

void MainWindow::startRestartCautionFlash()
{
    if (!m_restartCautionBorderOverlay) {
        class RestartCautionBorderWidget : public QWidget
        {
        public:
            explicit RestartCautionBorderWidget(QWidget *parent) : QWidget(parent)
            {
                setAttribute(Qt::WA_TransparentForMouseEvents);
                setAttribute(Qt::WA_NoSystemBackground);
                setAttribute(Qt::WA_TranslucentBackground);
                setObjectName(QStringLiteral("restartCautionBorderOverlay"));
            }

        protected:
            void paintEvent(QPaintEvent *) override
            {
                paintEdgeGlow(this, QColor(210, 153, 34, 220), 5, 62, 10);
            }
        };
        m_restartCautionBorderOverlay = new RestartCautionBorderWidget(this);
        m_restartCautionBorderTimer = new QTimer(this);
        connect(m_restartCautionBorderTimer, &QTimer::timeout, this, [this] {
            if (!m_restartCautionBorderOverlay)
                return;
            if (m_restartCautionBorderOverlay->isVisible()) {
                m_restartCautionBorderOverlay->hide();
                return;
            }
            m_restartCautionBorderOverlay->setGeometry(rect());
            m_restartCautionBorderOverlay->show();
            m_restartCautionBorderOverlay->raise();
        });
    }
    m_restartCautionBorderOverlay->setGeometry(rect());
    m_restartCautionBorderOverlay->show();
    m_restartCautionBorderOverlay->raise();
    m_restartCautionBorderTimer->start(650);
}

void MainWindow::stopRestartCautionFlash()
{
    if (m_restartCautionBorderTimer)
        m_restartCautionBorderTimer->stop();
    if (m_restartCautionBorderOverlay)
        m_restartCautionBorderOverlay->hide();
}

// The internal schemes the in-app notices link with, resolved into the same
// navigation record every other ping carries so the Pings row, the tray click
// and the notification action all land in one place.
NotificationLink MainWindow::notificationLinkForHref(const QString &href)
{
    NotificationLink link;
    QString coveOwner;
    QString coveRepository;
    QString coveId;
    if (forkmesh::cove::parseShareUrl(href, &coveOwner, &coveRepository,
                                     &coveId)) {
        link.kind = QStringLiteral("cove");
        link.owner = coveOwner;
        link.name = coveRepository;
        link.ref = coveId;
        return link;
    }
    link = packageRailLinkForHref(href);
    if (link.isValid())
        return link;
    const QString actionPrefix = QStringLiteral("fm:action:");
    if (href.startsWith(actionPrefix)) {
        bool ok = false;
        const int runId = href.mid(actionPrefix.size()).toInt(&ok);
        if (!ok || runId <= 0)
            return link;
        link.kind = QStringLiteral("action");
        link.number = runId;
        if (const ActionRun *run = findRun(runId)) {
            link.owner = run->owner;
            link.name = run->name;
        }
        return link;
    }
    const QString prefix = QStringLiteral("fm:agent:");
    if (!href.startsWith(prefix))
        return link;
    bool ok = false;
    const int sessionId = href.mid(prefix.size()).toInt(&ok);
    if (!ok || sessionId <= 0)
        return link;
    link.kind = QStringLiteral("agent");
    link.number = sessionId;
    if (const AgentSession *session = findAgentSession(sessionId)) {
        link.owner = session->owner;
        link.name = session->name;
    }
    return link;
}

// A notification the OS drew is often the only ForkMesh on screen — and after a
// long run the window is usually buried or minimized. Clicking has to bring the
// window back before it can navigate anywhere.
void MainWindow::activateDesktopNotification(const QString &href)
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
    const NotificationLink link = notificationLinkForHref(href);
    if (link.isValid())
        openNotificationLink(link);
    else
        showNotifications(); // no target of its own: the Pings page has the text
}

void MainWindow::openNotificationLink(const NotificationLink &link)
{
    if (!link.isValid())
        return;
    if (link.kind == QLatin1String("action")) {
        openActionRunFromNotification(link.number);
        return;
    }
    if (link.kind == QLatin1String("agent")) {
        switchToAgentsTab(link.number); // straight to that run's detail
        return;
    }
    if (link.kind == QLatin1String("chat")) {
        showChatView();
        if (!link.ref.isEmpty())
            switchConversation(link.ref);
        return;
    }
    if (link.kind == QLatin1String("search")) {
        openSavedSearch(link.ref); // ref is the saved search id
        return;
    }
    if (link.kind == QLatin1String("firewall")) {
        openFirewallRules(link.ref);
        return;
    }
    if (openPackageRailLink(link))
        return;
    const int index = repoIndexFor(link.owner, link.name);
    if (index < 0) {
        flashMessage(QStringLiteral("That repository isn't on this node anymore."),
                     true);
        return;
    }
    if (link.kind == QLatin1String("pull") && link.number > 0) {
        openRepoDetail(index, 4);
        if (m_repoDetailIndex == index)
            openPullFromNotification(link.number);
        return;
    }
    openRepoDetail(index);
    auto selectTab = [this](int tab) {
        if (m_repoDetailTabs && m_repoDetailTabs->button(tab))
            m_repoDetailTabs->button(tab)->setChecked(true);
        if (m_repoDetailStack)
            m_repoDetailStack->setCurrentIndex(tab);
    };
    if (link.kind == QLatin1String("issue")) {
        selectTab(2); // Issues
        if (link.number > 0)
            showIssue(link.number);
    } else if (link.kind == QLatin1String("pull")) {
        selectTab(4);
    } else if (link.kind == QLatin1String("discussion")) {
        selectTab(5); // Discussions
        if (link.number > 0)
            showDiscussion(link.number);
    } else if (link.kind == QLatin1String("commit")) {
        showOverviewCommits(); // the universal Git workspace
        if (!link.ref.isEmpty())
            showCommit(link.ref);
    } else if (link.kind == QLatin1String("cove")) {
        m_coveExplorerCurrentId = link.ref;
        showRepoCoveExplorer();
    } else if (link.kind == QLatin1String("release")) {
        if (m_releasesTabIndex >= 0)
            selectTab(m_releasesTabIndex);
    }
}

int MainWindow::pendingActionCount() const
{
    int count = 0;
    for (const ActionRun &run : m_actionRuns)
        if (run.status == ActionStatus::AwaitingApproval)
            ++count;
    return count;
}

namespace {

const QStringList &actionBadgeIconPool()
{
    static const QStringList pool = {
        QStringLiteral("workflow"),       QStringLiteral("rocket"),
        QStringLiteral("package"),        QStringLiteral("terminal"),
        QStringLiteral("sparkle"),        QStringLiteral("star"),
        QStringLiteral("graph"),          QStringLiteral("pie-chart"),
        QStringLiteral("database"),       QStringLiteral("cloud"),
        QStringLiteral("server"),         QStringLiteral("tag"),
        QStringLiteral("code"),           QStringLiteral("stopwatch"),
        QStringLiteral("broadcast"),      QStringLiteral("crown"),
        QStringLiteral("sun"),            QStringLiteral("moon"),
        QStringLiteral("loop"),           QStringLiteral("git-branch"),
        QStringLiteral("note"),           QStringLiteral("shield-check"),
        QStringLiteral("key"),            QStringLiteral("file-directory"),
        QStringLiteral("device-desktop"), QStringLiteral("device-mobile"),
        QStringLiteral("upload"),         QStringLiteral("download"),
        QStringLiteral("git-merge"),      QStringLiteral("lock"),
        QStringLiteral("search"),         QStringLiteral("calendar"),
    };
    return pool;
}

QString actionBadgeKeywordIcon(const QString &text)
{
    static const QList<QPair<QStringList, QString>> keywords = {
        {{QStringLiteral("deploy"), QStringLiteral("ship"),
          QStringLiteral("launch")},
         QStringLiteral("rocket")},
        {{QStringLiteral("release"), QStringLiteral("tag"),
          QStringLiteral("version")},
         QStringLiteral("tag")},
        {{QStringLiteral("publish"), QStringLiteral("upload")},
         QStringLiteral("upload")},
        {{QStringLiteral("build"), QStringLiteral("compile"),
          QStringLiteral("package"), QStringLiteral("bundle")},
         QStringLiteral("package")},
        {{QStringLiteral("test"), QStringLiteral("check"),
          QStringLiteral("verify"), QStringLiteral("ci")},
         QStringLiteral("shield-check")},
        {{QStringLiteral("lint"), QStringLiteral("format"),
          QStringLiteral("style")},
         QStringLiteral("code")},
        {{QStringLiteral("doc"), QStringLiteral("site"),
          QStringLiteral("page")},
         QStringLiteral("note")},
        {{QStringLiteral("backup"), QStringLiteral("migrat"),
          QStringLiteral("db"), QStringLiteral("database")},
         QStringLiteral("database")},
        {{QStringLiteral("sync"), QStringLiteral("mirror")},
         QStringLiteral("sync")},
        {{QStringLiteral("nightly"), QStringLiteral("cron"),
          QStringLiteral("schedule"), QStringLiteral("daily")},
         QStringLiteral("calendar")},
        {{QStringLiteral("bench"), QStringLiteral("perf")},
         QStringLiteral("stopwatch")},
        {{QStringLiteral("audit"), QStringLiteral("scan"),
          QStringLiteral("security")},
         QStringLiteral("search")},
        {{QStringLiteral("android"), QStringLiteral("ios"),
          QStringLiteral("mobile"), QStringLiteral("phone")},
         QStringLiteral("device-mobile")},
        {{QStringLiteral("desktop"), QStringLiteral("windows"),
          QStringLiteral("macos"), QStringLiteral("linux")},
         QStringLiteral("device-desktop")},
        {{QStringLiteral("agent"), QStringLiteral("ai")},
         QStringLiteral("sparkle")},
        {{QStringLiteral("merge")}, QStringLiteral("git-merge")},
        {{QStringLiteral("sign"), QStringLiteral("secret")},
         QStringLiteral("key")},
        {{QStringLiteral("server"), QStringLiteral("host"),
          QStringLiteral("node")},
         QStringLiteral("server")},
    };
    const QStringList words = text.toLower().split(
        QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts);
    for (const auto &entry : keywords)
        for (const QString &word : words)
            for (const QString &keyword : entry.first)
                if (word == keyword ||
                    (keyword.size() > 2 && word.startsWith(keyword)))
                    return entry.second;
    return QString();
}

quint32 actionBadgeHash(const QString &text)
{
    quint32 h = 2166136261u;
    for (const char c : text.toUtf8())
        h = (h ^ static_cast<quint8>(c)) * 16777619u;
    return h;
}

QString actionBadgeKey(const ActionRun &run)
{
    return run.repoKey() + QLatin1Char('\n') + run.workflowPath;
}

} // namespace

QString MainWindow::actionWorkflowIconName(const ActionRun &run)
{
    const QString key = actionBadgeKey(run);
    const auto known = m_actionWorkflowIcons.constFind(key);
    if (known != m_actionWorkflowIcons.constEnd())
        return known.value();
    const QList<QString> taken = m_actionWorkflowIcons.values();
    const QStringList &pool = actionBadgeIconPool();
    QString icon = actionBadgeKeywordIcon(
        run.workflowName + QLatin1Char(' ') +
        QFileInfo(run.workflowPath).completeBaseName());
    if (icon.isEmpty() || taken.contains(icon)) {
        const int start = int(actionBadgeHash(key) % quint32(pool.size()));
        icon = pool.at(start);
        for (int step = 0; step < pool.size(); ++step) {
            const QString candidate = pool.at((start + step) % pool.size());
            if (!taken.contains(candidate)) {
                icon = candidate;
                break;
            }
        }
    }
    m_actionWorkflowIcons.insert(key, icon);
    return icon;
}

void MainWindow::refreshActionRunStrip()
{
    if (!m_actionRunStrip)
        return;
    QVector<ActionRunStrip::Cell> cells;
    QVector<ActionRunStrip::Badge> badges;
    QSet<QString> badgeKeys;
    QStringList lines;
    QStringList runningLines;
    for (const ActionRun &run : std::as_const(m_actionRuns)) {
        const QString label = run.workflowName.isEmpty() ? run.workflowPath
                                                         : run.workflowName;
        if (run.status == ActionStatus::Running &&
            badges.size() < ActionRunStrip::kMaxBadges) {
            const QString key = actionBadgeKey(run);
            if (!badgeKeys.contains(key)) {
                badgeKeys.insert(key);
                ActionRunStrip::Badge badge;
                badge.runId = run.id;
                badge.iconName = actionWorkflowIconName(run);
                badge.color = Theme::nextOr(Theme::Role::AccentFg,
                                            insightContributorColor(key));
                badges.append(badge);
                runningLines << QString::fromUtf8("%1 \xE2\x80\x94 %2")
                                    .arg(label, run.name);
            }
        }
        if (cells.size() >= ActionRunStrip::kMaxCells)
            continue;
        ActionRunStrip::Cell cell;
        cell.runId = run.id;
        cell.color = actionStatusColor(run.status);
        cell.running = run.status == ActionStatus::Running;
        cells.append(cell);
        lines << QString::fromUtf8("%1 \xE2\x80\x94 %2 (%3)")
                     .arg(label, actionStatusText(run.status), run.name);
    }
    m_actionRunStrip->setCells(cells, badges);
    m_actionRunStrip->setVisible(
        !cells.isEmpty() && !topBarIndicatorHidden(QStringLiteral("actions")));
    updateChromeDotDivider();
    if (cells.isEmpty()) {
        m_actionRunStripTooltipKey.clear();
        return;
    }
    const QString key = badges.isEmpty()
                            ? lines.join(QLatin1Char('\n'))
                            : QStringLiteral("running\n") +
                                  runningLines.join(QLatin1Char('\n'));
    if (key == m_actionRunStripTooltipKey)
        return;
    m_actionRunStripTooltipKey = key;
    if (!badges.isEmpty()) {
        m_actionRunStrip->setToolTip(
            QStringLiteral("%1 action%2 running\n%3\n"
                           "Click an icon to open that run.")
                .arg(badges.size())
                .arg(badges.size() == 1 ? QString() : QStringLiteral("s"),
                     runningLines.join(QLatin1Char('\n'))));
        return;
    }
    m_actionRunStrip->setToolTip(
        QStringLiteral("%1 most recent action run%2, newest first\n%3\n"
                       "Click a square to open that run.")
            .arg(cells.size())
            .arg(cells.size() == 1 ? QString() : QStringLiteral("s"), key));
}

int MainWindow::unseenLocalPingCount()
{
    if (m_localPingsSeenAtMs < 0) {
        QSettings settings;
        m_localPingsSeenAtMs =
            settings.value(kLocalPingsSeenAtSetting, -1).toLongLong();
        // First run with the mark: the journal's backlog is old news, not a
        // badge of every ping this machine ever kept.
        if (m_localPingsSeenAtMs < 0) {
            m_localPingsSeenAtMs = QDateTime::currentMSecsSinceEpoch();
            settings.setValue(kLocalPingsSeenAtSetting, m_localPingsSeenAtMs);
        }
    }
    int count = 0;
    for (const AppNotification &item : std::as_const(m_notifications))
        if ((item.timestampMs > m_localPingsSeenAtMs &&
             item.sync != forkmesh::PingSync::Synced) ||
            (item.dismissOnly && !item.acknowledged))
            ++count;
    return count;
}

void MainWindow::markLocalPingsSeen()
{
    bool acknowledged = false;
    for (AppNotification &item : m_notifications) {
        if (item.dismissOnly && !item.acknowledged) {
            item.acknowledged = true;
            acknowledged = true;
        }
    }
    if (acknowledged)
        saveNotificationJournal();
    qint64 newest = 0;
    for (const AppNotification &item : std::as_const(m_notifications))
        newest = qMax(newest, item.timestampMs);
    if (m_localPingsSeenAtMs >= 0 && newest <= m_localPingsSeenAtMs)
        return; // nothing new: skip the QSettings rewrite
    m_localPingsSeenAtMs =
        qMax(newest, QDateTime::currentMSecsSinceEpoch());
    QSettings().setValue(kLocalPingsSeenAtSetting, m_localPingsSeenAtMs);
}

void MainWindow::updateNotificationButton()
{
    refreshActionRunStrip();
    if (!m_notificationButton)
        return;
    // A ping filed while the Pings page is on screen has been seen.
    if (m_sectionStack && m_sectionStack->currentIndex() == 3 &&
        isActiveWindow())
        markLocalPingsSeen();
    const int approvals = pendingActionCount();
    const int localUnseen = unseenLocalPingCount();
    const int pending = approvals + localUnseen + m_webAlertsUnread;
    QStringList tips;
    // First, because it changes what the counts below mean: they are still
    // arriving, they just are not interrupting anyone.
    if (const qint64 pausedUntil = pingPauseUntilMs())
        tips << pingPauseLabel(pausedUntil);
    else if (forkmesh::pings::quietUntilMs() > 0)
        tips << forkmesh::pings::quietHoursSummary(
            forkmesh::pings::loadQuietHours());
    if (approvals > 0)
        tips << QStringLiteral("%1 action(s) waiting for approval").arg(approvals);
    if (localUnseen > 0)
        tips << QStringLiteral("%1 new ping(s) on this desktop").arg(localUnseen);
    if (m_webAlertsUnread > 0)
        tips << QStringLiteral("%1 unread website ping(s)").arg(m_webAlertsUnread);
    m_notificationButton->setToolTip(
        tips.isEmpty() ? QStringLiteral("Pings")
                       : tips.join(QString::fromUtf8(" \xC2\xB7 ")));
    if (auto *railButton =
            dynamic_cast<ActivityRailButton *>(m_notificationButton)) {
        railButton->setAlertTint(pending > 0);
        railButton->setBadgeCount(pending);
    }
}

void MainWindow::setPingPause(qint64 untilMs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Every client says "resume" by asking for a deadline that has already
    // passed; there is no separate verb. The cap mirrors the relay's, which
    // clamps anything longer, so a pause is always something you wake up from.
    if (untilMs <= now)
        untilMs = 0;
    else
        untilMs = qMin(untilMs, now + kPingPauseMaxMs);
    if (untilMs > 0)
        QSettings().setValue(kPingPauseUntilSetting, untilMs);
    else
        QSettings().remove(kPingPauseUntilSetting);
    m_pendingPingPausePush = true;
    QSettings().setValue(kPingPausePushPendingSetting, true);
    sendNodeHeartbeat(); // hand it to the relay now; the timer retries if offline
    refreshPingPauseSurfaces();
    addNotification(untilMs > 0 ? pingPauseLabel(untilMs)
                                : QStringLiteral("Notifications resumed"),
                    untilMs > 0
                        ? QStringLiteral(
                              "Ping notifications, emails, and pop-ups are muted "
                              "until then. Pings still arrive on this page.")
                        : QStringLiteral(
                              "Ping notifications, emails, and pop-ups are back on."),
                    false);
}

void MainWindow::refreshPingPauseSurfaces()
{
    const qint64 pausedUntil = pingPauseUntilMs();
    // Quiet hours (#347) mute exactly like a pause, so they read like one
    // while no pause is running; the label names which it is.
    const qint64 quietUntil = forkmesh::pings::quietUntilMs();
    const qint64 until = pausedUntil > 0 ? pausedUntil : quietUntil;
    const QString label =
        pausedUntil > 0 || quietUntil <= 0
            ? pingPauseLabel(pausedUntil)
            : QStringLiteral("Quiet hours until %1")
                  .arg(QLocale().toString(
                      QDateTime::fromMSecsSinceEpoch(quietUntil).time(),
                      QLocale::ShortFormat));
    if (m_pingPauseButton) {
        m_pingPauseButton->setText(
            pausedUntil > 0  ? QStringLiteral("Notifications paused")
            : quietUntil > 0 ? QStringLiteral("Quiet hours")
                             : QStringLiteral("Pause notifications"));
        m_pingPauseButton->setToolTip(
            until > 0 ? label
                      : QStringLiteral(
                            "Pause ping notifications, emails, and pop-ups"));
    }
    if (m_pingPauseStatus)
        m_pingPauseStatus->setText(label);
    // One shot at the deadline rather than a ticking timer: nothing else about
    // a pause changes while it runs.
    if (!m_pingPauseExpiryTimer) {
        m_pingPauseExpiryTimer = new QTimer(this);
        m_pingPauseExpiryTimer->setSingleShot(true);
        connect(m_pingPauseExpiryTimer, &QTimer::timeout, this,
                [this] { refreshPingPauseSurfaces(); });
    }
    m_pingPauseExpiryTimer->stop();
    if (until > 0) {
        const qint64 remaining = until - QDateTime::currentMSecsSinceEpoch();
        // Capped because QTimer takes an int: a month-long pause overflows it,
        // and re-arming on the way there costs nothing.
        m_pingPauseExpiryTimer->start(
            static_cast<int>(qBound(qint64(1000), remaining + 1000,
                                    qint64(kPingPauseHourMs))));
    }
    updateNotificationButton();
}

void MainWindow::popupPingPauseMenu(QWidget *anchor)
{
    // Stack-allocated and exec'd, the way every other menu in this file is
    // built: the actions are compared against exec()'s return rather than run
    // from a lambda, so nothing can be invoked out of a menu that is already
    // tearing itself down while the Custom dialog holds the event loop.
    QMenu menu(anchor);
    QAction *hour = menu.addAction(QStringLiteral("For 1 hour"));
    QAction *day = menu.addAction(QStringLiteral("For 1 day"));
    QAction *week = menu.addAction(QStringLiteral("Until next week"));
    QAction *custom = menu.addAction(QString::fromUtf8("Custom\xE2\x80\xA6")); // …
    QAction *resume = nullptr;
    if (pingPauseUntilMs() > 0) {
        menu.addSeparator();
        resume = menu.addAction(QStringLiteral("Resume notifications"));
    }
    QAction *chosen =
        menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
    if (!chosen)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (chosen == hour)
        setPingPause(now + kPingPauseHourMs);
    else if (chosen == day)
        setPingPause(now + 24 * kPingPauseHourMs);
    else if (chosen == week)
        setPingPause(now + 7 * 24 * kPingPauseHourMs);
    else if (chosen == resume)
        setPingPause(0);
    else if (chosen == custom) {
        // Hours rather than a calendar widget: a snooze is chosen relative to
        // now ("give me three hours"), and this keeps the ask to one number.
        const qint64 running = pingPauseUntilMs();
        bool ok = false;
        const double hours = QInputDialog::getDouble(
            this, QStringLiteral("Pause notifications"),
            QStringLiteral("Pause notifications for how many hours?"),
            running > 0 ? qBound(0.25, double(running - now) / kPingPauseHourMs,
                                 double(kPingPauseMaxHours))
                        : 4.0,
            0.25, double(kPingPauseMaxHours), 2, &ok);
        if (ok)
            setPingPause(QDateTime::currentMSecsSinceEpoch()
                         + qint64(hours * kPingPauseHourMs));
    }
}

void MainWindow::openActionRunFromNotification(int runId)
{
    const ActionRun *run = findRun(runId);
    if (!run)
        return;
    const int index = repoIndexFor(run->owner, run->name);
    if (index < 0)
        return;
    // Land on Actions directly; opening on Code first flashed the file list
    // before the tab switch below.
    openRepoDetail(index, 6);
    if (m_repoDetailTabs && m_repoDetailTabs->button(6))
        m_repoDetailTabs->button(6)->click();
    else if (m_repoDetailStack) {
        ensureRepoDetailTabBuilt(6);
        m_repoDetailStack->setCurrentIndex(6);
    }
    refreshRepoActions();
    if (m_actionsTable) {
        for (int row = 0; row < m_actionsTable->rowCount(); ++row) {
            QTableWidgetItem *item = m_actionsTable->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt() == runId) {
                m_actionsTable->selectRow(row);
                break;
            }
        }
    }
    showRun(runId);
}


namespace {
// Column order for the Pings grid. Prompt is a button column: it holds no text
// and takes no part in content sizing.
enum NotificationColumn {
    NotificationColType = 0,
    NotificationColKind,
    NotificationColTitle,
    NotificationColDetail,
    NotificationColRepository,
    NotificationColFrom,
    NotificationColStatus,
    NotificationColWhen,
    NotificationColLink,
    NotificationColSettings,
    NotificationColPrompt,
    NotificationColumnCount,
};
constexpr int kNotificationColumns = NotificationColumnCount;
constexpr int kNotificationPromptWidth = 76;
constexpr int kNotificationSettingsWidth = 72;

// The full, unclipped cell text. Copy row and the tooltip read it so neither is
// limited to what the column had room to show.
constexpr int kPingFullTextRole = Qt::UserRole + 5;

constexpr int kPingOpensRole = Qt::UserRole + 6;
constexpr int kPingUnreadRole = Qt::UserRole + 7;
constexpr int kPingCategoryRole = Qt::UserRole + 8;
constexpr int kPingIconNameRole = Qt::UserRole + 9;
constexpr int kPingCloudRole = Qt::UserRole + 10;
constexpr int kPingsNextLogoPx = 14;
constexpr int kPingsNextOriginPx = 12;
constexpr int kPingsNextBadgesWidth = kPingsNextLogoPx + 4 + kPingsNextOriginPx;
constexpr int kPingsNextRowHeight = 56;
constexpr int kPingsNextHeadingHeight = 30;
constexpr int kPingsNextTableRowHeight = 34;

struct PingIcon {
    QString name;
    QColor color;
};

// The Pings grid combines desktop, action, and relay inbox events. Give each
// row the same visual shorthand its source uses instead of making the Type
// column a wall of identical text. Keep a safe bell fallback for future
// notification kinds, but cover every kind the relay currently emits here.
PingIcon pingIconFor(const QString &kind, const NotificationLink &link,
                     bool warning, bool good)
{
    const QString normalized = kind.trimmed().toLower();
    const bool dark = currentThemeIsDark();
    const QColor errorColor(dark ? QStringLiteral("#f85149")
                                 : QStringLiteral("#cf222e"));
    const QColor successColor(dark ? QStringLiteral("#3fb950")
                                   : QStringLiteral("#1a7f37"));
    const QColor infoColor(dark ? QStringLiteral("#58a6ff")
                                : QStringLiteral("#0969da"));
    const QColor warningColor(dark ? QStringLiteral("#d29922")
                                   : QStringLiteral("#9a6700"));
    const QColor accentColor = Theme::nextOr(
        Theme::Role::FgSecondary,
        QColor(dark ? QStringLiteral("#a371f7")
                    : QStringLiteral("#8250df")));
    const QColor neutralColor(dark ? QStringLiteral("#8b949e")
                                   : QStringLiteral("#656d76"));

    if (good)
        return {QStringLiteral("check-circle"), successColor};
    if (link.kind == QLatin1String("firewall"))
        return {QStringLiteral("shield-check"), warning ? warningColor
                                                        : neutralColor};
    if (normalized == QLatin1String("error") ||
        normalized == QLatin1String("error_group") ||
        normalized == QLatin1String("operational_alert")) {
        return {QStringLiteral("alert"), errorColor};
    }
    if (normalized == QLatin1String("action") ||
        normalized == QLatin1String("organization_task_started") ||
        normalized == QLatin1String("organization_task_activity") ||
        normalized == QLatin1String("organization_task_update")) {
        return {QStringLiteral("workflow"), accentColor};
    }
    if (normalized == kAgentDonePingKind)
        return {QStringLiteral("sparkle"), successColor};
    if (normalized == QLatin1String("mention"))
        return {QStringLiteral("person"), infoColor};
    if (normalized == QLatin1String("subscribed"))
        return {QStringLiteral("broadcast"), infoColor};
    if (normalized == QLatin1String("pull") ||
        normalized == QLatin1String("pull_submitted")) {
        return {QStringLiteral("git-pull-request"), accentColor};
    }
    if (normalized == QLatin1String("repo_activity") ||
        normalized == QLatin1String("code_push")) {
        return {QStringLiteral("git-commit"), infoColor};
    }
    if (normalized == QLatin1String("issue") ||
        normalized == QLatin1String("issue_opened") ||
        normalized == QLatin1String("issue_assigned")) {
        return {QStringLiteral("issue-opened"), warningColor};
    }
    if (normalized == QLatin1String("repo") ||
        normalized == QLatin1String("repo_shared") ||
        normalized == QLatin1String("repository_hosted")) {
        return {QStringLiteral("repo"),
                normalized == QLatin1String("repository_hosted")
                    ? successColor
                    : infoColor};
    }
    if (normalized == QLatin1String("bounty_funded") ||
        normalized == QLatin1String("bounty_paid") ||
        normalized == QLatin1String("credits_refilled") ||
        normalized == QLatin1String("pending_reward") ||
        normalized == QLatin1String("tip_sent")) {
        return {QStringLiteral("credit-card"), accentColor};
    }
    if (normalized == QLatin1String("release") ||
        normalized == QLatin1String("release_published")) {
        return {QStringLiteral("package"), accentColor};
    }
    if (normalized == QLatin1String("host_online"))
        return {QStringLiteral("server"), successColor};
    if (normalized == QLatin1String("host_offline"))
        return {QStringLiteral("server"), warningColor};
    if (normalized == QLatin1String("pending_inbox"))
        return {QStringLiteral("file"), infoColor};
    if (normalized == QLatin1String("mirror") ||
        normalized == QLatin1String("mirror_request")) {
        return {QStringLiteral("sync"), infoColor};
    }
    if (normalized == QLatin1String("org_succession"))
        return {QStringLiteral("organization"), accentColor};
    if (normalized == QLatin1String("calendar_invitation") ||
        normalized == QLatin1String("calendar_reminder") ||
        normalized == QLatin1String("calendar_rsvp")) {
        return {QStringLiteral("calendar"), infoColor};
    }
    if (normalized == QLatin1String("chat") ||
        normalized == QLatin1String("comment") ||
        normalized == QLatin1String("discussion") ||
        normalized == QLatin1String("chat_webhook")) {
        return {QStringLiteral("comment"), accentColor};
    }
    if (normalized == QLatin1String("plugin_activity"))
        return {QStringLiteral("package"), infoColor};
    if (normalized == QLatin1String("timer_finished"))
        return {QStringLiteral("clock"), infoColor};
    if (normalized == QLatin1String("account_email_sent") ||
        normalized == QLatin1String("prompt")) {
        return {QStringLiteral("paper-airplane"), infoColor};
    }
    if (normalized == QLatin1String("desktop")) {
        return {warning ? QStringLiteral("alert")
                        : QStringLiteral("device-desktop"),
                warning ? warningColor : neutralColor};
    }

    // Local pings often arrive with a generic kind but retain a precise
    // destination. Use it before falling back to severity or a bell.
    if (link.kind == QLatin1String("agent"))
        return {QStringLiteral("sparkle"), accentColor};
    if (link.kind == QLatin1String("search"))
        return {QStringLiteral("search"), infoColor};
    if (link.kind == QLatin1String("issue"))
        return {QStringLiteral("issue-opened"), warningColor};
    if (link.kind == QLatin1String("pull"))
        return {QStringLiteral("git-pull-request"), accentColor};
    if (link.kind == QLatin1String("discussion") ||
        link.kind == QLatin1String("chat")) {
        return {QStringLiteral("comment"), accentColor};
    }
    if (link.kind == QLatin1String("release"))
        return {QStringLiteral("package"), accentColor};
    if (link.kind == QLatin1String("repo"))
        return {QStringLiteral("repo"), infoColor};
    if (link.kind == QLatin1String("commit"))
        return {QStringLiteral("git-commit"), infoColor};
    if (link.kind == QLatin1String("cove"))
        return {QStringLiteral("file"), infoColor};
    if (warning)
        return {QStringLiteral("alert"), warningColor};
    return {QStringLiteral("bell"), neutralColor};
}

// A ping body is a whole agent summary or a worker traceback. Every cell holds
// one line of it and no more: simplified() so a cell is never several lines
// tall, and capped so that a column measured from its contents cannot drag the
// grid past the right-hand edge of the window. Title and Detail are wide
// because they divide the leftover width — their cap is a backstop, and the
// delegate elides to whatever the column actually got. The short cap is what
// really sizes Kind/Repository/From/Link, so it is the tightest.
constexpr int kPingTitlePreviewChars = 120;
constexpr int kPingDetailPreviewChars = 240;
constexpr int kPingShortPreviewChars = 40;

QString pingCellPreview(const QString &text, int maxChars)
{
    const QString flat = text.simplified();
    if (flat.size() <= maxChars)
        return flat;
    return flat.left(qMax(1, maxChars - 1)).trimmed() +
           QString::fromUtf8("\xE2\x80\xA6");
}

// Qt never word-wraps a plain-text tooltip, so a one-line traceback would draw
// a tooltip wider than the screen it is explaining.
QString pingCellTooltip(const QString &text)
{
    constexpr int kMaxTooltipChars = 1200;
    constexpr qsizetype kWrapChars = 100;
    QString clipped = text.trimmed();
    if (clipped.size() > kMaxTooltipChars)
        clipped = clipped.left(kMaxTooltipChars).trimmed() +
                  QString::fromUtf8("\xE2\x80\xA6");
    QStringList lines;
    const QStringList paragraphs = clipped.split(QLatin1Char('\n'));
    for (const QString &paragraph : paragraphs) {
        QString rest = paragraph;
        while (rest.size() > kWrapChars) {
            qsizetype cut = rest.lastIndexOf(QLatin1Char(' '), kWrapChars);
            if (cut <= 0)
                cut = kWrapChars; // one unbroken token: split it mid-word
            lines << rest.left(cut).trimmed();
            rest = rest.mid(cut).trimmed();
        }
        lines << rest;
    }
    return lines.join(QLatin1Char('\n'));
}

// One ping, written out for the composer. Only the fields this ping actually
// has are listed, so a bare desktop notice stays two lines long.
QString pingPromptText(const QString &type, const QString &title,
                       const QString &detail, const QString &repo,
                       const QString &actor, const QString &when,
                       const QString &link)
{
    QStringList head;
    if (!type.trimmed().isEmpty())
        head << type.trimmed();
    if (!when.trimmed().isEmpty())
        head << when.trimmed();
    QStringList lines;
    lines << (head.isEmpty()
                  ? QStringLiteral("ForkMesh ping")
                  : QStringLiteral("ForkMesh ping (%1)")
                        .arg(head.join(QString::fromUtf8(" \xC2\xB7 "))));
    QString headline = title.trimmed();
    if (headline.startsWith(QChar(0x25CF))) // the unread dot is a grid marker
        headline = headline.mid(1).trimmed();
    if (!headline.isEmpty())
        lines << headline;
    if (!detail.trimmed().isEmpty())
        lines << QString() << detail.trimmed();
    QStringList context;
    if (!repo.trimmed().isEmpty())
        context << QStringLiteral("Repository: %1").arg(repo.trimmed());
    if (!actor.trimmed().isEmpty())
        context << QStringLiteral("From: %1").arg(actor.trimmed());
    if (!link.trimmed().isEmpty())
        context << QStringLiteral("Link: %1").arg(link.trimmed());
    if (!context.isEmpty())
        lines << QString() << context;
    return lines.join(QLatin1Char('\n'));
}

class TimestampItem : public QTableWidgetItem
{
public:
    explicit TimestampItem(const QString &text, qint64 ms)
        : QTableWidgetItem(text)
    {
        setData(Qt::UserRole, static_cast<qlonglong>(ms));
    }
    bool operator<(const QTableWidgetItem &other) const override
    {
        return data(Qt::UserRole).toLongLong() <
               other.data(Qt::UserRole).toLongLong();
    }
};

NotificationLink webAlertLink(const QJsonObject &alert)
{
    const QString repo =
        alert.value(QStringLiteral("repo")).toString().trimmed();
    const int slash = repo.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash + 1 >= repo.size())
        return {};
    const QJsonObject meta = alert.value(QStringLiteral("meta")).toObject();
    const int number = meta.value(QStringLiteral("number")).toInt();
    QString source = meta.value(QStringLiteral("source")).toString().trimmed();
    if (source.isEmpty())
        source = alert.value(QStringLiteral("source")).toString().trimmed();
    const QString kind =
        alert.value(QStringLiteral("kind")).toString().trimmed();
    NotificationLink link;
    link.owner = repo.left(slash);
    link.name = repo.mid(slash + 1);
    link.number = number;
    if (number > 0 &&
        (source == QLatin1String("issue") ||
         source == QLatin1String("issue_assigned") ||
         source == QLatin1String("bounty"))) {
        link.kind = QStringLiteral("issue");
    } else if (number > 0 && source == QLatin1String("pull")) {
        link.kind = QStringLiteral("pull");
    } else if (number > 0 && source == QLatin1String("discussion")) {
        link.kind = QStringLiteral("discussion");
    } else if (source == QLatin1String("release") ||
               kind == QLatin1String("release_published")) {
        link.kind = QStringLiteral("release");
    } else if (source == QLatin1String("host") ||
               source == QLatin1String("repository_import") ||
               kind == QLatin1String("repo_shared") ||
               kind == QLatin1String("mirror_request") ||
               kind == QLatin1String("pending_inbox") ||
               kind == QLatin1String("issue_opened") ||
               kind == QLatin1String("pull_submitted") ||
               kind == QLatin1String("repo_activity") ||
               kind == QLatin1String("code_push")) {
        link.kind = QStringLiteral("repo");
    }
    return link; // invalid (no kind) when nothing above matched
}

QString webPingKindLabel(const QString &kind)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("mention"), QStringLiteral("Mention")},
        {QStringLiteral("subscribed"), QStringLiteral("Thread reply")},
        {QStringLiteral("issue_opened"), QStringLiteral("New issue")},
        {QStringLiteral("pull_submitted"), QStringLiteral("Pull request")},
        {QStringLiteral("issue_assigned"), QStringLiteral("Issue assigned")},
        {QStringLiteral("repo_shared"), QStringLiteral("Repo shared")},
        {QStringLiteral("security_advisory"), QStringLiteral("Security advisory")},
        {QStringLiteral("bounty_funded"), QStringLiteral("Bounty funded")},
        {QStringLiteral("bounty_paid"), QStringLiteral("Bounty paid")},
        {QStringLiteral("release_published"), QStringLiteral("Release")},
        {QStringLiteral("host_online"), QStringLiteral("Host online")},
        {QStringLiteral("host_offline"), QStringLiteral("Host offline")},
        {QStringLiteral("credits_refilled"), QStringLiteral("Credits")},
        {QStringLiteral("pending_inbox"), QStringLiteral("Inbox item")},
        {QStringLiteral("repo_activity"), QStringLiteral("Repo activity")},
        {QStringLiteral("code_push"), QStringLiteral("Code push")},
        {QStringLiteral("mirror_request"), QStringLiteral("Mirror request")},
        {QStringLiteral("pending_reward"), QStringLiteral("Reward")},
        {QStringLiteral("org_succession"), QStringLiteral("Org succession")},
        {QStringLiteral("repository_hosted"), QStringLiteral("Repo hosted")},
        {QStringLiteral("organization_task_started"), QStringLiteral("Org task")},
        {QStringLiteral("organization_task_activity"), QStringLiteral("Org task")},
        {QStringLiteral("organization_task_update"), QStringLiteral("Task")},
        {QStringLiteral("calendar_invitation"),
         QStringLiteral("Calendar invitation")},
        {QStringLiteral("calendar_reminder"),
         QStringLiteral("Calendar reminder")},
        {QStringLiteral("calendar_rsvp"), QStringLiteral("Calendar RSVP")},
        {QStringLiteral("chat_webhook"), QStringLiteral("Chat webhook")},
        {QStringLiteral("account_email_sent"), QStringLiteral("Account email")},
        {QStringLiteral("tip_sent"), QStringLiteral("Tip sent")},
        {QStringLiteral("error_group"), QStringLiteral("Error group")},
        {QStringLiteral("operational_alert"), QStringLiteral("System alert")},
        {QStringLiteral("plugin_activity"), QStringLiteral("Plugin")},
        {QStringLiteral("timer_finished"), QStringLiteral("Timer")},
    };
    return labels.value(kind, QStringLiteral("Web"));
}

bool webPingIsRecovery(const QJsonObject &alert)
{
    const QString state = alert.value(QStringLiteral("meta"))
                              .toObject()
                              .value(QStringLiteral("state"))
                              .toString()
                              .trimmed()
                              .toLower();
    if (state == QLatin1String("up"))
        return true;
    if (state == QLatin1String("down"))
        return false;
    const QString title = alert.value(QStringLiteral("title")).toString();
    return title.contains(QLatin1String("recovered"), Qt::CaseInsensitive) ||
           title.contains(QLatin1String("back online"), Qt::CaseInsensitive);
}

QString pingOpenLabel(const NotificationLink &link, int runId,
                      const QString &href)
{
    if (runId > 0 || link.kind == QLatin1String("action"))
        return QStringLiteral("Open action run");
    static const QHash<QString, QString> labels = {
        {QStringLiteral("issue"), QStringLiteral("Open issue")},
        {QStringLiteral("pull"), QStringLiteral("Open pull request")},
        {QStringLiteral("discussion"), QStringLiteral("Open discussion")},
        {QStringLiteral("release"), QStringLiteral("Open release")},
        {QStringLiteral("repo"), QStringLiteral("Open repository")},
        {QStringLiteral("commit"), QStringLiteral("Open commit")},
        {QStringLiteral("chat"), QStringLiteral("Open chat")},
        {QStringLiteral("agent"), QStringLiteral("Open agent")},
        {QStringLiteral("cove"), QStringLiteral("Open cove")},
        {QStringLiteral("firewall"), QStringLiteral("Open firewall")},
        {QStringLiteral("search"), QStringLiteral("Open saved search")},
    };
    if (link.isValid())
        return labels.value(link.kind, QStringLiteral("Open"));
    if (!href.isEmpty())
        return QStringLiteral("Open in browser");
    return {};
}

QString pingTitleWithoutUnreadDot(const QString &title)
{
    QString headline = title.trimmed();
    if (headline.startsWith(QChar(0x25CF)))
        headline = headline.mid(1).trimmed();
    return headline;
}

QString pingNextCategory(const QString &kind, const NotificationLink &link,
                         bool warning)
{
    const QString normalized = kind.trimmed().toLower();
    if (warning || normalized == QLatin1String("error"))
        return QStringLiteral("errors");
    if (normalized == QLatin1String("mention") ||
        normalized == QLatin1String("subscribed") ||
        normalized == QLatin1String("chat") ||
        normalized == QLatin1String("comment") ||
        normalized == QLatin1String("discussion") ||
        normalized == QLatin1String("chat_webhook") ||
        link.kind == QLatin1String("chat") ||
        link.kind == QLatin1String("discussion"))
        return QStringLiteral("mentions");
    if (normalized == QLatin1String("pull") ||
        normalized == QLatin1String("pull_submitted") ||
        link.kind == QLatin1String("pull"))
        return QStringLiteral("reviews");
    if (normalized == kAgentDonePingKind || link.kind == QLatin1String("agent"))
        return QStringLiteral("agents");
    return {};
}

QString pingsNextAge(qint64 ms)
{
    if (ms <= 0)
        return {};
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(ms);
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60)
        return QStringLiteral("Now");
    if (seconds < 3600)
        return QStringLiteral("%1m ago").arg(seconds / 60);
    const QDate day = when.date();
    const QDate today = now.date();
    if (day == today)
        return QStringLiteral("%1h ago").arg(seconds / 3600);
    if (day == today.addDays(-1))
        return QStringLiteral("Yesterday");
    if (day.year() == today.year())
        return QLocale().toString(day, QStringLiteral("MMM d"));
    return QLocale().toString(day, QStringLiteral("MMM d, yyyy"));
}

QString pingsNextGroup(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("EARLIER");
    const QDate day = QDateTime::fromMSecsSinceEpoch(ms).date();
    const QDate today = QDate::currentDate();
    if (day == today)
        return QStringLiteral("TODAY");
    if (day == today.addDays(-1))
        return QStringLiteral("YESTERDAY");
    return QStringLiteral("EARLIER");
}

QString pingsNextWhen(qint64 ms)
{
    if (ms <= 0)
        return {};
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(ms);
    const QDate today = QDate::currentDate();
    const QString time = when.time().toString(QStringLiteral("HH:mm"));
    if (when.date() == today)
        return QStringLiteral("Today, %1").arg(time);
    if (when.date() == today.addDays(-1))
        return QStringLiteral("Yesterday, %1").arg(time);
    if (when.date().year() == today.year())
        return QLocale().toString(when.date(), QStringLiteral("ddd, MMM d")) +
               QStringLiteral(", ") + time;
    return QLocale().toString(when.date(), QStringLiteral("MMM d, yyyy")) +
           QStringLiteral(", ") + time;
}

QString pingsNextInitials(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[\\s._@-]+"));
    QString initials;
    const QStringList parts = text.split(separators, Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        initials += part.at(0).toUpper();
        if (initials.size() == 2)
            break;
    }
    return initials;
}

QString pingsNextPerson(const QString &category, const QString &actor)
{
    if (category == QLatin1String("mentions") ||
        category == QLatin1String("reviews"))
        return pingsNextInitials(actor);
    return {};
}

QPixmap pingsNextAvatar(const QString &iconName, const QString &initials,
                        int side, qreal dpr)
{
    const qreal ratio = dpr > 0.0 ? dpr : 1.0;
    QPixmap pixmap(qMax(1, qRound(side * ratio)), qMax(1, qRound(side * ratio)));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Theme::color(Theme::Role::SurfaceActive));
    painter.drawEllipse(QRectF(0, 0, side, side));
    if (!initials.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(qMax(9, side * 10 / 28));
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(Theme::color(Theme::Role::FgMuted));
        painter.drawText(QRectF(0, 0, side, side), Qt::AlignCenter, initials);
        return pixmap;
    }
    const int glyph = side >= 34 ? 16 : 14;
    const QPixmap icon = kit::tintedIcon(
        iconName.isEmpty() ? QStringLiteral("bell") : iconName,
        Theme::color(Theme::Role::FgMuted), glyph, ratio);
    painter.drawPixmap(QRectF((side - glyph) / 2.0, (side - glyph) / 2.0,
                              glyph, glyph),
                       icon, QRectF(icon.rect()));
    return pixmap;
}

QString pingsNextCellText(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *cell = table ? table->item(row, column) : nullptr;
    if (!cell)
        return {};
    const QVariant full = cell->data(kPingFullTextRole);
    return full.isValid() ? full.toString() : cell->text();
}

class PingsNextRowDelegate final : public QStyledItemDelegate
{
public:
    explicit PingsNextRowDelegate(QTableWidget *table)
        : QStyledItemDelegate(table), m_table(table)
    {
    }

    void setHeadings(QHash<int, QString> headings)
    {
        m_headings = std::move(headings);
    }

    void setProject(const QPixmap &logo, const QString &name)
    {
        m_projectLogo = logo;
        m_projectName = name;
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override
    {
        if (event && event->type() == QEvent::ToolTip &&
            originBadges(option, index.row()).contains(event->pos())) {
            const QTableWidgetItem *type =
                m_table->item(index.row(), NotificationColType);
            const bool cloud = type && type->data(kPingCloudRole).toBool();
            QString text = cloud ? QStringLiteral("Cloud: stored in your "
                                                  "account's ping inbox")
                                 : QStringLiteral("Local: raised on this "
                                                  "desktop");
            if (!m_projectName.isEmpty())
                text += QStringLiteral("\nProject: ") + m_projectName;
            QToolTip::showText(event->globalPos(), text, view);
            return true;
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const int row = index.row();
        const QTableWidgetItem *type = m_table->item(row, NotificationColType);
        const bool unread = type && type->data(kPingUnreadRole).toBool();
        const QString category =
            type ? type->data(kPingCategoryRole).toString() : QString();
        const QString iconName =
            type ? type->data(kPingIconNameRole).toString() : QString();
        const QString title = pingTitleWithoutUnreadDot(
            pingsNextCellText(m_table, row, NotificationColTitle));
        const QString actor =
            pingsNextCellText(m_table, row, NotificationColFrom).trimmed();
        const QString repo =
            pingsNextCellText(m_table, row, NotificationColRepository).trimmed();
        const QString typeText =
            pingsNextCellText(m_table, row, NotificationColType).trimmed();
        const QTableWidgetItem *whenCell = m_table->item(row, NotificationColWhen);
        const qint64 whenMs =
            whenCell ? whenCell->data(Qt::UserRole).toLongLong() : 0;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QRect rect = option.rect;
        const QString heading = m_headings.value(row);
        if (!heading.isEmpty()) {
            const QRect band(rect.left(), rect.top(), rect.width(),
                             kPingsNextHeadingHeight);
            painter->fillRect(band, Theme::color(Theme::Role::Bg));
            QFont headingFont = option.font;
            headingFont.setPixelSize(11);
            headingFont.setWeight(QFont::Medium);
            headingFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            painter->setFont(headingFont);
            painter->setPen(Theme::color(Theme::Role::FgSubtle));
            painter->drawText(band.adjusted(16, 0, -16, -5),
                              Qt::AlignLeft | Qt::AlignBottom, heading);
            rect.setTop(band.bottom() + 1);
        }
        if (option.state & QStyle::State_Selected)
            painter->fillRect(rect, Theme::color(Theme::Role::SurfaceActive));
        else if (option.state & QStyle::State_MouseOver)
            painter->fillRect(rect, Theme::color(Theme::Role::SurfaceRaised));

        const int mid = rect.center().y();
        int x = rect.left() + 16;
        if (unread) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::Role::Accent));
            painter->drawEllipse(QRectF(x + 1, mid - 3, 6, 6));
        }
        x += 20;
        const qreal dpr = painter->device()
                              ? painter->device()->devicePixelRatioF()
                              : 1.0;
        painter->drawPixmap(QRect(x, mid - 14, 28, 28),
                            avatar(iconName, pingsNextPerson(category, actor),
                                   dpr));
        x += 40;

        QFont titleFont = option.font;
        titleFont.setPixelSize(13);
        titleFont.setWeight(unread ? QFont::Medium : QFont::Normal);
        QFont smallFont = option.font;
        smallFont.setPixelSize(12);
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics smallMetrics(smallFont);
        const QString age = pingsNextAge(whenMs);
        const int ageWidth = age.isEmpty() ? 0 : smallMetrics.horizontalAdvance(age);
        const int right = rect.right() - 16;
        const int textWidth = qMax(0, right - x - (ageWidth > 0 ? ageWidth + 12 : 0));
        const QRect badges = originBadges(option, row);
        const int sourceWidth =
            qMax(0, qMin(textWidth, badges.left() - 8 - x));
        const int block = titleMetrics.height() + smallMetrics.height() + 2;
        const int top = mid - block / 2;
        const QRect titleRect(x, top, textWidth, titleMetrics.height());
        painter->setFont(titleFont);
        painter->setPen(Theme::color(unread ? Theme::Role::Fg
                                            : Theme::Role::FgMuted));
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          titleMetrics.elidedText(
                              title.isEmpty() ? QStringLiteral("Ping") : title,
                              Qt::ElideRight, textWidth));
        QString source = actor.isEmpty() ? typeText : actor;
        if (!repo.isEmpty() && repo != source)
            source += (source.isEmpty() ? QString()
                                        : QString::fromUtf8(" \xC2\xB7 ")) +
                      repo;
        painter->setFont(smallFont);
        painter->setPen(Theme::color(Theme::Role::FgSubtle));
        painter->drawText(QRect(x, titleRect.bottom() + 3, sourceWidth,
                                smallMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          smallMetrics.elidedText(source, Qt::ElideRight,
                                                  sourceWidth));
        if (!m_projectLogo.isNull())
            painter->drawPixmap(QRect(badges.left(), badges.top(),
                                      kPingsNextLogoPx, kPingsNextLogoPx),
                                m_projectLogo);
        const bool cloud = type && type->data(kPingCloudRole).toBool();
        themedOcticon(cloud ? QStringLiteral("cloud")
                            : QStringLiteral("device-desktop"),
                      Theme::color(Theme::Role::FgSubtle), kPingsNextOriginPx)
            .paint(painter,
                   QRect(badges.right() - kPingsNextOriginPx + 1,
                         badges.top() +
                             (kPingsNextLogoPx - kPingsNextOriginPx) / 2,
                         kPingsNextOriginPx, kPingsNextOriginPx));
        if (ageWidth > 0)
            painter->drawText(QRect(right - ageWidth, titleRect.top(), ageWidth,
                                    titleMetrics.height()),
                              Qt::AlignRight | Qt::AlignVCenter, age);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return {qMax(240, option.rect.width()), kPingsNextRowHeight};
    }

private:
    QRect originBadges(const QStyleOptionViewItem &option, int row) const
    {
        QRect rect = option.rect;
        if (!m_headings.value(row).isEmpty())
            rect.setTop(rect.top() + kPingsNextHeadingHeight);
        QFont titleFont = option.font;
        titleFont.setPixelSize(13);
        QFont smallFont = option.font;
        smallFont.setPixelSize(12);
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics smallMetrics(smallFont);
        const int block = titleMetrics.height() + smallMetrics.height() + 2;
        const int line = rect.center().y() - block / 2 + titleMetrics.height() + 2;
        return QRect(rect.right() - 16 - kPingsNextBadgesWidth + 1,
                     line + (smallMetrics.height() - kPingsNextLogoPx) / 2,
                     kPingsNextBadgesWidth, kPingsNextLogoPx);
    }

    QPixmap avatar(const QString &iconName, const QString &initials,
                   qreal dpr) const
    {
        const QString key = iconName + QChar(0x1f) + initials + QChar(0x1f) +
                            QString::number(Theme::activeDark()) +
                            QChar(0x1f) + QString::number(dpr);
        auto found = m_avatars.constFind(key);
        if (found != m_avatars.constEnd())
            return found.value();
        if (m_avatars.size() > 256)
            m_avatars.clear();
        const QPixmap pixmap = pingsNextAvatar(iconName, initials, 28, dpr);
        m_avatars.insert(key, pixmap);
        return pixmap;
    }

    QTableWidget *m_table = nullptr;
    QHash<int, QString> m_headings;
    QPixmap m_projectLogo;
    QString m_projectName;
    mutable QHash<QString, QPixmap> m_avatars;
};

class PingsNextEventHook final : public QObject
{
public:
    PingsNextEventHook(QObject *parent,
                       std::function<void(QObject *, QEvent::Type)> handler)
        : QObject(parent), m_handler(std::move(handler))
    {
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (m_handler && (type == QEvent::Show || type == QEvent::Hide))
            m_handler(watched, type);
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void(QObject *, QEvent::Type)> m_handler;
};

void pingsNextRepolish(QWidget *widget)
{
    if (!widget)
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    for (QLabel *label : widget->findChildren<QLabel *>()) {
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    widget->update();
}

void pingsNextDropHiddenSelection(QTableWidget *table)
{
    if (!table || !table->selectionModel())
        return;
    QItemSelectionModel *selection = table->selectionModel();
    QSet<int> rows;
    const QModelIndexList selected = selection->selectedIndexes();
    for (const QModelIndex &index : selected)
        rows.insert(index.row());
    for (int row : std::as_const(rows)) {
        if (table->isRowHidden(row))
            selection->select(table->model()->index(row, 0),
                              QItemSelectionModel::Deselect |
                                  QItemSelectionModel::Rows);
    }
}

QString pingsNextSortLabel(const QTableWidget *table)
{
    const QHeaderView *header = table->horizontalHeader();
    const int section = header->sortIndicatorSection();
    const bool ascending = header->sortIndicatorOrder() == Qt::AscendingOrder;
    if (section == NotificationColWhen || section < 0 ||
        section >= table->columnCount())
        return ascending ? QStringLiteral("Oldest first")
                         : QStringLiteral("Newest first");
    const QTableWidgetItem *label = table->horizontalHeaderItem(section);
    return QStringLiteral("By %1").arg(
        label ? label->text().toLower() : QStringLiteral("column"));
}

void pingsNextLayout(QTableWidget *table)
{
    if (!table || !Theme::isNext())
        return;
    auto *delegate = dynamic_cast<PingsNextRowDelegate *>(
        table->property("pingsNextDelegate").value<QObject *>());
    if (!delegate)
        return;
    const bool tableMode = table->property("pingsNextTableMode").toBool();
    const QString filter = table->property("pingsNextFilter").toString();
    QHash<QString, int> unreadBy;
    int unread = 0;
    int localUnread = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *type = table->item(row, NotificationColType);
        const QString category =
            type ? type->data(kPingCategoryRole).toString() : QString();
        if (type && type->data(kPingUnreadRole).toBool()) {
            ++unread;
            if (type->data(Qt::UserRole + 3).toLongLong() > 0)
                ++localUnread;
            if (!category.isEmpty())
                ++unreadBy[category];
        }
        const bool match = filter.isEmpty() || category == filter;
        if (table->isRowHidden(row) == match)
            table->setRowHidden(row, !match);
    }
    pingsNextDropHiddenSelection(table);

    const bool byWhen =
        table->horizontalHeader()->sortIndicatorSection() == NotificationColWhen;
    QHash<int, QString> headings;
    QString previous;
    int shown = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->isRowHidden(row))
            continue;
        ++shown;
        const QTableWidgetItem *when = table->item(row, NotificationColWhen);
        const QString group =
            byWhen && !tableMode ? pingsNextGroup(when ? when->data(Qt::UserRole).toLongLong()
                                         : 0)
                   : QString();
        if (!group.isEmpty() && group != previous)
            headings.insert(row, group);
        previous = group;
        const int height =
            tableMode ? kPingsNextTableRowHeight
                      : kPingsNextRowHeight +
                            (headings.contains(row) ? kPingsNextHeadingHeight : 0);
        if (table->rowHeight(row) != height)
            table->setRowHeight(row, height);
    }
    delegate->setHeadings(headings);
    table->viewport()->update();

    auto *page = qobject_cast<QWidget *>(
        table->property("pingsNextPageObject").value<QObject *>());
    if (!page)
        return;
    const int total = table->rowCount();
    if (auto *summary = page->findChild<QLabel *>(QStringLiteral("pingsNextSummary"))) {
        const QString inbox = QStringLiteral("%1 in your inbox").arg(total);
        summary->setText(
            unread > 0
                ? QStringLiteral("%1 unread").arg(unread) +
                      QString::fromUtf8(" \xC2\xB7 ") + inbox
            : total > 0 ? QStringLiteral("You are all caught up") +
                              QString::fromUtf8(" \xC2\xB7 ") + inbox
                        : QStringLiteral("Nothing in your inbox"));
    }
    if (auto *markRead =
            page->findChild<QPushButton *>(QStringLiteral("pingsNextMarkRead")))
        markRead->setEnabled(localUnread > 0);
    const QList<QPushButton *> filters =
        page->findChildren<QPushButton *>(QStringLiteral("pingsNextFilter"));
    for (QPushButton *button : filters) {
        const QString id = button->property("pingsFilter").toString();
        const int count = id.isEmpty() ? unread : unreadBy.value(id);
        if (auto *pill =
                button->findChild<QLabel *>(QStringLiteral("pingsNextFilterCount"))) {
            pill->setText(QString::number(count));
            pill->setVisible(count > 0);
        }
        const bool active = id == filter;
        if (button->property("active").toBool() != active) {
            button->setProperty("active", active);
            pingsNextRepolish(button);
        }
        if (QLayout *inner = button->layout()) {
            inner->invalidate();
            inner->activate();
            button->setFixedWidth(inner->sizeHint().width());
        }
    }
    if (auto *sort = page->findChild<QPushButton *>(QStringLiteral("pingsNextSort")))
        sort->setText(pingsNextSortLabel(table));
    if (auto *emptyTitle =
            page->findChild<QLabel *>(QStringLiteral("pingsNextEmptyTitle"))) {
        auto *emptyBody =
            page->findChild<QLabel *>(QStringLiteral("pingsNextEmptyBody"));
        auto *showAll =
            page->findChild<QPushButton *>(QStringLiteral("pingsNextShowAll"));
        if (shown == 0) {
            emptyTitle->setText(filter.isEmpty()
                                    ? QStringLiteral("Nothing in your inbox")
                                    : QStringLiteral("No pings of this kind"));
            if (emptyBody)
                emptyBody->setText(
                    filter.isEmpty()
                        ? QStringLiteral("New pings from agents, CI and "
                                         "teammates will show up here.")
                        : QStringLiteral("Pings of this kind will appear "
                                         "here when they arrive."));
        } else {
            emptyTitle->setText(QStringLiteral("Select a ping"));
            if (emptyBody)
                emptyBody->setText(QStringLiteral(
                    "Choose a ping on the left to see the details."));
        }
        if (showAll)
            showAll->setVisible(shown == 0 && !filter.isEmpty());
    }
}

void pingsNextSetTableMode(QTableWidget *table, bool on)
{
    auto *delegate = qobject_cast<QAbstractItemDelegate *>(
        table ? table->property("pingsNextDelegate").value<QObject *>()
              : nullptr);
    if (!delegate || table->property("pingsNextTableMode").toBool() == on)
        return;
    table->setProperty("pingsNextTableMode", on);
    if (on) {
        table->setItemDelegateForColumn(NotificationColTitle, nullptr);
        const QVariantList hidden =
            table->property("pingsNextTableHidden").toList();
        for (int column = 0; column < table->columnCount(); ++column)
            table->setColumnHidden(column, hidden.contains(column));
        table->horizontalHeader()->show();
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    } else {
        QVariantList hidden;
        for (int column = 0; column < table->columnCount(); ++column)
            if (table->isColumnHidden(column))
                hidden << column;
        table->setProperty("pingsNextTableHidden", hidden);
        for (int column = 0; column < table->columnCount(); ++column)
            table->setColumnHidden(column, column != NotificationColTitle);
        table->horizontalHeader()->hide();
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        table->setItemDelegateForColumn(NotificationColTitle, delegate);
    }
    pingsNextLayout(table);
}

void pingsNextRefreshQuiet(QWidget *page)
{
    if (!page)
        return;
    auto *label = page->findChild<QLabel *>(QStringLiteral("pingsNextQuietLabel"));
    auto *toggle = page->findChild<QCheckBox *>(QStringLiteral("pingsNextQuietToggle"));
    auto *icon = page->findChild<QLabel *>(QStringLiteral("pingsNextQuietIcon"));
    const forkmesh::pings::QuietHours quiet = forkmesh::pings::loadQuietHours();
    const qint64 until = forkmesh::pings::quietUntilMs(
        quiet, QDateTime::currentMSecsSinceEpoch());
    const QString range = forkmesh::pings::minuteLabel(quiet.start) +
                          QStringLiteral(" to ") +
                          forkmesh::pings::minuteLabel(quiet.end);
    if (label) {
        label->setText(
            until > 0 ? QStringLiteral("Quiet hours on until %1")
                            .arg(QDateTime::fromMSecsSinceEpoch(until)
                                     .time()
                                     .toString(QStringLiteral("HH:mm")))
                      : QStringLiteral("Quiet hours %1").arg(range));
        label->setToolTip(forkmesh::pings::quietHoursSummary(quiet) +
                          QStringLiteral(". Days and times are in Settings "
                                         "> Pings."));
    }
    if (icon)
        icon->setPixmap(kit::tintedIcon(
            until > 0 ? QStringLiteral("bell") : QStringLiteral("moon"),
            Theme::color(Theme::Role::FgMuted), 14, icon->devicePixelRatioF()));
    if (toggle && toggle->isChecked() != quiet.enabled) {
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(quiet.enabled);
    }
}
} // namespace

QWidget *MainWindow::buildNotificationsSection()
{
    auto *page = new QWidget;

    auto *title = new QLabel(QStringLiteral("Pings"));
    title->setObjectName("settingsTitle");

    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    refreshButton->setObjectName("repoAction");
    refreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(refreshButton, "sync", 16);
    connect(refreshButton, &QPushButton::clicked, this, [this] {
        refreshWebAlerts(true);
        refreshNotificationsTable();
    });
    addRefreshSpin(refreshButton);

    auto *testButton = new QPushButton(QStringLiteral("Test"));
    testButton->setObjectName("repoAction");
    testButton->setCursor(Qt::PointingHandCursor);
    setOcticon(testButton, "bell", 16);
    testButton->setToolTip(
        QStringLiteral("Send a test desktop ping"));
    connect(testButton, &QPushButton::clicked, this, [this] {
        const QString body = QStringLiteral(
            "This is a test ping from ForkMesh — "
            "desktop pings are working.");
        addNotification(QStringLiteral("Test ping"), body, false);
        postNotification(QStringLiteral("Test ping"), body, false,
                         QStringLiteral("emblem-default"));
    });

    m_pingPauseButton = new QPushButton(QStringLiteral("Pause notifications"));
    m_pingPauseButton->setObjectName("repoAction");
    m_pingPauseButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_pingPauseButton, "bell", 16);
    // Rebuilt per click so Resume appears exactly while a pause is running.
    connect(m_pingPauseButton, &QPushButton::clicked, this,
            [this] { popupPingPauseMenu(m_pingPauseButton); });

    auto *deleteButton = new QPushButton(QStringLiteral("Delete"));
    deleteButton->setObjectName("repoAction");
    deleteButton->setCursor(Qt::PointingHandCursor);
    setOcticon(deleteButton, "x", 16);
    deleteButton->setToolTip(
        QStringLiteral("Delete the selected pings (Del)"));
    connect(deleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteSelectedNotifications);

    auto *clearButton = new QPushButton(QStringLiteral("Clear"));
    clearButton->setObjectName("repoAction");
    clearButton->setCursor(Qt::PointingHandCursor);
    setOcticon(clearButton, "trash", 16);
    clearButton->setToolTip(QStringLiteral(
        "Delete all past desktop and website pings"));
    connect(clearButton, &QPushButton::clicked, this, [this] {
        m_notifications.clear();
        clearWebAlerts();
        updateNotificationButton();
        saveNotificationJournal(); // emptied on disk too, not just on screen
        refreshNotificationsTable();
    });

    // Every ping setting lives on Settings > Pings (#347); each row's
    // Settings column opens it at that ping's own switch.
    auto *settingsButton = new QPushButton(QStringLiteral("Settings"));
    settingsButton->setObjectName("repoAction");
    settingsButton->setCursor(Qt::PointingHandCursor);
    setOcticon(settingsButton, "gear", 16);
    settingsButton->setToolTip(QStringLiteral(
        "Ping settings: turn ping types on or off, email, quiet hours"));
    connect(settingsButton, &QPushButton::clicked, this,
            [this] { openPingSettings(); });

    const bool next = Theme::isNext();
    QWidget *nextHeader = nullptr;
    QHBoxLayout *header = nullptr;
    if (!next) {
        header = new QHBoxLayout;
        header->setContentsMargins(16, 12, 16, 4);
        header->addWidget(title);
        header->addStretch();
        header->addWidget(settingsButton);
        header->addWidget(m_pingPauseButton);
        header->addWidget(testButton);
        header->addWidget(refreshButton);
        header->addWidget(deleteButton);
        header->addWidget(clearButton);
    } else {
        page->setProperty("pingsNextPage", true);
        nextHeader = new QWidget(page);
        nextHeader->setObjectName(QStringLiteral("pingsNextHeader"));
        nextHeader->setAttribute(Qt::WA_StyledBackground, true);
        auto *headerColumn = new QVBoxLayout(nextHeader);
        headerColumn->setContentsMargins(24, 12, 24, 12);
        headerColumn->setSpacing(10);
        auto *topRow = new QHBoxLayout;
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(6);
        auto *titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        titleColumn->addWidget(title);
        auto *summary = new QLabel(nextHeader);
        summary->setObjectName(QStringLiteral("pingsNextSummary"));
        titleColumn->addWidget(summary);
        topRow->addLayout(titleColumn);
        topRow->addStretch(1);

        auto *quiet = new QWidget(nextHeader);
        quiet->setObjectName(QStringLiteral("pingsNextQuiet"));
        auto *quietRow = new QHBoxLayout(quiet);
        quietRow->setContentsMargins(8, 0, 4, 0);
        quietRow->setSpacing(8);
        auto *quietIcon = new QLabel(quiet);
        quietIcon->setObjectName(QStringLiteral("pingsNextQuietIcon"));
        auto *quietLabel = new QLabel(quiet);
        quietLabel->setObjectName(QStringLiteral("pingsNextQuietLabel"));
        auto *quietToggle = new kit::Toggle(quiet);
        quietToggle->setObjectName(QStringLiteral("pingsNextQuietToggle"));
        quietToggle->setToolTip(QStringLiteral(
            "Turn quiet hours on or off. Pings still arrive here; no email, "
            "phone banner, desktop alert or sound is raised."));
        quietRow->addWidget(quietIcon);
        quietRow->addWidget(quietLabel);
        quietRow->addWidget(quietToggle);
        topRow->addWidget(quiet);
        connect(quietToggle, &QCheckBox::clicked, this, [this, page](bool on) {
            ensureSectionBuilt(1);
            auto *box = findChild<QCheckBox *>(
                QStringLiteral("pingQuietHoursCheck"));
            if (box) {
                if (box->isChecked() != on)
                    box->setChecked(on);
                refreshPingPauseSurfaces();
            } else {
                openPingSettings();
            }
            pingsNextRefreshQuiet(page);
        });

        topRow->addWidget(m_pingPauseButton);

        auto *markRead = new QPushButton(QStringLiteral("Mark all read"), nextHeader);
        markRead->setObjectName(QStringLiteral("pingsNextMarkRead"));
        markRead->setCursor(Qt::PointingHandCursor);
        markRead->setToolTip(QStringLiteral(
            "Mark this desktop's pings seen. A website ping is marked read "
            "when you open it."));
        setOcticon(markRead, "check", 14);
        connect(markRead, &QPushButton::clicked, this, [this] {
            markLocalPingsSeen();
            updateNotificationButton();
            if (!m_notificationsTable)
                return;
            for (int row = 0; row < m_notificationsTable->rowCount(); ++row) {
                QTableWidgetItem *type =
                    m_notificationsTable->item(row, NotificationColType);
                if (type && type->data(Qt::UserRole + 3).toLongLong() > 0 &&
                    type->data(kPingUnreadRole).toBool())
                    type->setData(kPingUnreadRole, false);
            }
            pingsNextLayout(m_notificationsTable);
        });
        topRow->addWidget(markRead);

        auto *hiddenActions = new QWidget(nextHeader);
        hiddenActions->setObjectName(QStringLiteral("pingsNextHiddenActions"));
        auto *hiddenRow = new QHBoxLayout(hiddenActions);
        hiddenRow->setContentsMargins(0, 0, 0, 0);
        const QList<QPointer<QPushButton>> overflow{
            settingsButton, testButton, refreshButton, deleteButton,
            clearButton};
        for (const QPointer<QPushButton> &button : overflow)
            hiddenRow->addWidget(button);
        hiddenActions->hide();
        topRow->addWidget(hiddenActions);

        auto *more = new QPushButton(nextHeader);
        more->setObjectName(QStringLiteral("pingsNextMore"));
        more->setCursor(Qt::PointingHandCursor);
        more->setToolTip(QStringLiteral(
            "Ping settings, test ping, refresh, delete and clear"));
        setOcticon(more, "kebab-horizontal", 16);
        connect(more, &QPushButton::clicked, this, [this, more, overflow] {
            QMenu menu(this);
            QHash<QAction *, QPointer<QPushButton>> mapped;
            for (const QPointer<QPushButton> &button : overflow) {
                if (!button || button->isHidden())
                    continue;
                if (button == overflow.value(3))
                    menu.addSeparator();
                QAction *action = menu.addAction(button->icon(), button->text());
                action->setEnabled(button->isEnabled());
                action->setToolTip(button->toolTip());
                mapped.insert(action, button);
            }
            menu.addSeparator();
            QAction *tableView = menu.addAction(QStringLiteral("Show as table"));
            tableView->setCheckable(true);
            tableView->setChecked(
                m_notificationsTable &&
                m_notificationsTable->property("pingsNextTableMode").toBool());
            tableView->setToolTip(QStringLiteral(
                "Show every column with its header, where each column's menu "
                "sorts, selects, hides and moves it"));
            QAction *chosen =
                menu.exec(more->mapToGlobal(QPoint(0, more->height() + 4)));
            if (chosen && chosen == tableView) {
                pingsNextSetTableMode(m_notificationsTable, tableView->isChecked());
                return;
            }
            const QPointer<QPushButton> button = mapped.value(chosen);
            if (button)
                button->click();
        });
        topRow->addWidget(more);
        headerColumn->addLayout(topRow);

        auto *filterRow = new QHBoxLayout;
        filterRow->setContentsMargins(0, 0, 0, 0);
        filterRow->setSpacing(8);
        auto *filters = new QFrame(nextHeader);
        filters->setObjectName(QStringLiteral("pingsNextFilters"));
        auto *filtersRow = new QHBoxLayout(filters);
        filtersRow->setContentsMargins(3, 3, 3, 3);
        filtersRow->setSpacing(2);
        auto *filterGroup = new QButtonGroup(filters);
        filterGroup->setExclusive(true);
        const QList<QPair<QString, QString>> filterChoices{
            {QString(), QStringLiteral("All")},
            {QStringLiteral("mentions"), QStringLiteral("Mentions")},
            {QStringLiteral("reviews"), QStringLiteral("Reviews")},
            {QStringLiteral("errors"), QStringLiteral("Errors")},
            {QStringLiteral("agents"), QStringLiteral("Agents")}};
        for (const auto &choice : filterChoices) {
            auto *button = new QPushButton(filters);
            button->setObjectName(QStringLiteral("pingsNextFilter"));
            button->setProperty("pingsFilter", choice.first);
            button->setProperty("active", choice.first.isEmpty());
            button->setCheckable(true);
            button->setChecked(choice.first.isEmpty());
            button->setCursor(Qt::PointingHandCursor);
            button->setFixedHeight(28);
            auto *inner = new QHBoxLayout(button);
            inner->setContentsMargins(10, 0, 10, 0);
            inner->setSpacing(6);
            auto *label = new QLabel(choice.second, button);
            label->setObjectName(QStringLiteral("pingsNextFilterLabel"));
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            auto *count = new QLabel(button);
            count->setObjectName(QStringLiteral("pingsNextFilterCount"));
            count->setAttribute(Qt::WA_TransparentForMouseEvents);
            count->setAlignment(Qt::AlignCenter);
            count->hide();
            inner->addWidget(label);
            inner->addWidget(count);
            filterGroup->addButton(button);
            filtersRow->addWidget(button);
            const QString id = choice.first;
            connect(button, &QPushButton::clicked, this, [this, id] {
                if (!m_notificationsTable)
                    return;
                m_notificationsTable->setProperty("pingsNextFilter", id);
                pingsNextLayout(m_notificationsTable);
            });
        }
        filterRow->addWidget(filters);
        filterRow->addStretch(1);
        auto *sort = new QPushButton(nextHeader);
        sort->setObjectName(QStringLiteral("pingsNextSort"));
        sort->setCursor(Qt::PointingHandCursor);
        sort->setToolTip(QStringLiteral("Choose how the pings are ordered"));
        setOcticon(sort, "sort-desc", 14);
        connect(sort, &QPushButton::clicked, this, [this, sort] {
            if (!m_notificationsTable)
                return;
            QMenu menu(this);
            const QList<QPair<int, Qt::SortOrder>> choices{
                {NotificationColWhen, Qt::DescendingOrder},
                {NotificationColWhen, Qt::AscendingOrder},
                {NotificationColTitle, Qt::AscendingOrder},
                {NotificationColType, Qt::AscendingOrder},
                {NotificationColKind, Qt::AscendingOrder},
                {NotificationColRepository, Qt::AscendingOrder},
                {NotificationColFrom, Qt::AscendingOrder},
                {NotificationColStatus, Qt::AscendingOrder}};
            QHeaderView *pingHeader = m_notificationsTable->horizontalHeader();
            QHash<QAction *, QPair<int, Qt::SortOrder>> mapped;
            for (const auto &choice : choices) {
                QString text;
                if (choice.first == NotificationColWhen)
                    text = choice.second == Qt::DescendingOrder
                               ? QStringLiteral("Newest first")
                               : QStringLiteral("Oldest first");
                else if (QTableWidgetItem *label =
                             m_notificationsTable->horizontalHeaderItem(
                                 choice.first))
                    text = QStringLiteral("By %1").arg(label->text().toLower());
                QAction *action = menu.addAction(text);
                action->setCheckable(true);
                action->setChecked(
                    pingHeader->sortIndicatorSection() == choice.first &&
                    (choice.first != NotificationColWhen ||
                     pingHeader->sortIndicatorOrder() == choice.second));
                mapped.insert(action, choice);
                if (choice.first == NotificationColWhen &&
                    choice.second == Qt::AscendingOrder)
                    menu.addSeparator();
            }
            QAction *chosen =
                menu.exec(sort->mapToGlobal(QPoint(0, sort->height() + 4)));
            if (!chosen || !mapped.contains(chosen))
                return;
            const auto picked = mapped.value(chosen);
            m_notificationsTable->sortByColumn(picked.first, picked.second);
        });
        filterRow->addWidget(sort);
        headerColumn->addLayout(filterRow);
        pingsNextRefreshQuiet(page);
    }

    m_notificationsTable = new QTableWidget(0, kNotificationColumns);
    installColumnHeaderMenu(m_notificationsTable);
    m_notificationsTable->setObjectName("issueTable"); // reuse the table styling
    m_notificationsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Type"), QStringLiteral("Kind"),
         QStringLiteral("Title"), QStringLiteral("Detail"),
         QStringLiteral("Repository"), QStringLiteral("From"),
         QStringLiteral("Status"), QStringLiteral("When"),
         QStringLiteral("Link"), QStringLiteral("Settings"),
         QStringLiteral("Prompt")});
    m_notificationsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_notificationsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_notificationsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_notificationsTable->verticalHeader()->setVisible(false);
    m_notificationsTable->setSortingEnabled(true);
    m_notificationsTable->setAlternatingRowColors(true);
    m_notificationsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    QHeaderView *pingHeader = m_notificationsTable->horizontalHeader();
    pingHeader->setSectionResizeMode(NotificationColTitle, QHeaderView::Stretch);
    pingHeader->setSectionResizeMode(NotificationColDetail,
                                     QHeaderView::Stretch);
    for (int column : {NotificationColType, NotificationColKind,
                       NotificationColRepository, NotificationColFrom,
                       NotificationColStatus, NotificationColWhen,
                       NotificationColLink})
        pingHeader->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    // Content sizing measures items, not cell widgets, so the button column is
    // given its width by hand. Interactive also keeps makeColumnsResizable()
    // from re-measuring it down to a sliver.
    pingHeader->setSectionResizeMode(NotificationColPrompt,
                                     QHeaderView::Interactive);
    m_notificationsTable->setColumnWidth(NotificationColPrompt,
                                         kNotificationPromptWidth);
    pingHeader->setSectionResizeMode(NotificationColSettings,
                                     QHeaderView::Interactive);
    m_notificationsTable->setColumnWidth(NotificationColSettings,
                                         kNotificationSettingsWidth);
    // Title and Detail keep dividing the spare width instead of being frozen at
    // their content width. Both hold free text of unbounded length — a ping body
    // is a whole agent summary — and freezing that is what used to push the grid
    // past the right-hand window edge. Every other column is still draggable.
    makeColumnsResizable(m_notificationsTable,
                         {NotificationColTitle, NotificationColDetail});
    pingHeader->setSortIndicator(NotificationColWhen,
                                 Qt::DescendingOrder); // newest first by default
    m_notificationsTable->setToolTip(
        QStringLiteral("Click a ping to see its details and actions on the "
                       "right. Double-click it, or click its link, to open the "
                       "related issue, pull request, discussion, commit, chat "
                       "or action; click a repository to open it. Del (or "
                       "right-click) removes the selected rows."));
    m_notificationsTable->setMouseTracking(true);
    connect(m_notificationsTable, &QAbstractItemView::entered, this,
            [this](const QModelIndex &index) {
                if (index.isValid())
                    m_notificationsTable->viewport()->setCursor(
                        Qt::PointingHandCursor);
                else
                    m_notificationsTable->viewport()->unsetCursor();
            });
    connect(m_notificationsTable, &QAbstractItemView::viewportEntered, this,
            [this] { m_notificationsTable->viewport()->unsetCursor(); });
    connect(m_notificationsTable, &QTableWidget::itemClicked, this,
            [this](QTableWidgetItem *item) {
                if (!item || (QGuiApplication::keyboardModifiers() &
                              (Qt::ControlModifier | Qt::ShiftModifier)))
                    return;
                const int row = item->row();
                const bool opens = item->data(kPingOpensRole).toBool();
                if (opens && item->column() == NotificationColRepository)
                    openNotificationRepository(row);
                else if (opens && item->column() == NotificationColLink)
                    openNotificationRow(row);
                else
                    showPingDetail(row);
            });
    connect(m_notificationsTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
                if (!item || item->column() == NotificationColRepository ||
                    item->column() == NotificationColLink)
                    return;
                openNotificationRow(item->row());
            });
    connect(m_notificationsTable, &QTableWidget::itemSelectionChanged, this,
            [this] {
                if (!m_pingDetailPane || m_pingDetailPane->isHidden())
                    return;
                const QModelIndexList rows =
                    m_notificationsTable->selectionModel()->selectedRows();
                if (rows.size() != 1)
                    return;
                const int row = rows.first().row();
                if (notificationRowKey(row) != m_pingDetailKey)
                    showPingDetail(row);
            });

    auto *deleteShortcut =
        new QShortcut(QKeySequence::Delete, m_notificationsTable);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteShortcut, &QShortcut::activated, this,
            &MainWindow::deleteSelectedNotifications);
    connect(m_notificationsTable, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                QTableWidgetItem *item = m_notificationsTable->itemAt(pos);
                if (item && !item->isSelected())
                    m_notificationsTable->selectRow(item->row());
                QMenu menu(this);
                if (item) {
                    const QTableWidgetItem *typeCell =
                        m_notificationsTable->item(item->row(), NotificationColType);
                    if (typeCell)
                        addPingFixMenuActions(
                            &menu, typeCell->data(Qt::UserRole + 3).toLongLong());
                }
                QAction *details =
                    item ? menu.addAction(QStringLiteral("Show details"))
                         : nullptr;
                QAction *open = item ? menu.addAction(QStringLiteral("Open"))
                                     : nullptr;
                QAction *copy = item ? menu.addAction(QStringLiteral("Copy row"))
                                     : nullptr;
                if (item)
                    menu.addSeparator();
                QAction *remove = menu.addAction(QStringLiteral("Delete"));
                remove->setEnabled(
                    !m_notificationsTable->selectionModel()->selectedRows()
                         .isEmpty());
                QAction *chosen =
                    menu.exec(m_notificationsTable->viewport()->mapToGlobal(pos));
                if (!chosen)
                    return;
                if (chosen == remove) {
                    deleteSelectedNotifications();
                } else if (chosen == details) {
                    showPingDetail(item->row());
                } else if (chosen == open) {
                    openNotificationRow(item->row());
                } else if (chosen == copy) {
                    QStringList cells;
                    for (int column = 0; column < NotificationColSettings;
                         ++column) {
                        QTableWidgetItem *cell =
                            m_notificationsTable->item(item->row(), column);
                        if (!cell)
                            continue;
                        const QVariant full = cell->data(kPingFullTextRole);
                        cells << (full.isValid() ? full.toString()
                                                 : cell->text());
                    }
                    QGuiApplication::clipboard()->setText(
                        cells.join(QStringLiteral("\t")));
                }
            });

    if (next) {
        m_notificationsTable->setProperty("pingsNext", true);
        m_notificationsTable->setProperty(
            "pingsNextPageObject", QVariant::fromValue<QObject *>(page));
        auto *rowDelegate = new PingsNextRowDelegate(m_notificationsTable);
        m_notificationsTable->setProperty(
            "pingsNextDelegate", QVariant::fromValue<QObject *>(rowDelegate));
        m_notificationsTable->setItemDelegateForColumn(NotificationColTitle,
                                                       rowDelegate);
        m_notificationsTable->horizontalHeader()->hide();
        for (int column = 0; column < kNotificationColumns; ++column)
            m_notificationsTable->setColumnHidden(column,
                                                  column != NotificationColTitle);
        m_notificationsTable->setAlternatingRowColors(false);
        m_notificationsTable->setShowGrid(false);
        m_notificationsTable->setWordWrap(false);
        m_notificationsTable->setFrameShape(QFrame::NoFrame);
        m_notificationsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_notificationsTable->setVerticalScrollMode(
            QAbstractItemView::ScrollPerPixel);
        m_notificationsTable->setMinimumWidth(300);
        m_notificationsTable->viewport()->setAttribute(Qt::WA_Hover, true);
        m_notificationsTable->verticalHeader()->setDefaultSectionSize(
            kPingsNextRowHeight);
        connect(m_notificationsTable->selectionModel(),
                &QItemSelectionModel::selectionChanged, m_notificationsTable,
                [this] { pingsNextDropHiddenSelection(m_notificationsTable); });
        connect(pingHeader, &QHeaderView::sortIndicatorChanged, this, [this] {
            QTimer::singleShot(0, this,
                               [this] { pingsNextLayout(m_notificationsTable); });
        });
    }

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(next ? 0 : 6);
    if (nextHeader)
        layout->addWidget(nextHeader);
    else
        layout->addLayout(header);

    m_pingDetailPane = new QScrollArea;
    m_pingDetailPane->setObjectName(QStringLiteral("issueSidebarScroll"));
    m_pingDetailPane->setFrameShape(QFrame::NoFrame);
    m_pingDetailPane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pingDetailPane->setWidgetResizable(true);
    m_pingDetailPane->setMinimumWidth(280);
    m_pingDetailPane->setMaximumWidth(460);
    m_pingDetailPane->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Ignored);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setObjectName(QStringLiteral("issuesSplitter"));
    splitter->setChildrenCollapsible(true);
    splitter->addWidget(m_notificationsTable);
    splitter->addWidget(m_pingDetailPane);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, true);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({900, 340});
    m_pingDetailPane->hide();
    if (next) {
        m_pingDetailPane->setMinimumWidth(320);
        m_pingDetailPane->setMaximumWidth(QWIDGETSIZE_MAX);
        m_pingDetailPane->setProperty("pingsNext", true);
        auto *placeholder = new QWidget;
        placeholder->setObjectName(QStringLiteral("pingsNextEmptyDetail"));
        placeholder->setAttribute(Qt::WA_StyledBackground, true);
        auto *emptyColumn = new QVBoxLayout(placeholder);
        emptyColumn->setContentsMargins(32, 32, 32, 32);
        emptyColumn->setSpacing(8);
        emptyColumn->addStretch(1);
        auto *emptyIcon = new QLabel(placeholder);
        emptyIcon->setPixmap(pingsNextAvatar(QStringLiteral("comment"), QString(),
                                             40, placeholder->devicePixelRatioF()));
        emptyIcon->setAlignment(Qt::AlignCenter);
        emptyColumn->addWidget(emptyIcon, 0, Qt::AlignHCenter);
        auto *emptyTitle = new QLabel(QStringLiteral("Select a ping"), placeholder);
        emptyTitle->setObjectName(QStringLiteral("pingsNextEmptyTitle"));
        emptyTitle->setAlignment(Qt::AlignCenter);
        emptyColumn->addWidget(emptyTitle);
        auto *emptyBody = new QLabel(
            QStringLiteral("Choose a ping on the left to see the details."),
            placeholder);
        emptyBody->setObjectName(QStringLiteral("pingsNextEmptyBody"));
        emptyBody->setAlignment(Qt::AlignCenter);
        emptyBody->setWordWrap(true);
        emptyColumn->addWidget(emptyBody);
        auto *showAll = new QPushButton(QStringLiteral("Show all pings"), placeholder);
        showAll->setObjectName(QStringLiteral("pingsNextShowAll"));
        showAll->setCursor(Qt::PointingHandCursor);
        showAll->hide();
        connect(showAll, &QPushButton::clicked, this, [this, page] {
            const QList<QPushButton *> filterButtons =
                page->findChildren<QPushButton *>(
                    QStringLiteral("pingsNextFilter"));
            for (QPushButton *button : filterButtons)
                if (button->property("pingsFilter").toString().isEmpty())
                    button->click();
        });
        emptyColumn->addWidget(showAll, 0, Qt::AlignHCenter);
        emptyColumn->addStretch(2);
        splitter->addWidget(placeholder);
        splitter->setCollapsible(2, false);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setStretchFactor(2, 1);
        splitter->setHandleWidth(1);
        splitter->setSizes({420, 900, 900});
        auto *hook = new PingsNextEventHook(
            page, [this, page, placeholder](QObject *watched, QEvent::Type type) {
                if (watched == m_pingDetailPane) {
                    placeholder->setVisible(m_pingDetailPane->isHidden());
                } else if (watched == page && type == QEvent::Show) {
                    pingsNextRefreshQuiet(page);
                    pingsNextLayout(m_notificationsTable);
                }
            });
        m_pingDetailPane->installEventFilter(hook);
        page->installEventFilter(hook);
    }
    layout->addWidget(splitter, 1);

    auto *closeDetail = new QShortcut(QKeySequence(Qt::Key_Escape), page);
    closeDetail->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeDetail, &QShortcut::activated, this,
            [this] { hidePingDetail(); });

    auto *pager = new QHBoxLayout;
    pager->setContentsMargins(16, 0, 16, 8);
    pager->setSpacing(8);
    m_webAlertsPrevPageButton =
        new QPushButton(QString::fromUtf8("\xE2\x80\xB9 Previous"));
    m_webAlertsPrevPageButton->setObjectName(QStringLiteral("ghostButton"));
    m_webAlertsNextPageButton =
        new QPushButton(QString::fromUtf8("Next \xE2\x80\xBA"));
    m_webAlertsNextPageButton->setObjectName(QStringLiteral("ghostButton"));
    m_webAlertsPageLabel = new QLabel;
    m_webAlertsPageLabel->setObjectName(QStringLiteral("mutedLabel"));
    connect(m_webAlertsPrevPageButton, &QPushButton::clicked, this,
            [this] { setWebAlertsPage(m_webAlertsPage - 1); });
    connect(m_webAlertsNextPageButton, &QPushButton::clicked, this,
            [this] { setWebAlertsPage(m_webAlertsPage + 1); });
    pager->addWidget(m_webAlertsPrevPageButton);
    pager->addWidget(m_webAlertsPageLabel);
    pager->addStretch();
    pager->addWidget(m_webAlertsNextPageButton);
    layout->addLayout(pager);

    refreshNotificationsTable();
    updateWebAlertPager();
    // A pause set before the last quit is still running: label the button from
    // the stored deadline rather than the "Pause" it was constructed with.
    refreshPingPauseSurfaces();
    return page;
}

void MainWindow::refreshNotificationsTable()
{
    if (!m_notificationsTable)
        return;
    TableRepaintGuard repaintGuard(m_notificationsTable);
    m_notificationsTable->setSortingEnabled(false);
    m_notificationsTable->setRowCount(0);
    if (auto *rowDelegate = dynamic_cast<PingsNextRowDelegate *>(
            m_notificationsTable->property("pingsNextDelegate")
                .value<QObject *>())) {
        QString projectName;
        const QPixmap logo = currentProjectLogo(kPingsNextLogoPx, &projectName);
        rowDelegate->setProject(logo, projectName);
    }
    int approvalRows = 0;
    for (const ActionRun &run : std::as_const(m_actionRuns))
        if (run.status == ActionStatus::AwaitingApproval)
            ++approvalRows;
    m_notificationsTable->setRowCount(
        approvalRows + int(m_notifications.size()) + int(m_webAlerts.size()));

    struct NotificationRow {
        QString type;
        QString kind;
        QString title;
        QString detail;
        QString repo;
        QString actor;
        QString status;
        QString statusTooltip;
        qint64 whenMs = 0;
        QString link;
        int runId = -1;
        bool warning = false;
        bool good = false;
        NotificationLink destination;
        QString href;
        qint64 localId = 0;
        QString webId;
        bool unread = false;
        bool cloud = false;
    };
    // Row heights live on the vertical header, not on the items, so they stay
    // put when a sort moves the rows. One height for the whole grid is the only
    // way a reordered page keeps its prompt buttons whole.
    int promptRowHeight = 0;
    int nextRow = 0;
    auto addRow = [this, &promptRowHeight,
                   &nextRow](const NotificationRow &data) {
        const int row = nextRow++;

        auto *typeItem = new QTableWidgetItem(data.type);
        const PingIcon pingIcon =
            pingIconFor(data.kind, data.destination, data.warning, data.good);
        typeItem->setIcon(themedOcticon(pingIcon.name, pingIcon.color, 14));
        typeItem->setData(Qt::UserRole, data.runId);
        if (data.destination.isValid())
            typeItem->setData(Qt::UserRole + 1,
                              QVariant::fromValue(data.destination));
        if (!data.href.isEmpty())
            typeItem->setData(Qt::UserRole + 2, data.href);
        if (data.localId > 0)
            typeItem->setData(Qt::UserRole + 3,
                              static_cast<qlonglong>(data.localId));
        if (!data.webId.isEmpty())
            typeItem->setData(Qt::UserRole + 4, data.webId);
        typeItem->setData(kPingUnreadRole, data.unread);
        typeItem->setData(kPingCategoryRole,
                          pingNextCategory(data.kind, data.destination,
                                           data.warning));
        typeItem->setData(kPingIconNameRole, pingIcon.name);
        typeItem->setData(kPingCloudRole, data.cloud);

        auto *whenItem = new TimestampItem(
            data.whenMs > 0 ? formatRepoDate(data.whenMs) : QString(),
            data.whenMs);
        // Every cell shows a capped single line and keeps the text it was cut
        // from, so a ping that runs to a whole agent summary cannot decide how
        // wide its column is.
        auto shortCell = [](const QString &text) {
            return new QTableWidgetItem(
                pingCellPreview(text, kPingShortPreviewChars));
        };
        struct Cell {
            QTableWidgetItem *item;
            QString full;
        };
        const QList<Cell> cells{
            {typeItem, data.type},
            {shortCell(data.kind), data.kind},
            {new QTableWidgetItem(
                 pingCellPreview(data.title, kPingTitlePreviewChars)),
             data.title},
            {new QTableWidgetItem(
                 pingCellPreview(data.detail, kPingDetailPreviewChars)),
             data.detail},
            {shortCell(data.repo), data.repo},
            {shortCell(data.actor), data.actor},
            {new QTableWidgetItem(data.status), data.status},
            {whenItem, whenItem->text()},
            {shortCell(data.link), data.link}};
        const QColor red("#f85149");
        const QColor green("#3fb950");
        for (int column = 0; column < cells.size(); ++column) {
            QTableWidgetItem *cell = cells.at(column).item;
            const QString &full = cells.at(column).full;
            if (!full.isEmpty()) {
                cell->setData(kPingFullTextRole, full);
                cell->setToolTip(pingCellTooltip(full));
            }
            if (column == NotificationColStatus) {
                cell->setIcon(themedOcticon(
                    data.cloud ? QStringLiteral("cloud")
                               : QStringLiteral("device-desktop"),
                    Theme::color(Theme::Role::FgSubtle), 12));
                if (!data.statusTooltip.isEmpty())
                    cell->setToolTip(data.statusTooltip);
            }
            if (data.warning)
                cell->setForeground(red);
            else if (data.good)
                cell->setForeground(green);
            m_notificationsTable->setItem(row, column, cell);
        }

        const bool opensDestination = data.runId > 0 ||
                                      data.destination.isValid() ||
                                      !data.href.isEmpty();
        auto markOpens = [](QTableWidgetItem *cell, bool underline) {
            cell->setData(kPingOpensRole, true);
            if (!underline)
                return;
            QFont font = cell->font();
            font.setUnderline(true);
            cell->setFont(font);
        };
        if (opensDestination && !data.link.isEmpty())
            markOpens(cells.at(NotificationColLink).item, true);
        const qsizetype repoSlash = data.repo.indexOf(QLatin1Char('/'));
        if (repoSlash > 0 &&
            repoIndexFor(data.repo.left(repoSlash),
                         data.repo.mid(repoSlash + 1)) >= 0)
            markOpens(cells.at(NotificationColRepository).item, false);

        // The settings column: this ping's own switch on Settings > Pings.
        // Captured by value for the same reason as the prompt button below.
        const QString kind = data.kind;
        const bool relayKind = forkmesh::pings::kindInfo(kind) != nullptr;
        auto *openSettings = new QPushButton;
        openSettings->setObjectName(QStringLiteral("ghostButton"));
        openSettings->setProperty("buttonSize", "xs");
        openSettings->setProperty("pingKind", kind);
        openSettings->setCursor(Qt::PointingHandCursor);
        openSettings->setFocusPolicy(Qt::NoFocus);
        setOcticon(openSettings, QStringLiteral("gear"), 12);
        openSettings->setToolTip(
            relayKind
                ? QStringLiteral("Ping settings for \"%1\" — turn this type "
                                 "off or choose whether it emails")
                      .arg(data.type)
                : QStringLiteral("Ping settings for this desktop — turn "
                                 "this type off here"));
        connect(openSettings, &QPushButton::clicked, this,
                [this, kind, openSettings] {
                    showPingKindMenu(openSettings, kind);
                });
        m_notificationsTable->setItem(row, NotificationColSettings,
                                      new QTableWidgetItem);
        m_notificationsTable->setCellWidget(row, NotificationColSettings,
                                            openSettings);

        // Hand this one ping to the footer composer. The text is captured by
        // value: a sort moves the button with its row, so looking the row up
        // again on the click would read whichever ping had landed there.
        const QString prompt = pingPromptText(
            data.type, data.title, data.detail, data.repo, data.actor,
            whenItem->text(), data.href.isEmpty() ? data.link : data.href);
        auto *sendToPrompt = new QPushButton;
        sendToPrompt->setObjectName(QStringLiteral("pingPromptAction"));
        sendToPrompt->setProperty("buttonSize", "xs");
        sendToPrompt->setCursor(Qt::PointingHandCursor);
        sendToPrompt->setFocusPolicy(Qt::NoFocus);
        setOcticon(sendToPrompt, QStringLiteral("paper-airplane"), 12);
        sendToPrompt->setToolTip(
            QStringLiteral("Add this ping to the prompt composer"));
        connect(sendToPrompt, &QPushButton::clicked, this,
                [this, prompt] { sendPingToPrompt(prompt); });
        // An item behind the widget keeps the column sortable and selectable:
        // without one the model has nothing to compare and drops these rows to
        // the bottom of every order. The button covers the cell.
        m_notificationsTable->setItem(row, NotificationColPrompt,
                                      new QTableWidgetItem);
        m_notificationsTable->setCellWidget(row, NotificationColPrompt,
                                            sendToPrompt);
        // Measured after the cell adopts it: an unparented button has not been
        // polished yet, so its size hint does not know the stylesheet.
        promptRowHeight = qMax(promptRowHeight,
                               sendToPrompt->sizeHint().height() + 4);
    };

    for (const ActionRun &run : std::as_const(m_actionRuns)) {
        if (run.status != ActionStatus::AwaitingApproval)
            continue;
        NotificationRow data;
        data.type = QStringLiteral("Approval");
        data.kind = QStringLiteral("action");
        data.title = run.workflowName;
        data.detail = QStringLiteral("%1/%2 at %3")
                          .arg(run.owner, run.name, run.commit.left(8));
        data.repo = run.owner + QLatin1Char('/') + run.name;
        data.status = QStringLiteral("Awaiting approval");
        data.whenMs = run.createdAtMs;
        data.link = QStringLiteral("run #%1").arg(run.id);
        data.runId = run.id;
        data.unread = true;
        addRow(data);
    }
    for (const AppNotification &notice : std::as_const(m_notifications)) {
        NotificationRow data;
        data.type = notice.warning ? QStringLiteral("Alert")
                                   : QStringLiteral("Info");
        data.kind = notice.kind;
        data.title = notice.title;
        data.detail = notice.body;
        data.repo = notice.repo;
        data.actor = notice.actor;
        data.status = forkmesh::pingSyncLabel(notice.sync);
        data.statusTooltip =
            notice.syncReason.isEmpty()
                ? forkmesh::pingSyncDescription(notice.sync)
                : forkmesh::pingSyncDescription(notice.sync) +
                      QString::fromUtf8("\n\xE2\x80\x94 ") + notice.syncReason;
        data.whenMs = notice.timestampMs;
        data.link = notificationLinkLabel(notice.link);
        data.runId = notice.runId;
        data.warning = notice.warning;
        data.destination = notice.link;
        data.localId = notice.id;
        data.unread = (m_localPingsSeenAtMs >= 0 &&
                       notice.timestampMs > m_localPingsSeenAtMs &&
                       notice.sync != forkmesh::PingSync::Synced) ||
                      (notice.dismissOnly && !notice.acknowledged);
        addRow(data);
    }
    for (const QJsonValue &value : std::as_const(m_webAlerts)) {
        const QJsonObject alert = value.toObject();
        const bool unread =
            alert.value(QStringLiteral("readAt")).toDouble() <= 0;
        const QString kind =
            alert.value(QStringLiteral("kind")).toString().trimmed();
        const QString title =
            alert.value(QStringLiteral("title")).toString().trimmed();
        const QString body =
            alert.value(QStringLiteral("body")).toString().trimmed();
        const QString repo =
            alert.value(QStringLiteral("repo")).toString().trimmed();
        const QString href =
            alert.value(QStringLiteral("href")).toString().trimmed();
        const QString webUrl =
            href.startsWith(QLatin1Char('/'))
                ? catalogApiUrl().resolved(QUrl(href)).toString()
                : QString();
        const bool recovery = kind == QLatin1String("operational_alert") &&
                              webPingIsRecovery(alert);
        NotificationRow data;
        data.type = recovery ? QStringLiteral("System recovered")
                             : webPingKindLabel(kind);
        data.kind = kind;
        data.title = (unread ? QString::fromUtf8("\xE2\x97\x8F ") : QString()) +
                     (title.isEmpty() ? QStringLiteral("Website ping") : title);
        data.detail = body;
        data.repo = repo;
        data.actor = alert.value(QStringLiteral("actor")).toString().trimmed();
        data.status = unread ? QStringLiteral("Unread")
                             : QStringLiteral("Read");
        data.statusTooltip =
            QStringLiteral("Stored in your account's cloud ping inbox.");
        data.whenMs = qint64(alert.value(QStringLiteral("ts")).toDouble());
        data.link = href;
        data.warning = !recovery &&
                       (kind == QLatin1String("error_group") ||
                        kind == QLatin1String("operational_alert"));
        data.good = recovery;
        data.destination = webAlertLink(alert);
        data.href = webUrl;
        data.webId = alert.value(QStringLiteral("id")).toString().trimmed();
        data.unread = unread;
        data.cloud = true;
        addRow(data);
    }
    m_notificationsTable->setRowCount(nextRow);

    if (promptRowHeight > 0) {
        int uniform = promptRowHeight;
        for (int row = 0; row < m_notificationsTable->rowCount(); ++row)
            uniform = qMax(uniform, m_notificationsTable->rowHeight(row));
        for (int row = 0; row < m_notificationsTable->rowCount(); ++row)
            m_notificationsTable->setRowHeight(row, uniform);
    }
    m_notificationsTable->setSortingEnabled(true);
    pingsNextLayout(m_notificationsTable);
    refreshPingDetail();
}

void MainWindow::updateWebAlertPager()
{
    const int pageCount =
        qMax(1, (m_webAlertsTotal + kWebAlertsPageSize - 1) /
                    kWebAlertsPageSize);
    if (m_webAlertsPageLabel) {
        if (m_webAlertsLoading) {
            m_webAlertsPageLabel->setText(
                QString::fromUtf8("Loading website pings\xE2\x80\xA6"));
        } else if (m_webAlertsTotal <= 0) {
            m_webAlertsPageLabel->setText(QStringLiteral("No website pings"));
        } else {
            const int first = m_webAlertsPage * kWebAlertsPageSize + 1;
            const int last = qMin(m_webAlertsTotal,
                                  first + m_webAlerts.size() - 1);
            m_webAlertsPageLabel->setText(
                QString::fromUtf8("Website pings %1\xE2\x80\x93%2 of %3 "
                               "\xC2\xB7 page %4 of %5")
                    .arg(first)
                    .arg(last)
                    .arg(m_webAlertsTotal)
                    .arg(m_webAlertsPage + 1)
                    .arg(pageCount));
        }
    }
    if (m_webAlertsPrevPageButton)
        m_webAlertsPrevPageButton->setEnabled(!m_webAlertsLoading &&
                                              m_webAlertsPage > 0);
    if (m_webAlertsNextPageButton)
        m_webAlertsNextPageButton->setEnabled(
            !m_webAlertsLoading && m_webAlertsPage + 1 < pageCount);
}

void MainWindow::setWebAlertsPage(int page)
{
    const int pageCount =
        qMax(1, (m_webAlertsTotal + kWebAlertsPageSize - 1) /
                    kWebAlertsPageSize);
    page = qBound(0, page, pageCount - 1);
    if (page == m_webAlertsPage || m_webAlertsLoading)
        return;
    m_webAlertsPage = page;
    // Page navigation is intentional and must not be swallowed by the push
    // burst throttle used for repeat refreshes of the same page.
    m_webAlertsFetchedAtMs = 0;
    refreshWebAlerts(true);
}

QString MainWindow::notificationLinkLabel(const NotificationLink &link)
{
    if (!link.isValid())
        return {};
    if (link.kind == QLatin1String("chat"))
        return link.ref;
    if (link.kind == QLatin1String("firewall"))
        return link.ref.isEmpty() ? QStringLiteral("firewall")
                                  : QStringLiteral("firewall ") + link.ref;
    if (link.kind == QLatin1String("search"))
        return link.owner.isEmpty()
                   ? QStringLiteral("saved search")
                   : link.owner + QLatin1Char('/') + link.name +
                         QStringLiteral(" saved search");
    if (link.kind == QLatin1String("agent")) {
        const QString run = QStringLiteral("agent #%1").arg(link.number);
        return link.owner.isEmpty()
                   ? run
                   : link.owner + QLatin1Char('/') + link.name +
                         QLatin1Char(' ') + run;
    }
    if (link.kind == QLatin1String("cove")) {
        const QString repo = link.owner.isEmpty()
                                 ? QStringLiteral("cove")
                                 : link.owner + QLatin1Char('/') + link.name;
        return link.ref.isEmpty()
                   ? repo + QStringLiteral(" cove")
                   : repo + QStringLiteral(" cove @") + link.ref.left(8);
    }
    QString label = link.owner.isEmpty()
                        ? link.kind
                        : link.owner + QLatin1Char('/') + link.name;
    if (link.number > 0)
        label += QLatin1Char('#') + QString::number(link.number);
    else if (!link.ref.isEmpty())
        label += QLatin1Char('@') + link.ref.left(8);
    return label;
}

void MainWindow::openNotificationRow(int row)
{
    if (!m_notificationsTable || row < 0)
        return;
    QTableWidgetItem *first = m_notificationsTable->item(row, 0);
    if (!first)
        return;
    const int runId = first->data(Qt::UserRole).toInt();
    if (runId > 0) {
        openActionRunFromNotification(runId);
        return;
    }
    const QVariant nav = first->data(Qt::UserRole + 1);
    if (nav.canConvert<NotificationLink>() &&
        qvariant_cast<NotificationLink>(nav).isValid()) {
        openNotificationLink(qvariant_cast<NotificationLink>(nav));
        return;
    }
    const QString href = first->data(Qt::UserRole + 2).toString();
    if (href.isEmpty())
        return;
    const QUrl url(href);
    if (url.host() == catalogApiUrl().host() && openRunnerSignInLink(url))
        return;
    QDesktopServices::openUrl(url);
}

void MainWindow::openNotificationRepository(int row)
{
    if (!m_notificationsTable || row < 0)
        return;
    QTableWidgetItem *cell =
        m_notificationsTable->item(row, NotificationColRepository);
    if (!cell)
        return;
    const QString repo = cell->data(kPingFullTextRole).toString();
    const qsizetype slash = repo.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return;
    const int index = repoIndexFor(repo.left(slash), repo.mid(slash + 1));
    if (index < 0) {
        flashMessage(QStringLiteral("That repository isn't on this node anymore."),
                     true);
        return;
    }
    openRepoDetail(index);
}

QString MainWindow::notificationRowKey(int row) const
{
    if (!m_notificationsTable || row < 0)
        return {};
    QTableWidgetItem *first =
        m_notificationsTable->item(row, NotificationColType);
    if (!first)
        return {};
    const qlonglong localId = first->data(Qt::UserRole + 3).toLongLong();
    if (localId > 0)
        return QStringLiteral("local:%1").arg(localId);
    const QString webId = first->data(Qt::UserRole + 4).toString();
    if (!webId.isEmpty())
        return QStringLiteral("web:") + webId;
    const int runId = first->data(Qt::UserRole).toInt();
    if (runId > 0)
        return QStringLiteral("run:%1").arg(runId);
    return {};
}

int MainWindow::notificationRowForKey(const QString &key) const
{
    if (!m_notificationsTable || key.isEmpty())
        return -1;
    for (int row = 0; row < m_notificationsTable->rowCount(); ++row)
        if (notificationRowKey(row) == key)
            return row;
    return -1;
}

void MainWindow::showPingDetail(int row)
{
    const QString key = notificationRowKey(row);
    if (key.isEmpty() || !m_pingDetailPane)
        return;
    m_pingDetailKey = key;
    renderPingDetail(row);
    if (key.startsWith(QLatin1String("web:"))) {
        const QString alertId = key.mid(4);
        QTimer::singleShot(0, this,
                           [this, alertId] { markWebAlertRead(alertId); });
    }
}

void MainWindow::refreshPingDetail()
{
    if (!m_pingDetailPane || m_pingDetailPane->isHidden() ||
        m_pingDetailKey.isEmpty())
        return;
    const int row = notificationRowForKey(m_pingDetailKey);
    if (row < 0) {
        hidePingDetail();
        return;
    }
    {
        const QSignalBlocker blocker(m_notificationsTable);
        if (!m_notificationsTable->selectionModel()->isRowSelected(row))
            m_notificationsTable->selectRow(row);
    }
    renderPingDetail(row);
}

void MainWindow::hidePingDetail()
{
    m_pingDetailKey.clear();
    m_pingDetailSignature.clear();
    if (!m_pingDetailPane)
        return;
    m_pingDetailPane->hide();
    if (QWidget *old = m_pingDetailPane->takeWidget())
        old->deleteLater();
}

void MainWindow::renderPingDetail(int row)
{
    if (!m_pingDetailPane || !m_notificationsTable)
        return;
    QTableWidgetItem *typeCell =
        m_notificationsTable->item(row, NotificationColType);
    if (!typeCell) {
        hidePingDetail();
        return;
    }
    auto full = [this, row](int column) {
        QTableWidgetItem *cell = m_notificationsTable->item(row, column);
        if (!cell)
            return QString();
        const QVariant text = cell->data(kPingFullTextRole);
        return text.isValid() ? text.toString() : cell->text();
    };
    const QString type = full(NotificationColType);
    const QString kind = full(NotificationColKind);
    const QString title = pingTitleWithoutUnreadDot(full(NotificationColTitle));
    const QString detail = full(NotificationColDetail).trimmed();
    const QString repo = full(NotificationColRepository);
    const QString actor = full(NotificationColFrom);
    const QString status = full(NotificationColStatus);
    const QString when = full(NotificationColWhen);
    const QString link = full(NotificationColLink);
    QTableWidgetItem *statusCell =
        m_notificationsTable->item(row, NotificationColStatus);
    const QString statusWhy = statusCell ? statusCell->toolTip() : QString();
    const int runId = typeCell->data(Qt::UserRole).toInt();
    NotificationLink destination;
    const QVariant nav = typeCell->data(Qt::UserRole + 1);
    if (nav.canConvert<NotificationLink>())
        destination = qvariant_cast<NotificationLink>(nav);
    const QString href = typeCell->data(Qt::UserRole + 2).toString();
    const qlonglong localId = typeCell->data(Qt::UserRole + 3).toLongLong();
    const QString webId = typeCell->data(Qt::UserRole + 4).toString();
    const QString key = notificationRowKey(row);

    const QString signature =
        QStringList{key, type, kind, title, detail, repo, actor, status,
                    statusWhy, when, link, href, QString::number(runId),
                    QString::number(currentThemeIsDark())}
            .join(QChar(0x1f));
    m_pingDetailPane->show();
    if (signature == m_pingDetailSignature && m_pingDetailPane->widget())
        return;
    m_pingDetailSignature = signature;

    const bool next = Theme::isNext() &&
                      m_pingDetailPane->property("pingsNext").toBool();
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("issueSidebar"));
    body->setProperty("pingKey", key);
    QVBoxLayout *layout = nullptr;
    QWidget *actionsHost = nullptr;
    if (next) {
        body->setProperty("pingNext", true);
        auto *centre = new QHBoxLayout(body);
        centre->setContentsMargins(32, 28, 32, 28);
        centre->setSpacing(0);
        auto *column = new QWidget(body);
        column->setObjectName(QStringLiteral("pingsNextDetailColumn"));
        column->setMaximumWidth(720);
        layout = new QVBoxLayout(column);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        centre->addStretch(1);
        centre->addWidget(column, 100);
        centre->addStretch(1);
        actionsHost = new QWidget(column);
        actionsHost->setObjectName(QStringLiteral("pingsNextDetailActions"));
        new FlowLayout(actionsHost, 0, 6, 6);
    } else {
        layout = new QVBoxLayout(body);
        layout->setContentsMargins(14, 16, 12, 18);
        layout->setSpacing(8);
    }

    auto addButton = [this, body, layout, actionsHost](const QString &text,
                                                       const QString &icon,
                                                       const QString &objectName) {
        auto *button = new QPushButton(text, body);
        button->setObjectName(objectName);
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
        setOcticon(button, icon, 14);
        if (actionsHost)
            actionsHost->layout()->addWidget(button);
        else
            layout->addWidget(button, 0, Qt::AlignLeft);
        return button;
    };
    auto addHeading = [body, layout, next](const QString &text) {
        auto *label = new QLabel(next ? text.toUpper() : text, body);
        label->setObjectName(QStringLiteral("issueSidebarHeading"));
        layout->addSpacing(next ? 16 : 8);
        layout->addWidget(label);
    };

    const QString category =
        typeCell->data(kPingCategoryRole).toString();
    if (next) {
        const QTableWidgetItem *whenCell =
            m_notificationsTable->item(row, NotificationColWhen);
        const qint64 whenMs =
            whenCell ? whenCell->data(Qt::UserRole).toLongLong() : 0;
        auto *top = new QHBoxLayout;
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(12);
        auto *avatar = new QLabel(body);
        avatar->setObjectName(QStringLiteral("pingsNextDetailIcon"));
        avatar->setPixmap(pingsNextAvatar(
            typeCell->data(kPingIconNameRole).toString(),
            pingsNextPerson(category, actor), 36, body->devicePixelRatioF()));
        avatar->setFixedSize(36, 36);
        top->addWidget(avatar, 0, Qt::AlignTop);
        auto *heading = new QVBoxLayout;
        heading->setContentsMargins(0, 0, 0, 0);
        heading->setSpacing(6);
        auto *titleLabel = new QLabel(
            title.isEmpty() ? QStringLiteral("Ping") : title, body);
        titleLabel->setObjectName(QStringLiteral("issuePageTitle"));
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setWordWrap(true);
        titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        heading->addWidget(titleLabel);
        auto *meta = new QHBoxLayout;
        meta->setContentsMargins(0, 0, 0, 0);
        meta->setSpacing(6);
        const QString source =
            !actor.trimmed().isEmpty() ? actor.trimmed()
            : !repo.trimmed().isEmpty() ? repo.trimmed()
                                        : type;
        auto *sourceLabel = new QLabel(source, body);
        sourceLabel->setObjectName(QStringLiteral("pingsNextDetailMeta"));
        sourceLabel->setTextFormat(Qt::PlainText);
        meta->addWidget(sourceLabel);
        const QString whenText = pingsNextWhen(whenMs);
        if (!whenText.isEmpty()) {
            auto *dot = new QLabel(QString::fromUtf8("Â·"), body);
            dot->setObjectName(QStringLiteral("pingsNextDetailMetaDot"));
            meta->addWidget(dot);
            auto *clock = new QLabel(body);
            clock->setPixmap(kit::tintedIcon(QStringLiteral("clock"),
                                             Theme::color(Theme::Role::FgMuted),
                                             12, body->devicePixelRatioF()));
            meta->addWidget(clock);
            auto *whenLabel = new QLabel(whenText, body);
            whenLabel->setObjectName(QStringLiteral("pingsNextDetailMeta"));
            whenLabel->setToolTip(when);
            meta->addWidget(whenLabel);
        }
        if (!type.trimmed().isEmpty())
            meta->addWidget(new kit::Badge(
                type, category == QLatin1String("errors") ? kit::Tone::Danger
                                                          : kit::Tone::Neutral,
                false, body));
        meta->addStretch(1);
        heading->addLayout(meta);
        top->addLayout(heading, 1);
        auto *close = new QPushButton(body);
        close->setObjectName(QStringLiteral("ghostButton"));
        close->setProperty("buttonSize", "xs");
        close->setCursor(Qt::PointingHandCursor);
        close->setToolTip(QStringLiteral("Close details (Esc)"));
        setOcticon(close, QStringLiteral("x"), 12);
        connect(close, &QPushButton::clicked, this, [this] { hidePingDetail(); });
        top->addWidget(close, 0, Qt::AlignTop);
        layout->addLayout(top);
        layout->addSpacing(12);
        layout->addWidget(actionsHost);
    }

    auto *top = next ? nullptr : new QHBoxLayout;
    if (top) {
    top->setSpacing(8);
    auto *icon = new QLabel(body);
    icon->setPixmap(typeCell->icon().pixmap(20, 20));
    auto *typeLabel = new QLabel(type, body);
    typeLabel->setObjectName(QStringLiteral("mutedLabel"));
    auto *close = new QPushButton(body);
    close->setObjectName(QStringLiteral("ghostButton"));
    close->setProperty("buttonSize", "xs");
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("Close details (Esc)"));
    setOcticon(close, QStringLiteral("x"), 12);
    connect(close, &QPushButton::clicked, this, [this] { hidePingDetail(); });
    top->addWidget(icon);
    top->addWidget(typeLabel);
    top->addStretch();
    top->addWidget(close);
    layout->addLayout(top);

    auto *titleLabel = new QLabel(
        title.isEmpty() ? QStringLiteral("Ping") : title, body);
    titleLabel->setObjectName(QStringLiteral("issuePageTitle"));
    titleLabel->setTextFormat(Qt::PlainText);
    titleLabel->setWordWrap(true);
    titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(titleLabel);
    }
    if (!next && !detail.isEmpty()) {
        auto *detailLabel = new QLabel(detail, body);
        detailLabel->setObjectName(QStringLiteral("statusLine"));
        detailLabel->setTextFormat(Qt::PlainText);
        detailLabel->setWordWrap(true);
        detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(detailLabel);
    }

    const QString openLabel = pingOpenLabel(destination, runId, href);
    if (!openLabel.isEmpty()) {
        if (!next)
            layout->addSpacing(4);
        QPushButton *open = addButton(openLabel, QStringLiteral("chevron-right"),
                                      QStringLiteral("primaryButton"));
        connect(open, &QPushButton::clicked, this, [this, key] {
            const int current = notificationRowForKey(key);
            if (current >= 0)
                openNotificationRow(current);
        });
    }

    if (!next)
        addHeading(QStringLiteral("Actions"));
    const QString prompt = pingPromptText(type, title, detail, repo, actor,
                                          when, href.isEmpty() ? link : href);
    QPushButton *toPrompt =
        addButton(QStringLiteral("Send to prompt"),
                  QStringLiteral("paper-airplane"), QStringLiteral("ghostButton"));
    toPrompt->setToolTip(
        QStringLiteral("Add this ping to the prompt composer"));
    connect(toPrompt, &QPushButton::clicked, this,
            [this, prompt] { sendPingToPrompt(prompt); });
    QPushButton *copyText = addButton(QStringLiteral("Copy text"),
                                      QStringLiteral("copy"),
                                      QStringLiteral("ghostButton"));
    connect(copyText, &QPushButton::clicked, this, [this, prompt] {
        QGuiApplication::clipboard()->setText(prompt);
        flashMessage(QStringLiteral("Ping copied to the clipboard."));
    });
    if (!href.isEmpty()) {
        QPushButton *copyLink = addButton(QStringLiteral("Copy link"),
                                          QStringLiteral("link"),
                                          QStringLiteral("ghostButton"));
        connect(copyLink, &QPushButton::clicked, this, [this, href] {
            QGuiApplication::clipboard()->setText(href);
            flashMessage(QStringLiteral("Link copied to the clipboard."));
        });
    }
    const qsizetype repoSlash = repo.indexOf(QLatin1Char('/'));
    if (repoSlash > 0 &&
        repoIndexFor(repo.left(repoSlash), repo.mid(repoSlash + 1)) >= 0) {
        QPushButton *openRepo = addButton(QStringLiteral("Open repository"),
                                          QStringLiteral("repo"),
                                          QStringLiteral("ghostButton"));
        connect(openRepo, &QPushButton::clicked, this, [this, key] {
            const int current = notificationRowForKey(key);
            if (current >= 0)
                openNotificationRepository(current);
        });
    }
    if (localId > 0) {
        auto *fixes = new QMenu(body);
        addPingFixMenuActions(fixes, localId);
        if (fixes->actions().isEmpty()) {
            fixes->deleteLater();
        } else {
            QPushButton *fix = addButton(QStringLiteral("Quick fixes"),
                                         QStringLiteral("sparkle"),
                                         QStringLiteral("ghostButton"));
            fix->setMenu(fixes);
        }
    }
    const bool relayKind = forkmesh::pings::kindInfo(kind) != nullptr;
    QPushButton *settings = addButton(QStringLiteral("Ping settings"),
                                      QStringLiteral("gear"),
                                      QStringLiteral("ghostButton"));
    settings->setToolTip(
        relayKind ? QStringLiteral("Turn \"%1\" pings off or choose whether "
                                   "they email")
                        .arg(type)
                  : QStringLiteral("Ping settings for this desktop — turn "
                                   "this type off here"));
    connect(settings, &QPushButton::clicked, this, [this, kind, settings] {
        showPingKindMenu(settings, kind);
    });
    if (localId > 0 || !webId.isEmpty()) {
        QPushButton *remove = addButton(QStringLiteral("Delete ping"),
                                        QStringLiteral("trash"),
                                        QStringLiteral("issueDangerLink"));
        connect(remove, &QPushButton::clicked, this, [this, key] {
            const int current = notificationRowForKey(key);
            if (current < 0)
                return;
            m_notificationsTable->clearSelection();
            m_notificationsTable->selectRow(current);
            deleteSelectedNotifications();
        });
    }

    if (next && !detail.isEmpty()) {
        const bool log = category == QLatin1String("errors");
        addHeading(log ? QStringLiteral("Log excerpt")
                   : !actor.trimmed().isEmpty() ? QStringLiteral("Message")
                                                : QStringLiteral("Summary"));
        auto *card = new QFrame(body);
        card->setObjectName(QStringLiteral("pingsNextDetailCard"));
        card->setProperty("pingLog", log);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(16, 12, 16, 12);
        cardLayout->setSpacing(6);
        auto *detailLabel = new QLabel(detail, card);
        detailLabel->setObjectName(QStringLiteral("statusLine"));
        detailLabel->setProperty("pingDetailText", true);
        detailLabel->setTextFormat(Qt::PlainText);
        detailLabel->setWordWrap(true);
        detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (log)
            detailLabel->setFont(
                QFontDatabase::systemFont(QFontDatabase::FixedFont));
        cardLayout->addWidget(detailLabel);
        if (!log && !actor.trimmed().isEmpty()) {
            auto *by = new QLabel(actor.trimmed(), card);
            by->setObjectName(QStringLiteral("pingsNextDetailMeta"));
            by->setTextFormat(Qt::PlainText);
            cardLayout->addWidget(by);
        }
        layout->addWidget(card);
    }

    addHeading(QStringLiteral("Details"));
    auto *facts = new QFormLayout;
    facts->setContentsMargins(0, 0, 0, 0);
    facts->setHorizontalSpacing(12);
    facts->setVerticalSpacing(6);
    facts->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto addFact = [body, facts](const QString &label, const QString &value,
                                 const QString &tooltip = QString()) {
        if (value.trimmed().isEmpty())
            return;
        auto *name = new QLabel(label, body);
        name->setObjectName(QStringLiteral("mutedLabel"));
        auto *text = new QLabel(value, body);
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (!tooltip.isEmpty())
            text->setToolTip(tooltip);
        facts->addRow(name, text);
    };
    addFact(QStringLiteral("Type"), type);
    addFact(QStringLiteral("Kind"), kind);
    addFact(QStringLiteral("Repository"), repo);
    addFact(QStringLiteral("From"), actor);
    addFact(QStringLiteral("Status"), status, statusWhy);
    addFact(QStringLiteral("When"), when);
    addFact(QStringLiteral("Link"), href.isEmpty() ? link : href);
    layout->addLayout(facts);
    if (!statusWhy.isEmpty() && statusWhy != status) {
        auto *why = new QLabel(statusWhy, body);
        why->setObjectName(QStringLiteral("mutedLabel"));
        why->setTextFormat(Qt::PlainText);
        why->setWordWrap(true);
        layout->addWidget(why);
    }
    layout->addStretch();

    if (QWidget *old = m_pingDetailPane->takeWidget())
        old->deleteLater();
    m_pingDetailPane->setWidget(body);
}

void MainWindow::markWebAlertRead(const QString &alertId)
{
    const QString id = alertId.trimmed().toLower();
    if (id.isEmpty())
        return;
    bool changed = false;
    for (int i = 0; i < m_webAlerts.size(); ++i) {
        QJsonObject alert = m_webAlerts.at(i).toObject();
        if (alert.value(QStringLiteral("id")).toString().trimmed().toLower() !=
            id)
            continue;
        if (alert.value(QStringLiteral("readAt")).toDouble() > 0)
            return;
        alert.insert(QStringLiteral("readAt"),
                     double(QDateTime::currentMSecsSinceEpoch()));
        m_webAlerts.replace(i, alert);
        if (m_webAlertsUnread > 0)
            --m_webAlertsUnread;
        changed = true;
        break;
    }
    if (!changed)
        return;
    updateNotificationButton();
    refreshNotificationsTable();
    if (!m_networkAccess)
        return;
    const QString node = accountOwner().trimmed().toLower();
    if (node.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/notifications"));
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (!authenticateOrgTaskRequest(url, request, kAccountAlertReadProof,
                                    QString()))
        return;
    const QJsonObject payload{{QStringLiteral("node"), node},
                              {QStringLiteral("ids"), QJsonArray{id}}};
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 ||
            status >= 300)
            m_webAlertsFetchedAtMs = 0;
    });
}

// The composer is a footer overlay, on screen whichever section is showing, so
// a ping reaches it without leaving the Pings page.
void MainWindow::sendPingToPrompt(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    if (!m_issueQuickAdd) {
        QGuiApplication::clipboard()->setText(text);
        logSystem(QStringLiteral(
            "No prompt composer on screen; the ping went to the clipboard."));
        return;
    }
    appendTextToPrompt(text);
    logSystem(QStringLiteral("Added a ping to the prompt composer."));
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testShowPingsSection()
{
    showNotifications();
    refreshNotificationsTable();
}

bool MainWindow::testClickPingSettings(const QString &titleNeedle,
                                       QString *highlightedKind)
{
    if (!m_notificationsTable)
        return false;
    for (int row = 0; row < m_notificationsTable->rowCount(); ++row) {
        QTableWidgetItem *title =
            m_notificationsTable->item(row, NotificationColTitle);
        if (!title || !title->text().contains(titleNeedle))
            continue;
        auto *button = qobject_cast<QPushButton *>(
            m_notificationsTable->cellWidget(row, NotificationColSettings));
        if (!button)
            return false;
        button->click();
        QCoreApplication::processEvents();
        for (QMenu *menu :
             findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!menu->isVisible())
                continue;
            const QList<QAction *> actions = menu->actions();
            if (!actions.isEmpty())
                actions.last()->trigger();
            menu->close();
            break;
        }
        QCoreApplication::processEvents();
        if (highlightedKind) {
            highlightedKind->clear();
            for (QFrame *frame : findChildren<QFrame *>(
                     QStringLiteral("pingKindRow"))) {
                if (!frame->styleSheet().isEmpty())
                    *highlightedKind = frame->property("pingKind").toString();
            }
        }
        return m_settingsSections &&
               m_settingsSections->currentIndex() == m_pingsSettingsTabIndex;
    }
    return false;
}

int MainWindow::testPingsTableOverflowPx() const
{
    if (!m_notificationsTable)
        return 0;
    int columns = 0;
    for (int column = 0; column < m_notificationsTable->columnCount(); ++column)
        if (!m_notificationsTable->isColumnHidden(column))
            columns += m_notificationsTable->columnWidth(column);
    return qMax(0, columns - m_notificationsTable->viewport()->width());
}

QString MainWindow::testClickPingPrompt(const QString &titleNeedle)
{
    if (!m_notificationsTable || !m_issueQuickAdd)
        return {};
    for (int row = 0; row < m_notificationsTable->rowCount(); ++row) {
        QTableWidgetItem *title =
            m_notificationsTable->item(row, NotificationColTitle);
        if (!title ||
            !title->data(kPingFullTextRole).toString().contains(titleNeedle))
            continue;
        auto *button = qobject_cast<QPushButton *>(
            m_notificationsTable->cellWidget(row, NotificationColPrompt));
        if (!button)
            return {};
        m_issueQuickAdd->clear();
        button->click();
        return m_issueQuickAdd->toPlainText();
    }
    return {};
}

bool MainWindow::testPingCellOpens(const QString &titleNeedle,
                                   int column) const
{
    if (!m_notificationsTable)
        return false;
    for (int row = 0; row < m_notificationsTable->rowCount(); ++row) {
        QTableWidgetItem *title =
            m_notificationsTable->item(row, NotificationColTitle);
        if (!title ||
            !title->data(kPingFullTextRole).toString().contains(titleNeedle))
            continue;
        QTableWidgetItem *cell = m_notificationsTable->item(row, column);
        return cell && cell->data(kPingOpensRole).toBool();
    }
    return false;
}

QString MainWindow::testOpenPingDetail(const QString &titleNeedle)
{
    if (!m_notificationsTable)
        return {};
    for (int row = 0; row < m_notificationsTable->rowCount(); ++row) {
        QTableWidgetItem *title =
            m_notificationsTable->item(row, NotificationColTitle);
        if (!title ||
            !title->data(kPingFullTextRole).toString().contains(titleNeedle))
            continue;
        emit m_notificationsTable->itemClicked(title);
        QCoreApplication::processEvents();
        if (!m_pingDetailPane || m_pingDetailPane->isHidden() ||
            !m_pingDetailPane->widget())
            return {};
        QStringList parts;
        for (QLabel *label : m_pingDetailPane->widget()->findChildren<QLabel *>())
            parts << label->text();
        for (QPushButton *button :
             m_pingDetailPane->widget()->findChildren<QPushButton *>())
            if (!button->text().isEmpty())
                parts << QStringLiteral("[%1]").arg(button->text());
        return parts.join(QLatin1Char('\n'));
    }
    return {};
}

bool MainWindow::testPingDetailVisible() const
{
    return m_pingDetailPane && !m_pingDetailPane->isHidden();
}
#endif

void MainWindow::deleteSelectedNotifications()
{
    if (!m_notificationsTable || !m_notificationsTable->selectionModel())
        return;
    QSet<qlonglong> localIds;
    QStringList webIds;
    int approvals = 0;
    const QModelIndexList rows =
        m_notificationsTable->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        QTableWidgetItem *first = m_notificationsTable->item(index.row(), 0);
        if (!first)
            continue;
        const qlonglong localId = first->data(Qt::UserRole + 3).toLongLong();
        const QString webId = first->data(Qt::UserRole + 4).toString();
        if (localId > 0)
            localIds.insert(localId);
        else if (!webId.isEmpty())
            webIds << webId;
        else
            ++approvals;
    }
    if (localIds.isEmpty() && webIds.isEmpty()) {
        if (approvals > 0)
            flashMessage(QStringLiteral(
                             "Approvals clear themselves once the run is "
                             "approved or rejected."),
                         false);
        return;
    }
    if (!localIds.isEmpty()) {
        auto stale = [&localIds](const AppNotification &notice) {
            return localIds.contains(static_cast<qlonglong>(notice.id));
        };
        QStringList phonePingIds;
        for (const AppNotification &notice : std::as_const(m_notifications))
            if (stale(notice) && !notice.phonePingId.isEmpty())
                phonePingIds << notice.phonePingId;
        m_notifications.erase(std::remove_if(m_notifications.begin(),
                                             m_notifications.end(), stale),
                              m_notifications.end());
        deleteForwardedPings(phonePingIds);
    }
    for (const QString &alertId : std::as_const(webIds))
        deleteWebAlert(alertId);
    updateNotificationButton();
    scheduleNotificationJournalSave();
    refreshNotificationsTable();
}

// Delete one mirrored website ping from this account's inbox on the relay.
// The desktop holds no session token, so the request is signed with the
// account key and a proof that binds the id being deleted.
void MainWindow::deleteWebAlert(const QString &alertId)
{
    const QString id = alertId.trimmed().toLower();
    QJsonArray remaining;
    bool removed = false;
    for (const QJsonValue &value : std::as_const(m_webAlerts)) {
        const QJsonObject alert = value.toObject();
        if (alert.value(QStringLiteral("id")).toString().trimmed().toLower() == id) {
            if (alert.value(QStringLiteral("readAt")).toDouble() <= 0 &&
                m_webAlertsUnread > 0)
                --m_webAlertsUnread;
            removed = true;
            continue;
        }
        remaining.append(value);
    }
    m_webAlerts = remaining;
    if (removed && m_webAlertsTotal > 0)
        --m_webAlertsTotal;
    updateWebAlertPager();
    if (!m_networkAccess || id.isEmpty())
        return;
    const QString node = accountOwner().trimmed().toLower();
    if (node.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/notifications"));
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (!authenticateOrgTaskRequest(url, request, kAccountAlertDeleteProof, id))
        return;
    const QJsonObject body{{QStringLiteral("node"), node},
                           {QStringLiteral("id"), id}};
    QNetworkReply *reply = m_networkAccess->sendCustomRequest(
        request, QByteArrayLiteral("DELETE"),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 ||
            status >= 300) {
            flashMessage(QStringLiteral(
                             "Couldn't delete that website ping (%1).")
                             .arg(status),
                         true);
            m_webAlertsFetchedAtMs = 0;
        }
    });
}

// Pull this account's website alert inbox onto the Notifications page. The
// desktop normally holds no account session token (authenticateSilently proves
// the account key instead), so the read is signed exactly like the Tasks
// board's — see _account_alert_signed_session in the worker.
void MainWindow::refreshWebAlerts(bool force)
{
    if (!m_networkAccess || m_webAlertsLoading)
        return;
    const QString node = accountOwner().trimmed().toLower();
    if (node.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_webAlertsFetchedAtMs > 0 &&
        (!force || now - m_webAlertsFetchedAtMs < kWebAlertPushFloorMs))
        return;

    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/notifications"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("node"), node);
    query.addQueryItem(QStringLiteral("limit"),
                       QString::number(kWebAlertsPageSize));
    query.addQueryItem(QStringLiteral("offset"),
                       QString::number(m_webAlertsPage * kWebAlertsPageSize));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (!authenticateOrgTaskRequest(url, request, kAccountAlertListProof,
                                    QString()))
        return; // neither a session nor this account's signing key
    m_webAlertsLoading = true;
    m_webAlertsFetchedAtMs = now;
    updateWebAlertPager();
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_webAlertsLoading = false;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 ||
            status >= 300) {
            m_webAlertsFetchedAtMs = 0;
            updateWebAlertPager();
            return;
        }
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray fetched =
            payload.value(QStringLiteral("notifications")).toArray();
        m_webAlertsTotal = payload.value(QStringLiteral("total"))
                               .toInt(fetched.size());
        m_webAlertsUnread = payload.value(QStringLiteral("unread")).toInt();
        const QString node = machineNodeName();
        QJsonArray alerts;
        for (const QJsonValue &value : fetched) {
            const QJsonObject alert = value.toObject();
            if (!forkmesh::pingforward::isOwnEcho(alert, node)) {
                alerts.append(value);
                continue;
            }
            m_webAlertsTotal = qMax(0, m_webAlertsTotal - 1);
            if (alert.value(QStringLiteral("readAt")).toDouble() <= 0)
                m_webAlertsUnread = qMax(0, m_webAlertsUnread - 1);
        }
        const int pageCount =
            qMax(1, (m_webAlertsTotal + kWebAlertsPageSize - 1) /
                        kWebAlertsPageSize);
        if (m_webAlertsPage >= pageCount) {
            m_webAlertsPage = pageCount - 1;
            m_webAlertsFetchedAtMs = 0;
            refreshWebAlerts(true);
            return;
        }
        m_webAlerts = alerts;
        updateNotificationButton();
        updateWebAlertPager();
        celebrateFreshWebAlerts();
        const bool loadingStartupBaseline = !m_webAlertsBaselineLoaded;
        m_webAlertsBaselineLoaded = true;
        QList<QJsonObject> pendingAlerts;
        for (const QJsonValue &value : std::as_const(m_webAlerts)) {
            const QJsonObject alert = value.toObject();
            const QString kind =
                alert.value(QStringLiteral("kind")).toString().trimmed();
            // Organization task pings (activity, comments, check-ins, and an
            // @mention inside one) exist only in this Worker inbox - no local
            // event raises them - so this read is where the desktop surfaces
            // them. A repository @mention is left alone: the desktop already
            // raised that one from its own issue/pull sync.
            const bool taskPing =
                kind.startsWith(QLatin1String("organization_task_")) ||
                (kind == QLatin1String("mention") &&
                 !alert.value(QStringLiteral("meta"))
                      .toObject()
                      .value(QStringLiteral("taskId"))
                      .toString()
                      .isEmpty());
            if ((kind != QLatin1String("error_group") &&
                 kind != QLatin1String("operational_alert") && !taskPing) ||
                alert.value(QStringLiteral("readAt")).toDouble() > 0)
                continue;
            const QString id =
                alert.value(QStringLiteral("id")).toString().trimmed();
            if (id.isEmpty() || m_flashedWebAlertIds.contains(id))
                continue;
            m_flashedWebAlertIds.insert(id);
            if (loadingStartupBaseline)
                continue;
            if (kind == QLatin1String("operational_alert") &&
                !forkMeshSystemAlertsEnabled())
                continue;
            pendingAlerts.append(alert);
        }
        // The inbox is returned newest first. Flashing that order makes an
        // unread recovery appear green, then lets its older outage card queue
        // behind it and finish red. Show the transition history oldest first
        // so the newest state is the one left on screen.
        std::stable_sort(
            pendingAlerts.begin(), pendingAlerts.end(),
            [](const QJsonObject &left, const QJsonObject &right) {
                return left.value(QStringLiteral("ts")).toVariant().toLongLong()
                       < right.value(QStringLiteral("ts"))
                             .toVariant()
                             .toLongLong();
            });
        for (const QJsonObject &alert : std::as_const(pendingAlerts))
            if (alert.value(QStringLiteral("href"))
                    .toString()
                    .contains(QLatin1String("/dashboard/runners"))) {
                refreshRunnerAttention(true);
                break;
            }
        for (const QJsonObject &alert : std::as_const(pendingAlerts)) {
            const QString kind =
                alert.value(QStringLiteral("kind")).toString().trimmed();
            const QString title =
                alert.value(QStringLiteral("title")).toString().trimmed();
            const bool recovery = kind == QLatin1String("operational_alert") &&
                                  webPingIsRecovery(alert);
            if (kind != QLatin1String("error_group") &&
                kind != QLatin1String("operational_alert")) {
                AppNotification taskPing;
                taskPing.title = title.isEmpty()
                                     ? QStringLiteral("Task activity")
                                     : title;
                taskPing.body =
                    alert.value(QStringLiteral("body")).toString().trimmed();
                taskPing.kind = kind;
                taskPing.id = -1;
                const bool reminder =
                    alert.value(QStringLiteral("meta"))
                        .toObject()
                        .value(QStringLiteral("context"))
                        .toString() == QLatin1String("reminder");
                taskPing.dismissOnly = reminder;
                flashNotification(taskPing);
                if (reminder) {
                    if (nativeSystemNotificationsEnabled() && m_trayIcon &&
                        QSystemTrayIcon::supportsMessages())
                        m_trayIcon->showMessage(taskPing.title, taskPing.body,
                                                QSystemTrayIcon::Information,
                                                0);
                    continue;
                }
                // The row is already on the Pings page (it came from there), so
                // only the OS card is raised here; showPingTrayMessage honours
                // the pause and the native-notification switch.
                showPingTrayMessage(taskPing.title, taskPing.body);
                continue;
            }
            AppNotification ping;
            ping.title = title.isEmpty()
                             ? (kind == QLatin1String("operational_alert")
                                    ? (recovery
                                           ? QStringLiteral("ForkMesh system recovered")
                                           : QStringLiteral("ForkMesh system alert"))
                                    : QStringLiteral("New error group on the relay"))
                             : title;
            ping.body = alert.value(QStringLiteral("body")).toString().trimmed();
            ping.warning = !recovery;
            ping.kind = kind;
            ping.id = -1;
            flashNotification(ping);
        }
        if (m_notificationsTable && m_sectionStack &&
            m_sectionStack->currentIndex() == 3)
            refreshNotificationsTable();
    });
}

void MainWindow::celebrateFreshWebAlerts()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject newest;
    for (const QJsonValue &value : std::as_const(m_webAlerts)) {
        const QJsonObject alert = value.toObject();
        if (!forkmesh::pingforward::isFreshCelebration(alert, now))
            continue;
        const QString id = alert.value(QStringLiteral("id")).toString().trimmed();
        if (m_celebratedWebAlertIds.contains(id))
            continue;
        m_celebratedWebAlertIds.insert(id);
        if (newest.isEmpty() ||
            alert.value(QStringLiteral("ts")).toDouble() >
                newest.value(QStringLiteral("ts")).toDouble())
            newest = alert;
    }
    if (newest.isEmpty() || pingsPaused())
        return;
    const QString title =
        newest.value(QStringLiteral("title")).toString().trimmed();
    const QString body =
        newest.value(QStringLiteral("body")).toString().trimmed();
    new forkmesh::ui::CelebrationOverlay(
        this, 10000, title.isEmpty() ? QStringLiteral("🎉") : title, body, 2);
    logSystem(title.isEmpty() ? QStringLiteral("🎉") : title);
    showPingTrayMessage(title, body);
    cheerGenieFor(newest);
}

namespace {
constexpr int kWebAlertsClearMaxRetries = 5;
int g_webAlertsClearRetries = 0;
}

void MainWindow::clearWebAlerts()
{
    m_webAlerts = QJsonArray();
    m_webAlertsUnread = 0;
    m_webAlertsTotal = 0;
    m_webAlertsPage = 0;
    updateWebAlertPager();
    if (!m_networkAccess)
        return;
    const QString node = accountOwner().trimmed().toLower();
    if (node.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/notifications"));
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    // A bulk deletion has its own signed proof; a captured row-delete proof
    // cannot be replayed to empty the whole inbox.
    if (!authenticateOrgTaskRequest(url, request, kAccountAlertClearProof,
                                    QStringLiteral("all"))) {
        flashMessage(QStringLiteral(
                         "Couldn't clear website pings: sign in or unlock "
                         "this account's key."),
                     true);
        m_webAlertsFetchedAtMs = 0;
        return;
    }
    const QJsonObject body{{QStringLiteral("node"), node},
                           {QStringLiteral("all"), true}};
    QNetworkReply *reply = m_networkAccess->sendCustomRequest(
        request, QByteArrayLiteral("DELETE"),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 &&
            status < 300) {
            g_webAlertsClearRetries = 0;
            return;
        }
        m_webAlertsFetchedAtMs = 0;
        const bool backingOff =
            BackoffNetworkAccessManager::isBackoffSuppressed(reply) || status == 429;
        if (backingOff && g_webAlertsClearRetries < kWebAlertsClearMaxRetries) {
            ++g_webAlertsClearRetries;
            const auto *network =
                qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
            const qint64 waitMs =
                network ? network->hostCooldownRemainingMs(url.host()) : 0;
            logSystem(QStringLiteral("Pings: the relay is backing off, so clearing "
                                     "website pings will be retried in %1 s.")
                          .arg(qMax<qint64>(1, (waitMs + 999) / 1000)));
            QTimer::singleShot(int(qBound(qint64(1000), waitMs + 500,
                                          qint64(10 * 60 * 1000))),
                               this, [this] { clearWebAlerts(); });
            return;
        }
        g_webAlertsClearRetries = 0;
        const QString reason =
            status > 0 ? QStringLiteral("HTTP %1").arg(status)
                       : reply->errorString().simplified();
        flashMessage(QStringLiteral("Couldn't clear website pings (%1).").arg(reason),
                     true);
    });
}

void MainWindow::showNotifications()
{
    showSection(3);
}

// libnotify 0.8 can hand a notification click back to the process that raised
// it: --wait keeps notify-send alive until the notification closes and prints
// the key of the action that closed it. Older builds have neither flag and
// abort on them, so probe once and fall back to an unclickable notification
// rather than none at all.
// code-health: allow(blocking-wait) a once-per-process probe: the static initializer runs `notify-send --help` the first time postNotification raises a clickable Linux notification, capped at 1.5 s and killed on timeout; every later call reads the cached bool
static bool notifySendSupportsActions()
{
    static const bool supported = [] {
        const QString exe =
            QStandardPaths::findExecutable(QStringLiteral("notify-send"));
        if (exe.isEmpty())
            return false;
        QProcess probe;
        probe.start(exe, {QStringLiteral("--help")});
        if (!probe.waitForFinished(1500)) {
            probe.kill();
            probe.waitForFinished(200);
            return false;
        }
        const QString help = QString::fromUtf8(probe.readAllStandardOutput()) +
                             QString::fromUtf8(probe.readAllStandardError());
        return help.contains(QLatin1String("--action"));
    }();
    return supported;
}

static bool notificationServerReadsMarkup()
{
    static const bool markup = [] {
        const QString exe = QStandardPaths::findExecutable(QStringLiteral("gdbus"));
        if (exe.isEmpty())
            return false;
        QProcess probe;
        probe.start(exe, {QStringLiteral("call"), QStringLiteral("--session"),
                          QStringLiteral("--dest"),
                          QStringLiteral("org.freedesktop.Notifications"),
                          QStringLiteral("--object-path"),
                          QStringLiteral("/org/freedesktop/Notifications"),
                          QStringLiteral("--method"),
                          QStringLiteral("org.freedesktop.Notifications.GetCapabilities")});
        if (!probe.waitForFinished(1500)) {
            probe.kill();
            probe.waitForFinished(200);
            return false;
        }
        return QString::fromUtf8(probe.readAllStandardOutput())
            .contains(QLatin1String("'body-markup'"));
    }();
    return markup;
}

void MainWindow::postNotification(const QString &title, const QString &body,
                                  bool warning, const QString &icon,
                                  const QString &activateHref,
                                  bool dismissOnly, bool fileLocally)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString osText = (title + QLatin1Char(' ') + body).simplified();
    const bool alreadyFiled = std::any_of(
        m_notifications.cbegin(), m_notifications.cend(),
        [&](const AppNotification &filed) {
            if (now - filed.timestampMs > kPingDedupeWindowMs)
                return false;
            const QString filedText =
                (filed.title + QLatin1Char(' ') + filed.body).simplified();
            return sameAlertText(filedText, osText)
                   || sameAlertText(filed.body, body)
                   || sameAlertText(filed.title, body);
        });
    if (fileLocally && !alreadyFiled && !osText.isEmpty()) {
        AppNotification item;
        item.title = title;
        item.body = body;
        item.warning = warning;
        item.kind = QStringLiteral("desktop");
        // The Pings row opens the same place the OS card does, so the ping is
        // still reachable long after the desktop notification is dismissed.
        item.link = notificationLinkForHref(activateHref);
        item.dismissOnly = dismissOnly;
        item.forward = true;
        // A regular native notification deliberately avoids duplicating itself
        // as an in-app card. A dismiss-only one is the exception: its in-app
        // card is where ForkMesh enforces the required acknowledgement.
        item.quiet = !dismissOnly;
        recordNotification(item);
    }
    // Filed above, so the Pings page carries this one either way; what a pause
    // suppresses is the OS card that interrupts whatever the user is doing.
    if (pingsPaused() || !nativeSystemNotificationsEnabled())
        return;
    if (dismissOnly ||
        batchNativeNotificationCard(title, body, warning, icon, activateHref))
        showNativeNotificationCard(title, body, warning, icon, activateHref,
                                   dismissOnly);
}

namespace {

class PingCardBatchState : public QObject
{
public:
    explicit PingCardBatchState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshPingCardBatchState"));
        flushTimer = new QTimer(this);
        flushTimer->setSingleShot(true);
    }

    forkmesh::pingbatch::Batcher cards;
    forkmesh::pingbatch::Batcher sounds;
    QString lastIcon;
    QTimer *flushTimer = nullptr;
    bool wired = false;
};

PingCardBatchState *pingCardBatchState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(
        QStringLiteral("forkmeshPingCardBatchState"), Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<PingCardBatchState *>(found))
        return state;
    return new PingCardBatchState(window);
}

}

bool MainWindow::batchNativeNotificationCard(const QString &title,
                                             const QString &body, bool warning,
                                             const QString &icon,
                                             const QString &activateHref)
{
    PingCardBatchState *state = pingCardBatchState(this);
    if (!state->wired) {
        state->wired = true;
        connect(state->flushTimer, &QTimer::timeout, this,
                [this] { flushNativeNotificationBatch(); });
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state->cards.due(now))
        flushNativeNotificationBatch();
    forkmesh::pingbatch::Item item;
    item.title = title;
    item.body = body;
    item.warning = warning;
    item.href = activateHref;
    item.atMs = now;
    switch (state->cards.offer(item, now)) {
    case forkmesh::pingbatch::Step::Show:
        return true;
    case forkmesh::pingbatch::Step::Summarize:
        state->lastIcon = icon;
        flushNativeNotificationBatch();
        return false;
    case forkmesh::pingbatch::Step::Hold:
        break;
    }
    state->lastIcon = icon;
    state->flushTimer->start(
        int(std::max<qint64>(0, state->cards.dueAtMs() - now)));
    return false;
}

void MainWindow::flushNativeNotificationBatch()
{
    PingCardBatchState *state = pingCardBatchState(this);
    state->flushTimer->stop();
    if (state->cards.held() == 0)
        return;
    const forkmesh::pingbatch::Card card = state->cards.take();
    if (pingsPaused() || !nativeSystemNotificationsEnabled())
        return;
    if (card.summary)
        showNativeNotificationCard(card.title, card.body, card.warning,
                                   QString(), QStringLiteral("fm:pings"), false,
                                   10000);
    else
        showNativeNotificationCard(card.title, card.body, card.warning,
                                   state->lastIcon, card.href, false);
}

bool MainWindow::admitPingSound()
{
    PingCardBatchState *state = pingCardBatchState(this);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state->sounds.due(now))
        state->sounds.take();
    forkmesh::pingbatch::Item item;
    item.atMs = now;
    switch (state->sounds.offer(item, now)) {
    case forkmesh::pingbatch::Step::Show:
        return true;
    case forkmesh::pingbatch::Step::Summarize:
        state->sounds.take();
        return true;
    case forkmesh::pingbatch::Step::Hold:
        break;
    }
    return false;
}

void MainWindow::showNativeNotificationCard(const QString &title,
                                            const QString &body, bool warning,
                                            const QString &icon,
                                            const QString &activateHref,
                                            bool dismissOnly, int trayTimeoutMs)
{
    m_desktopNotificationHref = activateHref;
#ifdef FORKMESH_WINDOW_TESTS
    // The window tests drive the real notifyAgentDone/postNotification path, and
    // an ungated notify-send here files a live desktop notification on whoever
    // is running ctest — one per run, which is how a stack of identical
    // "Agent #133894 is done!" cards builds up in the OS notification centre.
    // The in-app ping above is still recorded, so the assertions keep working.
    return;
#else
    const QString iconName =
        !icon.isEmpty() ? icon
                        : (warning ? QStringLiteral("dialog-error")
                                   : QStringLiteral("dialog-information"));
#if defined(Q_OS_LINUX)
    static const QString notifySend =
        QStandardPaths::findExecutable(QStringLiteral("notify-send"));
    if (!notifySend.isEmpty()) {
        QStringList args = {
            QStringLiteral("-a"), QStringLiteral("ForkMesh"),
            QStringLiteral("-i"), iconName,
            QStringLiteral("-u"),
            warning ? QStringLiteral("critical") : QStringLiteral("normal")};
        // notify-send's zero timeout requests a resident card. Notification
        // servers may still impose their own policy, but the in-app card above
        // remains the authoritative acknowledgement surface.
        if (dismissOnly)
            args << QStringLiteral("-t") << QStringLiteral("0");
        // A notification nobody can click is a dead end: wait on the ones that
        // have somewhere to go, capped so a burst cannot leave a process per
        // card parked on the notification server.
        const bool clickable = !activateHref.isEmpty() &&
                               m_desktopNotificationWaiters < kMaxNotifyWaiters &&
                               notifySendSupportsActions();
        if (clickable)
            args << QStringLiteral("--wait")
                 << QStringLiteral("--action=default=Open");
        args << title
             << (notificationServerReadsMarkup() ? body.toHtmlEscaped() : body);
        if (clickable) {
            auto *proc = new QProcess(this);
            ++m_desktopNotificationWaiters;
            connect(proc, &QProcess::finished, this,
                    [this, proc, activateHref](int, QProcess::ExitStatus) {
                        --m_desktopNotificationWaiters;
                        const QString picked =
                            QString::fromUtf8(proc->readAllStandardOutput())
                                .trimmed();
                        proc->deleteLater();
                        if (picked == QLatin1String("default"))
                            activateDesktopNotification(activateHref);
                    });
            proc->start(notifySend, args);
            // waitForStarted blocks on the process channel; unlike an exec() or
            // a git pump it does not run the GUI event loop, so nothing can be
            // destroyed underneath the capture above.
            if (proc->waitForStarted(2000))
                return;
            --m_desktopNotificationWaiters;
            proc->disconnect(this);
            proc->deleteLater();
            args.removeAll(QStringLiteral("--wait"));
            args.removeAll(QStringLiteral("--action=default=Open"));
        }
        if (forkmesh::processreaper::startReleased(notifySend, args))
            return;
    }
#endif
#endif
    if (m_trayIcon && QSystemTrayIcon::isSystemTrayAvailable()) {
        const QFileInfo iconFile(icon);
        if (!warning && iconFile.isAbsolute() && iconFile.isFile())
            m_trayIcon->showMessage(title, body, QIcon(icon),
                                    dismissOnly ? 0 : trayTimeoutMs);
        else
            m_trayIcon->showMessage(
                title, body,
                warning ? QSystemTrayIcon::Warning
                        : QSystemTrayIcon::Information,
                dismissOnly ? 0 : trayTimeoutMs);
    }
}

void MainWindow::showPingTrayMessage(const QString &title, const QString &body,
                                     bool warning, int timeoutMs)
{
    // Some older event sources use the native tray directly rather than the
    // postNotification() funnel. They are still allowed to create/file their
    // Pings; this is only the visible delivery rail, so the shared pause has to
    // stop it here as well.
    if (pingsPaused() || !nativeSystemNotificationsEnabled() || !m_trayIcon ||
        !QSystemTrayIcon::supportsMessages())
        return;
    if (timeoutMs != 0 &&
        !batchNativeNotificationCard(title, body, warning, QString(), QString()))
        return;
    m_trayIcon->showMessage(
        title, body,
        warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information,
        timeoutMs);
}

void MainWindow::refreshActionsTable()
{
    if (!m_actionsTable)
        return;
    refreshWorkflowProgress();
    QString owner, name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }

    // Work out every cell first, then compare that against what is already
    // rendered. A single run status change fans out into several callers that all
    // land here (statusChanged, finished, the spin tick, the pull checks
    // refresh), and each rebuild throws away and re-creates every row — four
    // QTableWidgetItems plus a tinted icon apiece, each one a model signal the
    // view reacts to. Stalls were sampled inside setRowCount() and setItem()
    // here. The rows below are the whole rendered table, so identical rows mean
    // an identical table and the rebuild is pure cost.
    struct PreparedRow {
        int runId = -1;
        bool failed = false;
        QString workflowName;
        QString detail;
        QString iconName;
        QString statusText;
        QString statusTooltip;
        QColor statusColor;
        QString when;
        QString whenTooltip;
        QString durationText;
    };
    QList<PreparedRow> rows;
    QString signature;
    for (const ActionRun &run : m_actionRuns) {
        if (run.owner != owner || run.name != name)
            continue;
        if (!m_selectedWorkflowFilter.isEmpty() &&
            run.workflowPath != m_selectedWorkflowFilter)
            continue;
        PreparedRow prepared;
        prepared.runId = run.id;
        prepared.failed = run.status == ActionStatus::Failed;
        prepared.workflowName = run.workflowName;
        QStringList detail;
        QString branch = run.ref;
        for (const QLatin1String prefix : {QLatin1String("refs/heads/"),
                                           QLatin1String("refs/tags/"),
                                           QLatin1String("refs/")}) {
            if (branch.startsWith(prefix)) {
                branch = branch.mid(prefix.size());
                break;
            }
        }
        if (!branch.isEmpty())
            detail << branch;
        if (!run.commit.isEmpty())
            detail << run.commit.left(8);
        const QString machine =
            !run.machine.isEmpty() ? run.machine
            : run.startedAtMs > 0  ? machineNodeName()
                                   : QString();
        if (!machine.isEmpty())
            detail << machine;
        prepared.detail = detail.join(QString::fromUtf8("  \xC2\xB7  "));
        prepared.iconName = actionRunStatusIconName(run.status);
        prepared.statusText = actionStatusText(run.status);
        // "Queued" alone invites the question this answers.
        prepared.statusTooltip = actionRunQueueReason(run.id);
        prepared.statusColor = actionStatusColor(run.status);
        if (run.createdAtMs > 0) {
            const QString rel = formatShortRelativeTime(run.createdAtMs / 1000);
            prepared.when = rel == QStringLiteral("now")
                                ? rel
                                : rel + QStringLiteral(" ago");
            prepared.whenTooltip =
                QDateTime::fromMSecsSinceEpoch(run.createdAtMs)
                    .toString(QStringLiteral("MMM d  hh:mm"));
        }
        if (run.status == ActionStatus::Queued ||
            run.status == ActionStatus::Running) {
            const qint64 last = estimatedRunDurationMs(run);
            if (last > 0) {
                const QString lastStr =
                    QStringLiteral("last %1").arg(formatDuration(last));
                prepared.when =
                    prepared.when.isEmpty()
                        ? lastStr
                        : prepared.when + QString::fromUtf8(" \xC2\xB7 ") + lastStr;
            }
        }
        if (run.startedAtMs > 0 && run.finishedAtMs >= run.startedAtMs)
            prepared.durationText =
                formatDuration(run.finishedAtMs - run.startedAtMs);
        signature += QStringLiteral(
                         "%1\x1f%2\x1f%3\x1f%4\x1f%5\x1f%6\x1f%7\x1f%8\x1f%9\x1f%10\x1e")
                         .arg(prepared.runId)
                         .arg(prepared.failed ? 1 : 0)
                         .arg(prepared.workflowName, prepared.detail,
                              prepared.iconName,
                              prepared.statusText, prepared.statusTooltip,
                              prepared.when, prepared.whenTooltip,
                              prepared.durationText);
        rows.append(prepared);
    }
    // The columns are the runs table's selection, so a run that is no longer
    // listed here — history cleared, or another repository opened — has no row
    // left to be selected and no business holding a pane.
    if (!m_parallelRunColumns.isEmpty()) {
        QSet<int> listed;
        for (const PreparedRow &prepared : rows)
            listed.insert(prepared.runId);
        QList<int> kept;
        for (int runId : parallelRunIds()) {
            if (listed.contains(runId))
                kept.append(runId);
        }
        if (kept.size() != m_parallelRunColumns.size())
            setParallelRunIds(kept);
    }
    for (int runId : openActionRunIds())
        signature += QStringLiteral("sel=%1\x1e").arg(runId);
    if (signature == m_actionsTableSignature) {
        // Both of these read state the rows do not carry (the workflow list, and
        // whether anything is still running), so they run either way.
        updateActionsSpinTimer();
        updateActionsTabIndicator();
        return;
    }
    m_actionsTableSignature = signature;

    QSignalBlocker block(m_actionsTable);
    TableRepaintGuard repaintGuard(m_actionsTable);
    m_actionsTable->setRowCount(0);
    for (const PreparedRow &prepared : rows) {
        const int row = m_actionsTable->rowCount();
        m_actionsTable->insertRow(row);

        auto *wfItem = new QTableWidgetItem(prepared.workflowName);
        wfItem->setData(Qt::UserRole, prepared.runId);
        wfItem->setData(ActionFailureBorderDelegate::ActionFailedRole,
                        prepared.failed);
        wfItem->setData(ActionFailureBorderDelegate::ActionRunDetailRole,
                        prepared.detail);
        if (!prepared.detail.isEmpty())
            wfItem->setToolTip(prepared.workflowName + QLatin1Char('\n') +
                               prepared.detail);
        wfItem->setIcon(themedOcticon(prepared.iconName, prepared.statusColor,
                                      kActionStatusIconPx));
        auto *statusItem = new QTableWidgetItem(prepared.statusText);
        statusItem->setForeground(prepared.statusColor);
        if (!prepared.statusTooltip.isEmpty())
            statusItem->setToolTip(prepared.statusTooltip);
        auto *whenItem = new QTableWidgetItem(prepared.when);
        if (!prepared.whenTooltip.isEmpty())
            whenItem->setToolTip(prepared.whenTooltip);
        auto *durationItem = new QTableWidgetItem(prepared.durationText);

        m_actionsTable->setItem(row, 0, wfItem);
        m_actionsTable->setItem(row, 1, statusItem);
        m_actionsTable->setItem(row, 2, whenItem);
        m_actionsTable->setItem(row, 3, durationItem);
    }
    selectActionRunRows(openActionRunIds());
    updateActionsSpinTimer();
    updateActionsTabIndicator();
}

void MainWindow::showLatestVisibleActionRun()
{
    if (!m_actionsTable || m_actionsTable->rowCount() == 0) {
        showRun(-1);
        return;
    }

    int targetRow = 0;
    int targetRunId = -1;
    for (int row = 0; row < m_actionsTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_actionsTable->item(row, 0);
        if (!item)
            continue;
        const int runId = item->data(Qt::UserRole).toInt();
        if (targetRunId < 0) {
            targetRow = row;
            targetRunId = runId;
        }
        const ActionRun *run = findRun(runId);
        if (run && run->status == ActionStatus::Running) {
            targetRow = row;
            targetRunId = runId;
            break;
        }
    }

    if (targetRunId < 0) {
        showRun(-1);
        return;
    }

    m_actionsTable->selectRow(targetRow);
    showRun(targetRunId);
}

int MainWindow::commitStatusCode(const QString &sha) const
{
    if (sha.isEmpty() || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return 0;
    const QString owner = m_repositories.at(m_repoDetailIndex).owner;
    const QString name = m_repositories.at(m_repoDetailIndex).name;

    bool running = false, failed = false, success = false;
    for (const ActionRun &run : m_actionRuns) {
        if (run.owner != owner || run.name != name || run.commit.isEmpty())
            continue;
        if (!(run.commit.startsWith(sha) || sha.startsWith(run.commit)))
            continue;
        if (run.status == ActionStatus::Running ||
            run.status == ActionStatus::Queued ||
            run.status == ActionStatus::AwaitingApproval)
            running = true;
        else if (run.status == ActionStatus::Failed ||
                 run.status == ActionStatus::Rejected)
            failed = true;
        else if (run.status == ActionStatus::Success)
            success = true;
    }
    if (running)
        return 3;
    if (failed)
        return 2;
    if (success)
        return 1;
    return 0;
}

MainWindow::CommitStatusIndex MainWindow::buildCommitStatusIndex() const
{
    CommitStatusIndex index;
    index.built = true;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return index;
    const QString owner = m_repositories.at(m_repoDetailIndex).owner;
    const QString name = m_repositories.at(m_repoDetailIndex).name;
    for (const ActionRun &run : m_actionRuns) {
        if (run.owner != owner || run.name != name || run.commit.isEmpty())
            continue;
        int flag = 0;
        if (run.status == ActionStatus::Running ||
            run.status == ActionStatus::Queued ||
            run.status == ActionStatus::AwaitingApproval)
            flag = 4;
        else if (run.status == ActionStatus::Failed ||
                 run.status == ActionStatus::Rejected)
            flag = 2;
        else if (run.status == ActionStatus::Success)
            flag = 1;
        if (!flag)
            continue;
        index.flagsByCommit[run.commit] |= flag;
        if (run.commit.size() < 40)
            index.abbreviated.append({run.commit, flag});
    }
    return index;
}

int MainWindow::commitStatusCode(const QString &sha,
                                 const CommitStatusIndex &index) const
{
    if (sha.isEmpty() || !index.built)
        return commitStatusCode(sha);
    int flags = index.flagsByCommit.value(sha, 0);
    // A run recorded with an abbreviated commit matches any sha it prefixes
    // (and a short table sha matches runs it prefixes) — same rule as the
    // full scan, over the usually-empty short list only.
    for (const auto &entry : index.abbreviated)
        if (entry.first != sha &&
            (sha.startsWith(entry.first) || entry.first.startsWith(sha)))
            flags |= entry.second;
    if (sha.size() < 40)
        for (auto it = index.flagsByCommit.cbegin(); it != index.flagsByCommit.cend();
             ++it)
            if (it.key().startsWith(sha))
                flags |= it.value();
    if (flags & 4)
        return 3;
    if (flags & 2)
        return 2;
    if (flags & 1)
        return 1;
    return 0;
}

QString MainWindow::commitStatusGlyph(const QString &sha) const
{
    switch (commitStatusCode(sha)) {
    case 3:
        return QString::fromUtf8(" <span style='color:#58a6ff' "
                              "title='Checks running'>\xE2\x97\x90</span>"); // ◐
    case 2:
        return QString::fromUtf8(" <span style='color:#f85149' "
                              "title='Checks failed'>\xE2\x9C\x95</span>"); // ✕
    case 1:
        return QString::fromUtf8(" <span style='color:#3fb950' "
                              "title='Checks passed'>\xE2\x9C\x93</span>"); // ✓
    default:
        return QString();
    }
}

void MainWindow::refreshCommitBarStatusGlyph()
{
    if (!m_commitBar || m_commitBarStatusHash.isEmpty() ||
        m_commitBarBodyHtml.isEmpty())
        return;
    m_commitBar->setText(m_commitBarBodyHtml +
                         commitStatusGlyph(m_commitBarStatusHash));
}

void MainWindow::refreshCommitTableStatusGlyphs()
{
    if (!m_commitsTable)
        return;
    // Runs on every run state transition. One index over the run history,
    // and hold the table's signals/repaints while every row's icon and
    // tooltip are rewritten — each setIcon/setToolTip used to emit
    // dataChanged and repaint the view on its own.
    const CommitStatusIndex index = buildCommitStatusIndex();
    QSignalBlocker block(m_commitsTable);
    TableRepaintGuard repaintGuard(m_commitsTable);
    for (int row = 0; row < m_commitsTable->rowCount(); ++row) {
        QTableWidgetItem *summary = m_commitsTable->item(row, kCommitSummaryCol);
        if (!summary || summary->data(kCommitRowKindRole).toInt() != 0)
            continue; // expanded file rows carry the same sha — commits only
        const QString sha = summary->data(Qt::UserRole).toString();
        if (sha.isEmpty())
            continue;
        const int code = commitStatusCode(sha, index);
        switch (code) {
        case 1:
            summary->setIcon(themedOcticon("check-circle", QColor("#3fb950"), 14));
            break;
        case 2:
            summary->setIcon(themedOcticon("x", QColor("#f85149"), 14));
            break;
        case 3:
            summary->setIcon(themedOcticon("sync", QColor("#58a6ff"), 14));
            break;
        default:
            summary->setIcon(QIcon());
            break;
        }
        updateCommitRowHover(row, code);
    }
}

void MainWindow::refreshCommitStatusGlyphs()
{
    if (!m_repoDetailStack)
        return;
    switch (m_repoDetailStack->currentIndex()) {
    case 0: // Code overview: refresh the latest-commit strip
        refreshCommitBarStatusGlyph();
        break;
    case 1: // Commits list
        refreshCommitTableStatusGlyphs();
        break;
    default:
        break;
    }
}

void MainWindow::updateActionsTabIndicator()
{
    QAbstractButton *tab = m_repoDetailTabs ? m_repoDetailTabs->button(6) : nullptr;
    if (!tab)
        return;

    if (auto *b = dynamic_cast<VerticalIconButton *>(tab))
        b->setBadgeCount(m_repoWorkflows.size());
}

qint64 MainWindow::estimatedRunDurationMs(const ActionRun &run) const
{
    qint64 best = 0;
    qint64 newest = 0;
    for (const ActionRun &r : m_actionRuns) {
        if (r.id == run.id || r.owner != run.owner || r.name != run.name ||
            r.workflowPath != run.workflowPath)
            continue;
        if (r.startedAtMs <= 0 || r.finishedAtMs <= r.startedAtMs)
            continue; // never actually ran to completion
        if (r.finishedAtMs > newest) {
            newest = r.finishedAtMs;
            best = r.finishedAtMs - r.startedAtMs;
        }
    }
    return best;
}

void MainWindow::updateAgentsTabIndicator()
{
    QString owner, name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name  = m_repositories.at(m_repoDetailIndex).name;
    }
    // Only running rows animate. Surfaced external (watch-only) Claude
    // sessions are injected into m_agentSessions with a live/idle status by
    // injectExternalSessions(), so they are covered by the scan below; a bare
    // "any external session detected" used to pin the 60 ms timer on for the
    // whole session even with nothing running.
    bool anyRunning = false;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.owner == owner && s.name == name && !s.merged &&
            s.status == AgentStatus::Running) {
            anyRunning = true;
            break;
        }
    }
    if (!anyRunning) {
        if (m_agentsSpinTimer)
            m_agentsSpinTimer->stop();
        return;
    }
    if (!m_agentsSpinTimer) {
        m_agentsSpinTimer = new QTimer(this);
        // Off-screen (another tab or section is up) there is nothing to
        // paint: idle the timer down instead of compositing a glyph per
        // running row 16 times a second into a table nobody sees. The slow
        // tick keeps checking, so the animation resumes by itself when the
        // table comes back. Direct callers (tests) still animate.
        connect(m_agentsSpinTimer, &QTimer::timeout, this, [this] {
            if (spinTimerTickAllowed(m_agentsSpinTimer, m_agentTable))
                animateRunningAgentIcons(); // refresh live row metadata
        });
    }
    if (!m_agentsSpinTimer->isActive())
        m_agentsSpinTimer->start(kAgentSpinTickMs);
}

void MainWindow::updateActionsSpinTimer()
{
    if (!m_actionsTable)
        return;
    bool anyRunning = false;
    for (int r = 0; r < m_actionsTable->rowCount(); ++r) {
        QTableWidgetItem *item = m_actionsTable->item(r, 0);
        if (!item)
            continue;
        const ActionRun *run = findRun(item->data(Qt::UserRole).toInt());
        if (run && run->status == ActionStatus::Running) {
            anyRunning = true;
            break;
        }
    }
    if (!anyRunning) {
        if (m_actionsSpinTimer)
            m_actionsSpinTimer->stop();
        return;
    }
    if (!m_actionsSpinTimer) {
        m_actionsSpinTimer = new QTimer(this);
        connect(m_actionsSpinTimer, &QTimer::timeout, this, [this] {
            if (spinTimerTickAllowed(m_actionsSpinTimer, m_actionsTable))
                animateRunningActionIcons(); // same off-screen idling as agents
        });
    }
    if (!m_actionsSpinTimer->isActive())
        m_actionsSpinTimer->start(kAgentSpinTickMs);
}

void MainWindow::animateRunningActionIcons()
{
    if (!m_actionsTable)
        return;
    ++m_actionSpinTicks;
    const qreal angle = qreal((m_actionSpinTicks * 11) % 360);
    QSignalBlocker block(m_actionsTable);
    for (int r = 0; r < m_actionsTable->rowCount(); ++r) {
        QTableWidgetItem *item = m_actionsTable->item(r, 0);
        if (!item)
            continue;
        const ActionRun *run = findRun(item->data(Qt::UserRole).toInt());
        if (!run || run->status != ActionStatus::Running)
            continue;
        const QPixmap spinning = rotatedTintedOcticonPixmap(
            actionRunStatusIconName(run->status), actionStatusColor(run->status),
            kActionStatusIconPx, angle);
        item->setIcon(QIcon(spinning));
        m_actionsTable->viewport()->update(m_actionsTable->visualItemRect(item));
    }
}


void MainWindow::persistLooperState()
{
    QSettings settings;
    settings.setValue(kLooperActiveSetting, m_looperActive);
    settings.setValue(kLooperProviderSetting, m_looperProvider);
    settings.setValue(kLooperRepoSetting, m_looperRepoSlug);
}

void MainWindow::maybeRestoreIssueLooper()
{
    if (m_looperActive)
        return; // already looping this session
    QSettings settings;
    if (!settings.value(kLooperActiveSetting, false).toBool())
        return;
    const QString slug = settings.value(kLooperRepoSetting).toString();
    const int idx = issuesRepoIndex(); // the repo the looper would actually drive
    if (slug.isEmpty() || idx < 0 || idx >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(idx);
    if (slug != repo.owner + QLatin1Char('/') + repo.name)
        return;
    if (repoAgentGitDir(repo).isEmpty())
        return;
    m_looperActive = true;
    m_looperProvider = settings.value(kLooperProviderSetting).toString();
    if (m_looperProvider.isEmpty())
        m_looperProvider = defaultAgentProvider();
    m_looperRepoSlug = slug;
    updateIssueLooperButton();
    setIssueInlineNotice(
        QString::fromUtf8("Resumed the issue looper with %1 after restart\xE2\x80\xA6")
            .arg(agentProviderName(m_looperProvider)));
    looperStartNext();
}

// code-health: allow(blocking-wait) reads the mirror's .forkmesh workflows with one `git ls-tree` plus one `git show` per .yml/.yaml file (a handful per repo), each capped at 8 s. reloadWorkflowCountInBackground runs it on a runOffThread worker; refreshRepoActions (opening the Actions tab) and buildRepoSecurityInput (Security/Quality refresh) call it synchronously on the GUI thread, and making those async is a larger change than this pass
static QList<ActionWorkflow> workflowsInRepoPaths(const QString &mirrorPath,
                                                  const QString &localPath)
{
    QList<ActionWorkflow> out;
    if (!mirrorPath.isEmpty() && QDir(mirrorPath).exists()) {
        QProcess ls;
        ls.start(QStringLiteral("git"),
                 {QStringLiteral("-C"), mirrorPath, QStringLiteral("ls-tree"),
                  QStringLiteral("-r"), QStringLiteral("--name-only"),
                  QStringLiteral("HEAD"), QStringLiteral("--"),
                  QStringLiteral(".forkmesh")});
        ls.waitForFinished(8000);
        const QStringList paths = QString::fromUtf8(ls.readAllStandardOutput())
                                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &path : paths) {
            if (!(path.endsWith(QLatin1String(".yml")) ||
                  path.endsWith(QLatin1String(".yaml"))))
                continue;
            QProcess show;
            show.start(QStringLiteral("git"),
                       {QStringLiteral("-C"), mirrorPath,
                        QStringLiteral("show"), QStringLiteral("HEAD:") + path});
            show.waitForFinished(8000);
            if (show.exitCode() != 0)
                continue;
            out.append(ActionFile::parse(
                path, QString::fromUtf8(show.readAllStandardOutput())));
        }
    }
    if (out.isEmpty() && !localPath.isEmpty())
        out = ActionFile::parseWorkflowsInDir(localPath);
    return out;
}

QList<ActionWorkflow>
MainWindow::availableWorkflowsForRepo(const RepositoryRecord &repo) const
{
    return workflowsInRepoPaths(repo.mirrorPath, repo.localPath);
}

void MainWindow::reloadWorkflowCountInBackground()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const int repoIndex = m_repoDetailIndex;
    const int generation = ++m_workflowCountLoadGen;
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    const QString mirrorPath = repo.mirrorPath;
    const QString localPath = repo.localPath;
    runOffThread<QList<ActionWorkflow>>(
        [mirrorPath, localPath] {
            return workflowsInRepoPaths(mirrorPath, localPath);
        },
        [this, repoIndex, generation](QList<ActionWorkflow> loaded) {
            if (generation != m_workflowCountLoadGen ||
                repoIndex != m_repoDetailIndex)
                return; // superseded, or the user moved to another repo
            m_repoWorkflows = std::move(loaded);
            updateActionsTabIndicator();
        });
}

void MainWindow::updateWorkflowListItem(QListWidgetItem *item,
                                        const ActionWorkflow &wf,
                                        const RepositoryRecord &repo)
{
    if (!item)
        return;
    QStringList triggers;
    if (wf.triggersOnPush())
        triggers << QStringLiteral("on: push");
    if (wf.triggersOnRelease())
        triggers << QStringLiteral("on: release");
    if (wf.allowsManualRun())
        triggers << QStringLiteral("manual");
    const QStringList runsOn = workflowRunsOnLabels(repo, wf);
    item->setText(wf.name);
    item->setForeground(palette().color(QPalette::Active, QPalette::Text));
    if (!runsOn.isEmpty()) {
        const QString where = runsOn.join(QStringLiteral(", "));
        triggers << QStringLiteral("runs-on: ") + where;
        if (!workflowRunsOnThisNode(repo, wf)) {
            item->setText(wf.name + QString::fromUtf8("  \xC2\xB7  ") + where);
            item->setForeground(
                palette().color(QPalette::Disabled, QPalette::Text));
        }
    }
    item->setToolTip(
        wf.valid ? wf.path +
                       (triggers.isEmpty()
                            ? QString()
                            : QStringLiteral("  (") +
                                  triggers.join(QStringLiteral(", ")) +
                                  QStringLiteral(")")) +
                       QStringLiteral("\nUntick to disable this workflow.")
                 : wf.path + QStringLiteral("  — ") + wf.error);
}

void MainWindow::refreshWorkflowProgress()
{
    if (!m_actionWorkflowList || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);

    auto latestRun = [this, &repo](const QString &path) -> const ActionRun * {
        for (const ActionRun &run : std::as_const(m_actionRuns)) {
            if (run.owner == repo.owner && run.name == repo.name &&
                run.workflowPath == path)
                return &run;
        }
        return nullptr;
    };
    auto stepStatus = [](const ActionWorkflow &workflow, const ActionRun *run,
                         int index) {
        if (!run)
            return ActionStepStatus::Pending;
        if (run->stepStatuses.size() == workflow.steps.size() &&
            index < run->stepStatuses.size())
            return run->stepStatuses.at(index);
        if (run->status == ActionStatus::Queued)
            return ActionStepStatus::Queued;
        if (run->status == ActionStatus::Success)
            return ActionStepStatus::Success;
        if (run->status == ActionStatus::Failed &&
            index == run->currentStepIndex)
            return ActionStepStatus::Failed;
        if (run->status == ActionStatus::Cancelled &&
            index == run->currentStepIndex)
            return ActionStepStatus::Cancelled;
        if (run->status == ActionStatus::Running) {
            if (index < run->currentStepIndex)
                return ActionStepStatus::Success;
            if (index == run->currentStepIndex)
                return ActionStepStatus::Running;
        }
        return ActionStepStatus::Pending;
    };

    const QStringList nodeLabels = actionNodeLabels();
    for (int row = 0; row < m_actionWorkflowList->count(); ++row) {
        QListWidgetItem *item = m_actionWorkflowList->item(row);
        if (!item)
            continue;
        const QString path = item->data(Qt::UserRole).toString();
        if (path.isEmpty())
            continue;
        const ActionWorkflow *workflow = nullptr;
        for (const ActionWorkflow &candidate : std::as_const(m_repoWorkflows)) {
            if (candidate.path == path) {
                workflow = &candidate;
                break;
            }
        }
        auto *area = dynamic_cast<ActionWorkflowArea *>(
            m_actionWorkflowList->itemWidget(item));
        if (!workflow || !area)
            continue;

        const ActionRun *run = latestRun(path);
        QVector<ActionWorkflowArea::Step> steps;
        steps.reserve(workflow->steps.size());
        for (int index = 0; index < workflow->steps.size(); ++index)
            steps.append({workflow->steps.at(index).name,
                          stepStatus(*workflow, run, index)});

        QStringList details;
        details << QStringLiteral("%1 step%2")
                       .arg(workflow->steps.size())
                       .arg(workflow->steps.size() == 1 ? QString() :
                                                           QStringLiteral("s"));
        if (run)
            details << actionStatusText(run->status);
        const QStringList runsOn = workflowRunsOnLabels(repo, *workflow);
        if (!runsOn.isEmpty())
            details << runsOn.join(QStringLiteral(", "));
        area->setData(workflow->name, workflow->path, details, steps,
                      !repo.disabledWorkflows.contains(workflow->path),
                      item->isSelected(),
                      workflowRunsOnThisNode(repo, *workflow, nodeLabels));
        item->setSizeHint(area->sizeHint());
    }
}

void MainWindow::refreshRepoActions()
{
    if (!m_actionWorkflowList)
        return;
    QSignalBlocker block(m_actionWorkflowList);
    m_actionWorkflowList->clear();

    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        m_repoWorkflows.clear();
        updateActionsTabIndicator();
        refreshActionsTable();
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);

    if (m_actionsEnabledCheck) {
        QSignalBlocker block(m_actionsEnabledCheck);
        m_actionsEnabledCheck->setChecked(repo.actionsEnabled);
    }
    if (m_actionsAutoApproveCheck) {
        QSignalBlocker block(m_actionsAutoApproveCheck);
        m_actionsAutoApproveCheck->setChecked(repo.actionsAutoApprove);
    }

    auto *all = new QListWidgetItem(QStringLiteral("All workflows"));
    all->setData(Qt::UserRole, QString());
    m_actionWorkflowList->addItem(all);
    all->setSelected(true);

    const QList<ActionWorkflow> wfs = availableWorkflowsForRepo(repo);
    m_repoWorkflows = wfs; // cache so the manual-run bar can look workflows up
    for (const ActionWorkflow &wf : wfs) {
        auto *item = new QListWidgetItem;
        item->setData(Qt::UserRole, wf.path);
        updateWorkflowListItem(item, wf, repo);
        m_actionWorkflowList->addItem(item);
        auto *area = new ActionWorkflowArea(m_actionWorkflowList);
        area->onClicked = [this, item] {
            if (m_actionWorkflowList)
                m_actionWorkflowList->setCurrentItem(item);
        };
        area->onEnabledChanged = [this, path = wf.path](bool enabled) {
            setWorkflowDisabled(path, !enabled);
            refreshWorkflowProgress();
        };
        m_actionWorkflowList->setItemWidget(item, area);
    }
    if (wfs.isEmpty()) {
        auto *none = new QListWidgetItem(
            repo.actionsEnabled
                ? QStringLiteral("No workflows in .forkmesh/")
                : QStringLiteral("No workflows in .forkmesh/ (actions disabled)"));
        none->setFlags(Qt::NoItemFlags);
        m_actionWorkflowList->addItem(none);
    }

    m_selectedWorkflowFilter.clear();
    refreshWorkflowProgress();
    refreshActionsTable();
    showLatestVisibleActionRun();
    updateManualRunBar();
    refreshWorkflowNodeCombo();
    ++m_workflowCountLoadGen;
    updateActionsTabIndicator();
}

void MainWindow::refreshWorkflowNodeCombo()
{
    if (!m_actionNodeCombo)
        return;
    QSignalBlocker block(m_actionNodeCombo);
    m_actionNodeCombo->clear();
    const bool haveRepo = m_repoDetailIndex >= 0 &&
                          m_repoDetailIndex < m_repositories.size();
    m_actionNodeCombo->setEnabled(haveRepo);
    if (m_actionNodeLabel)
        m_actionNodeLabel->setEnabled(haveRepo);
    if (!haveRepo) {
        m_actionNodeCombo->addItem(QStringLiteral("Any node"), QString());
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);

    const ActionWorkflow *selected = nullptr;
    for (const ActionWorkflow &wf : std::as_const(m_repoWorkflows)) {
        if (wf.valid && wf.path == m_selectedWorkflowFilter) {
            selected = &wf;
            break;
        }
    }
    if (m_actionNodeLabel) {
        const QString caption =
            selected ? QString::fromUtf8("Run \xE2\x80\x9C%1\xE2\x80\x9D on")
                           .arg(QFontMetrics(m_actionNodeLabel->font())
                                    .elidedText(selected->name, Qt::ElideRight,
                                                140))
                     : QStringLiteral("Run every workflow on");
        m_actionNodeLabel->setText(caption);
        m_actionNodeLabel->setToolTip(
            selected ? QString::fromUtf8("Run \xE2\x80\x9C%1\xE2\x80\x9D on")
                           .arg(selected->name)
                     : QString());
    }

    QString pin;
    if (selected) {
        pin = workflowNodePin(repo, selected->path);
    } else {
        bool first = true;
        for (const ActionWorkflow &wf : std::as_const(m_repoWorkflows)) {
            if (!wf.valid)
                continue;
            const QString wfPin = workflowNodePin(repo, wf.path);
            if (first) {
                pin = wfPin;
                first = false;
            } else if (wfPin != pin) {
                pin.clear();
                break;
            }
        }
    }

    const QStringList declared = selected ? selected->runsOn : QStringList();
    m_actionNodeCombo->addItem(
        declared.isEmpty()
            ? QStringLiteral("Any node")
            : QStringLiteral("Workflow default (%1)")
                  .arg(declared.join(QStringLiteral(", "))),
        QString());
    const QString self = machineNodeName().trimmed();
    for (const QString &node : actionNodeCandidates()) {
        const bool isSelf = node.compare(self, Qt::CaseInsensitive) == 0;
        m_actionNodeCombo->addItem(
            isSelf ? QStringLiteral("%1 (this node)").arg(node) : node,
            node.toLower());
    }
    if (!pin.isEmpty() && m_actionNodeCombo->findData(pin) < 0)
        m_actionNodeCombo->addItem(
            QStringLiteral("%1 (offline)").arg(pin), pin);
    m_actionNodeCombo->setCurrentIndex(
        pin.isEmpty() ? 0 : qMax(0, m_actionNodeCombo->findData(pin)));
    m_actionNodeCombo->setToolTip(QStringLiteral(
        "Which node runs these actions. Picking a node here overrides the "
        "workflow's own runs-on: line for this repo, and stops this node "
        "queueing the workflow unless it is that node."));
}

void MainWindow::setWorkflowNode(const QString &node)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const QString label = node.trimmed().toLower();
    QStringList targets;
    if (m_selectedWorkflowFilter.isEmpty()) {
        for (const ActionWorkflow &wf : std::as_const(m_repoWorkflows))
            if (wf.valid)
                targets.append(wf.path);
    } else {
        targets.append(m_selectedWorkflowFilter);
    }
    if (targets.isEmpty())
        return;

    RepositoryRecord &repo = m_repositories[m_repoDetailIndex];
    const QStringList encoded =
        ActionFile::setPinnedNode(repo.workflowNodes, targets, label);
    if (encoded == repo.workflowNodes)
        return;
    repo.workflowNodes = encoded;
    saveRepositories();
    logSystem(QStringLiteral("Actions: %1 now %2 for %3/%4.")
                  .arg(m_selectedWorkflowFilter.isEmpty()
                           ? QStringLiteral("every workflow")
                           : m_selectedWorkflowFilter,
                       label.isEmpty()
                           ? QStringLiteral("runs wherever the workflow says")
                           : QStringLiteral("runs on ") + label,
                       repo.owner, repo.name));

    if (m_actionWorkflowList) {
        QSignalBlocker block(m_actionWorkflowList);
        const RepositoryRecord &saved = m_repositories.at(m_repoDetailIndex);
        for (int row = 0; row < m_actionWorkflowList->count(); ++row) {
            QListWidgetItem *item = m_actionWorkflowList->item(row);
            const QString path = item->data(Qt::UserRole).toString();
            if (path.isEmpty())
                continue;
            for (const ActionWorkflow &wf : std::as_const(m_repoWorkflows)) {
                if (wf.path != path)
                    continue;
                updateWorkflowListItem(item, wf, saved);
                break;
            }
        }
    }
    refreshWorkflowProgress();
    updateManualRunBar();
    refreshWorkflowNodeCombo();
}

// code-health: allow(blocking-wait) the for-each-ref wait is inside the runOffThread branch read
void MainWindow::updateManualRunBar()
{
    if (!m_actionManualRunBar)
        return;
    const ActionWorkflow *wf = nullptr;
    for (const ActionWorkflow &w : std::as_const(m_repoWorkflows)) {
        if (w.valid && w.path == m_selectedWorkflowFilter) {
            wf = &w;
            break;
        }
    }
    const bool haveRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    const bool show = haveRepo && wf && wf->allowsManualRun() &&
                      !isWorkflowDisabled(wf->path);
    m_actionManualRunBar->setVisible(show);
    if (!show)
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    m_actionManualRunButton->setText(
        QString::fromUtf8("Run \xE2\x80\x9C%1\xE2\x80\x9D").arg(wf->name));
    const bool ours = workflowRunsOnThisNode(repo, *wf);
    m_actionManualRunButton->setEnabled(ours);
    m_actionManualRunButton->setToolTip(
        ours ? QString()
             : QString::fromUtf8("Dedicated to %1 \xE2\x80\x94 start this "
                                 "workflow from that node.")
                   .arg(workflowDedicationLabel(repo, *wf)));

    // The branch list is read on a worker (this runs on every workflow
    // selection) and applied only if the same repository is still open.
    const QString mirrorPath = repo.mirrorPath;
    runOffThread<QStringList>(
        [mirrorPath]() -> QStringList {
            if (mirrorPath.isEmpty() || !QDir(mirrorPath).exists())
                return {};
            QProcess refs;
            refs.start(QStringLiteral("git"),
                       {QStringLiteral("-C"), mirrorPath,
                        QStringLiteral("for-each-ref"),
                        QStringLiteral("--format=%(refname:short)"),
                        QStringLiteral("refs/heads")});
            refs.waitForFinished(8000);
            return QString::fromUtf8(refs.readAllStandardOutput())
                       .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        },
        [this, mirrorPath](QStringList branches) {
            if (!m_actionRunBranchCombo || m_repoDetailIndex < 0 ||
                m_repoDetailIndex >= m_repositories.size() ||
                m_repositories.at(m_repoDetailIndex).mirrorPath != mirrorPath)
                return;
            const QString previous =
                m_actionRunBranchCombo->currentText().trimmed();
            QSignalBlocker block(m_actionRunBranchCombo);
            m_actionRunBranchCombo->clear();
            m_actionRunBranchCombo->addItems(branches);
            const QString want =
                previous.isEmpty() ? QStringLiteral("main") : previous;
            int idx = m_actionRunBranchCombo->findText(want);
            if (idx < 0 && want != QStringLiteral("main"))
                idx = m_actionRunBranchCombo->findText(QStringLiteral("main"));
            if (idx >= 0)
                m_actionRunBranchCombo->setCurrentIndex(idx);
            else if (m_actionRunBranchCombo->count() > 0)
                m_actionRunBranchCombo->setCurrentIndex(0);
            else
                m_actionRunBranchCombo->setEditText(QStringLiteral("main"));
        });
}

void MainWindow::runSelectedWorkflowManually()
{
    if (m_actionRunsRestoreInFlight) {
        flashMessage(QStringLiteral(
            "Actions history is still loading. Try again in a moment."));
        return;
    }
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const QString path = m_selectedWorkflowFilter;
    if (path.isEmpty() || m_repositories.at(m_repoDetailIndex).mirrorPath.isEmpty())
        return;
    const QString branch = m_actionRunBranchCombo->currentText().trimmed();
    if (branch.isEmpty()) {
        flashMessage(QStringLiteral("Choose a branch to run on."));
        return;
    }
    startManualWorkflowRun(m_repoDetailIndex, path, branch,
                           [this](int runId, const QString &error) {
                               if (runId > 0)
                                   showRun(runId);
                               else if (!error.isEmpty())
                                   flashMessage(error);
                           });
}

// code-health: allow(blocking-wait) the git waits are inside the runOffThread workflow read
void MainWindow::startManualWorkflowRun(
    int repoIndex, const QString &path, const QString &branch,
    std::function<void(int runId, const QString &error)> done)
{
    const QString loading =
        QStringLiteral("Actions history is still loading. Try again in a moment.");
    if (m_actionRunsRestoreInFlight) {
        done(0, loading);
        return;
    }
    if (repoIndex < 0 || repoIndex >= m_repositories.size()) {
        done(0, QStringLiteral("This repository is not on this desktop."));
        return;
    }
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    if (path.isEmpty() || repo.mirrorPath.isEmpty()) {
        done(0, QStringLiteral("This repository has no mirror to run Actions from."));
        return;
    }

    // The rev-parse, `git show` and repository-state binding run on a worker;
    // the checks that read GUI state and the run creation happen once it is
    // back, provided the same repository is still open.
    struct ManualWorkflowRead {
        QString commit;
        bool showOk = false;
        QString content;
        ActionWorkflow workflow;
        ActionRun run;
        bool bound = false;
        QString snapshotError;
    };
    const QString mirrorPath = repo.mirrorPath;
    const QString owner = repo.owner;
    const QString name = repo.name;
    runOffThread<ManualWorkflowRead>(
        [mirrorPath, owner, name, path, branch]() -> ManualWorkflowRead {
            ManualWorkflowRead read;
            auto revParse = [&mirrorPath](const QString &rev) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        {QStringLiteral("-C"), mirrorPath,
                         QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                         QStringLiteral("%1^{commit}").arg(rev)});
                p.waitForFinished(8000);
                return p.exitCode() == 0
                           ? QString::fromUtf8(p.readAllStandardOutput()).trimmed()
                           : QString();
            };
            read.commit = revParse(branch);
            if (read.commit.isEmpty())
                read.commit = revParse(QStringLiteral("refs/heads/") + branch);
            if (read.commit.isEmpty())
                return read;

            QProcess show;
            show.start(QStringLiteral("git"),
                       {QStringLiteral("-C"), mirrorPath, QStringLiteral("show"),
                        read.commit + QLatin1Char(':') + path});
            show.waitForFinished(10000);
            if (show.exitCode() != 0)
                return read;
            read.showOk = true;
            read.content = QString::fromUtf8(show.readAllStandardOutput());
            read.workflow = ActionFile::parse(path, read.content);
            if (!read.workflow.valid)
                return read;

            read.run.owner = owner;
            read.run.name = name;
            read.run.workflowPath = path;
            read.run.workflowName = read.workflow.name;
            read.run.workflowContent = read.content;
            read.run.commit = read.commit;
            read.run.ref = QStringLiteral("refs/heads/") + branch;
            read.bound = bindActionRepositoryState(&read.run, mirrorPath,
                                                   &read.snapshotError);
            return read;
        },
        [this, repoIndex, owner, name, path, branch, loading,
         done](ManualWorkflowRead read) {
            if (m_actionRunsRestoreInFlight) {
                done(0, loading);
                return;
            }
            if (!m_actionStore || repoIndex < 0 ||
                repoIndex >= m_repositories.size() ||
                m_repositories.at(repoIndex).owner != owner ||
                m_repositories.at(repoIndex).name != name) {
                done(0, QStringLiteral("The repository changed while the run was prepared."));
                return;
            }
            const RepositoryRecord repo = m_repositories.at(repoIndex);
            const QString &commit = read.commit;
            if (commit.isEmpty()) {
                done(0,
                    QStringLiteral("Branch not found in this repo's mirror: %1")
                        .arg(branch));
                return;
            }
            if (!read.showOk) {
                done(0,
                    QString::fromUtf8(
                        "\xE2\x80\x9C%1\xE2\x80\x9D doesn't exist on %2.")
                        .arg(path, branch));
                return;
            }
            const ActionWorkflow &wf = read.workflow;
            if (!wf.valid) {
                done(0,
                    QStringLiteral("Workflow is invalid: %1").arg(wf.error));
                return;
            }
            if (!workflowRunsOnThisNode(repo, wf)) {
                done(0, QString::fromUtf8(
                                 "\xE2\x80\x9C%1\xE2\x80\x9D runs on %2 "
                                 "\xE2\x80\x94 start it from that node.")
                                 .arg(wf.name, workflowDedicationLabel(repo, wf)));
                return;
            }
            if (!read.bound) {
                done(0, QStringLiteral(
                                 "Could not bind this run to the repository "
                                 "state: %1")
                                 .arg(read.snapshotError));
                return;
            }
            ActionRun run = read.run;
            const bool approved = resolveActionApproval(repo, run);
            run.status =
                approved ? ActionStatus::Queued : ActionStatus::AwaitingApproval;

            const ActionRun created = m_actionStore->createRun(run);
            m_actionRuns.prepend(created);
            scheduleMirrorActionsSummary(0);
            cancelSupersededRuns(created);
            if (approved)
                m_actionQueue.append(created.id);
            logSystem(
                QStringLiteral("Actions: manual %1 \"%2\" for %3/%4 on %5 @ %6")
                    .arg(approved ? QStringLiteral("run of")
                                  : QStringLiteral("run awaiting approval of"),
                         wf.name, repo.owner, repo.name, branch,
                         commit.left(8)));

            refreshActionsTable();
            updateNotificationButton();
            processActionQueue();
            ringLocalControlRepos();
            done(created.id, QString());
        });
}

static QString unifiedWorkflowDiff(const QString &prior, const QString &incoming,
                                   const QString &path)
{
    QTemporaryDir temp;
    if (!temp.isValid())
        return QString();

    const bool isNew = prior.isEmpty();
    const QString oldFile = temp.path() + QStringLiteral("/old");
    const QString newFile = temp.path() + QStringLiteral("/new");
    if (!isNew) {
        QFile f(oldFile);
        if (f.open(QIODevice::WriteOnly))
            f.write(prior.toUtf8());
    }
    {
        QFile f(newFile);
        if (f.open(QIODevice::WriteOnly))
            f.write(incoming.toUtf8());
    }

    const QByteArray out = gitCaptureStdout(
        temp.path(),
        {"diff", "--no-index", "--",
         isNew ? QStringLiteral("/dev/null") : oldFile, newFile});

    const QString shown = path.isEmpty() ? QStringLiteral("workflow") : path;
    QStringList lines = QString::fromUtf8(out).split(QLatin1Char('\n'));
    for (QString &line : lines) {
        if (line.startsWith(QLatin1String("diff --git ")))
            line = QStringLiteral("diff --git a/%1 b/%1").arg(shown);
        else if (line.startsWith(QLatin1String("--- ")))
            line = line.startsWith(QLatin1String("--- /dev/null"))
                       ? QStringLiteral("--- /dev/null")
                       : QStringLiteral("--- a/%1").arg(shown);
        else if (line.startsWith(QLatin1String("+++ ")))
            line = QStringLiteral("+++ b/%1").arg(shown);
    }
    return lines.join(QLatin1Char('\n'));
}

void MainWindow::showRun(int runId)
{
    m_selectedRunId = runId;
    // A run promoted into the primary pane must not also linger as a column,
    // and with no run selected at all there is nothing to watch beside.
    if (!m_parallelRunColumns.isEmpty() &&
        (runId < 0 || parallelRunIds().contains(runId)))
        setParallelRunIds(runId < 0 ? QList<int>() : parallelRunIds());
    const ActionRun *run = findRun(runId);
    if (m_actionRerunButton)
        m_actionRerunButton->setVisible(run != nullptr);
    if (m_actionCopyLogButton)
        m_actionCopyLogButton->setVisible(run != nullptr);
    if (m_actionCopyLogPathButton)
        m_actionCopyLogPathButton->setVisible(run != nullptr);
    if (m_actionStopButton)
        m_actionStopButton->setVisible(run != nullptr &&
                                       (run->status == ActionStatus::Running ||
                                        run->status == ActionStatus::Queued));
    if (m_actionSkipButton)
        m_actionSkipButton->setVisible(run != nullptr &&
                                       (run->status == ActionStatus::Queued ||
                                        run->status == ActionStatus::AwaitingApproval));
    const bool fixable = run != nullptr && run->status == ActionStatus::Failed;
    if (m_actionFixButton)
        m_actionFixButton->setVisible(fixable);
    // Sending a run to an agent is a fix-it action, so it rides with the same
    // gate: a run that did not fail has nothing to hand over.
    if (m_actionSendToAgentButton)
        m_actionSendToAgentButton->setVisible(fixable);
    if (m_actionFixAgentCombo)
        m_actionFixAgentCombo->setVisible(false);
    if (m_actionFixModelCombo)
        m_actionFixModelCombo->setVisible(false);
    if (m_actionRunSpinner)
        m_actionRunSpinner->setVisible(run != nullptr &&
                                       run->status == ActionStatus::Running);
    if (!run) {
        if (m_actionRunTitle)
            m_actionRunTitle->setText(QStringLiteral("Select a run"));
        if (m_actionRunMeta)
            m_actionRunMeta->clear();
        if (m_actionRunLogPath)
            m_actionRunLogPath->hide();
        if (m_actionLog) {
            m_actionLog->clear();
            m_actionLog->setPlaceholderText(QString());
        }
        if (m_actionApprovalBar)
            m_actionApprovalBar->hide();
        if (m_actionApprovalBanner)
            m_actionApprovalBanner->hide();
        if (m_actionDiff)
            m_actionDiff->hide();
        return;
    }

    if (m_actionRunTitle)
        m_actionRunTitle->setText(run->workflowName);
    if (m_actionRunMeta)
        m_actionRunMeta->setText(actionRunMetaText(*run));

    const bool pending = run->status == ActionStatus::AwaitingApproval;
    if (m_actionRunLogPath) {
        const QString logFile = actionRunLogLocation(*run);
        m_actionRunLogPath->setFullText(
            logFile.isEmpty() ? QString()
                              : QStringLiteral("Log: %1").arg(logFile));
        // An approval is showing the workflow diff, not a log, so the path to a
        // file the run has not opened yet would only be noise there.
        m_actionRunLogPath->setVisible(!pending && !logFile.isEmpty());
    }
    const QString prior =
        pending ? ActionStore::lastApprovedContent(run->repoKey(), run->workflowPath)
                : QString();
    const bool workflowUnchanged =
        !prior.isEmpty() && prior == run->workflowContent;
    const bool heldForContributor =
        run->firstTimePullAuthor && run->pullNumber > 0;
    if (pending && m_actionApprovalBanner) {
        QString reason;
        if (heldForContributor) {
            reason = QStringLiteral(
                         "Held for your review: pull request #%1 is from %2, "
                         "who has no merged work in this repository yet, so "
                         "their code runs on this node only after you approve it.")
                         .arg(run->pullNumber)
                         .arg(run->pullAuthor.isEmpty()
                                  ? QStringLiteral("a first-time contributor")
                                  : run->pullAuthor);
        } else if (prior.isEmpty()) {
            reason = QStringLiteral("This workflow has never been approved here.");
        } else if (!workflowUnchanged) {
            reason = QStringLiteral(
                "This workflow changed since it was last approved.");
        } else {
            const int repoIndex = repoIndexFor(run->owner, run->name);
            const bool autoApprove =
                repoIndex >= 0 && m_repositories.at(repoIndex).actionsAutoApprove;
            reason = autoApprove
                         ? QStringLiteral(
                               "The workflow is unchanged, but the repository's "
                               "files changed since the last approval.")
                         : QStringLiteral(
                               "The workflow is unchanged, but the repository's "
                               "files changed since the last approval and "
                               "automatic approval is off for this repository.");
        }
        const ActionWorkflow workflow =
            ActionFile::parse(run->workflowPath, run->workflowContent);
        QStringList graph;
        for (int index = 0; index < workflow.steps.size(); ++index) {
            const QString name = workflow.steps.at(index).name.trimmed();
            graph.append(QStringLiteral("%1. %2")
                             .arg(index + 1)
                             .arg(name.isEmpty()
                                      ? QStringLiteral("shell step")
                                      : name));
        }
        m_actionApprovalBanner->setText(
            QStringLiteral(
                "%6\n\n"
                "Review the complete execution graph before approving. "
                "Approval covers this exact repository state and is invalidated "
                "by any tracked-file change.\n\n"
                "Commit: %1\nTree: %2\nState SHA-256: %3\nWorkflow: %4\n"
                "Steps:\n%5\n\n"
                "The workflow diff below contains the exact shell commands. "
                "Secrets are exposed only after this bound approval.")
                .arg(run->commit, run->repositoryTree,
                     run->executionDigest, run->workflowPath,
                     graph.join(QLatin1Char('\n')), reason));
    }
    if (m_actionApprovalBanner)
        m_actionApprovalBanner->setVisible(pending);
    if (m_actionApprovalBar)
        m_actionApprovalBar->setVisible(pending);
    if (m_actionDiff)
        m_actionDiff->setVisible(pending);
    if (m_actionLog)
        m_actionLog->setVisible(!pending);

    if (pending && m_actionSplitButton) {
        m_actionSplitButton->setChecked(diffSplitPref());
        updateDiffSplitButton(m_actionSplitButton);
    }
    if (pending && m_actionDiff) {
        const QString patch =
            unifiedWorkflowDiff(prior, run->workflowContent, run->workflowPath);
        QList<DiffFileEntry> files;
        QString html = renderDiffHtml(patch, files, QString(), QString(), QString());
        if (html.isEmpty())
            html = prior.isEmpty()
                       ? QStringLiteral(
                             "<p style='color:#8b949e'>This workflow has never "
                             "been approved.</p>")
                   : heldForContributor
                       ? QStringLiteral(
                             "<p style='color:#8b949e'>The workflow is "
                             "unchanged from the approved one. What needs "
                             "review is the code in pull request #%1: read its "
                             "changes in the pull request before approving, "
                             "because this workflow runs it.</p>")
                             .arg(run->pullNumber)
                       : QStringLiteral(
                             "<p style='color:#8b949e'>The workflow is "
                             "unchanged from the approved one. What is new is "
                             "the repository's files at commit %1.</p>")
                             .arg(run->commit.left(8).toHtmlEscaped());
        setDiffHtml(m_actionDiff, html);
    } else if (m_actionLog) {
        m_actionLog->setPlaceholderText(actionRunLogPlaceholder(*run));
        m_actionLog->setPlainText(displaySafePlainLog(m_actionStore->readLog(*run)));
        m_actionLog->moveCursor(QTextCursor::End);
    }
}

// ---- Watching several runs at once ----------------------------------------
//
// A fan-out push queues one run per workflow, and following them one click at a
// time means never seeing the one that is about to fail. The runs table selects
// a set instead: the current row keeps the full detail pane on the left, and
// every other selected run gets a column of its own to the right of it, each
// streaming its log live.

QList<int> MainWindow::parallelRunIds() const
{
    QList<int> ids;
    ids.reserve(m_parallelRunColumns.size());
    for (const ParallelRunColumn &column : m_parallelRunColumns)
        ids.append(column.runId);
    return ids;
}

QList<int> MainWindow::openActionRunIds() const
{
    QList<int> ids;
    if (m_selectedRunId >= 0)
        ids.append(m_selectedRunId);
    ids.append(parallelRunIds());
    return ids;
}

QPlainTextEdit *MainWindow::parallelRunLog(int runId) const
{
    for (const ParallelRunColumn &column : m_parallelRunColumns) {
        if (column.runId == runId)
            return column.log;
    }
    return nullptr;
}

MainWindow::ParallelRunColumn MainWindow::makeParallelRunColumn(int runId)
{
    ParallelRunColumn column;
    column.runId = runId;
    column.pane = new QWidget;
    column.pane->setMinimumWidth(220);

    column.title = new ElidingStatusLabel;
    column.title->setObjectName("channelTitle");
    column.spinner = new BusySpinner(nullptr, 16);
    column.spinner->setToolTip(QStringLiteral("Running"));
    column.spinner->hide();
    column.meta = new ElidingStatusLabel;
    column.meta->setObjectName("statusLine");
    // Each watched run names its own log file, the same way the primary pane
    // does: with several streaming side by side, "the log" is ambiguous unless
    // every column says which one it is.
    column.logPath = new ElidingStatusLabel;
    column.logPath->setObjectName("statusLine");
    column.logPath->setElideMode(Qt::ElideMiddle);
    column.logPath->hide();

    auto *closeButton = new QPushButton;
    closeButton->setObjectName("ghostButton");
    closeButton->setProperty("buttonSize", "sm");
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setToolTip("Stop watching this run here");
    setOcticon(closeButton, "x", 14);
    connect(closeButton, &QPushButton::clicked, this,
            [this, runId] { closeParallelRun(runId); });

    // The same controls as the primary pane, acting on this column's run. The
    // copy and agent buttons go icon-only: a column is narrow, and the labelled
    // ones already elide there.
    auto makeButton = [this](const char *objectName, const QString &text,
                             const char *icon, const QString &tip) {
        auto *button = new QPushButton(text);
        button->setObjectName(QLatin1String(objectName));
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
        setOcticon(button, icon, 16);
        button->hide();
        return button;
    };
    column.stopButton = makeButton("dangerButton", QStringLiteral("Stop"),
                                   "stop", QStringLiteral("Stop this run"));
    column.skipButton =
        makeButton("ghostButton", QStringLiteral("Skip"), "circle-slash",
                   QStringLiteral("Skip this run without executing it"));
    column.copyLogButton =
        makeButton("ghostButton", QString(), "copy",
                   QStringLiteral("Copy this run's full log to the clipboard"));
    column.copyLogPathButton = makeButton(
        "ghostButton", QString(), "file-directory",
        QStringLiteral("Copy the path of this run's log file on this machine"));
    column.sendToAgentButton = makeButton(
        "ghostButton", QString(), "paper-airplane",
        QStringLiteral("Send to agent: start an agent on this run with its "
                       "log's path and the ask to fix it"));
    column.rerunButton =
        makeButton("ghostButton", QStringLiteral("Rerun"), "sync",
                   QStringLiteral("Run this action again with the same commit"));
    connect(column.stopButton, &QPushButton::clicked, this,
            [this, runId] { stopActionRun(runId); });
    connect(column.skipButton, &QPushButton::clicked, this,
            [this, runId] { skipActionRun(runId); });
    connect(column.copyLogButton, &QPushButton::clicked, this,
            [this, runId] { copyActionRunLog(runId); });
    connect(column.copyLogPathButton, &QPushButton::clicked, this,
            [this, runId] { copyActionRunLogPath(runId); });
    connect(column.sendToAgentButton, &QPushButton::clicked, this,
            [this, runId] {
                sendActionRunToAgent(runId, defaultAgentProvider(), QString());
            });
    connect(column.rerunButton, &QPushButton::clicked, this,
            [this, runId] { rerunActionRun(runId); });

    column.log = new QPlainTextEdit;
    column.log->setReadOnly(true);
    column.log->setObjectName("actionLog");
    applyLogFont(column.log);
    column.log->setLineWrapMode(QPlainTextEdit::NoWrap);
    new AgentLogHighlighter(column.log->document());
    column.log->setMaximumBlockCount(20000);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleRow->addWidget(column.spinner);
    titleRow->addWidget(column.title, 1);
    titleRow->addWidget(closeButton);

    auto *controlRow = new QHBoxLayout;
    controlRow->setContentsMargins(0, 0, 0, 0);
    controlRow->setSpacing(4);
    controlRow->addWidget(column.stopButton);
    controlRow->addWidget(column.skipButton);
    controlRow->addWidget(column.copyLogButton);
    controlRow->addWidget(column.copyLogPathButton);
    controlRow->addWidget(column.sendToAgentButton);
    controlRow->addStretch();
    controlRow->addWidget(column.rerunButton);

    auto *layout = new QVBoxLayout(column.pane);
    layout->setContentsMargins(12, 22, 12, 22);
    layout->setSpacing(8);
    layout->addLayout(titleRow);
    layout->addLayout(controlRow);
    layout->addWidget(column.meta);
    layout->addWidget(column.logPath);
    layout->addWidget(column.log, 1);
    return column;
}

void MainWindow::setParallelRunIds(QList<int> runIds)
{
    if (!m_actionDetailSplitter)
        return;

    QList<int> wanted;
    int dropped = 0;
    for (int runId : runIds) {
        // The primary pane already shows m_selectedRunId.
        if (runId < 0 || runId == m_selectedRunId || wanted.contains(runId))
            continue;
        // Counted before the history lookup: Ctrl+A over a long list would
        // otherwise scan every run in it once per selected row.
        if (wanted.size() >= kMaxParallelRunColumns) {
            ++dropped;
            continue;
        }
        // A run with no record left — history cleared, another repository —
        // has nothing to stream.
        if (!findRun(runId))
            continue;
        wanted.append(runId);
    }
    // Say once that the cap bit, not on every step of a drag across the table.
    if (dropped > 0 && m_parallelRunsDropped == 0)
        showPageNotice(
            QStringLiteral("Watching %1 runs at once is the most that stays "
                           "readable, so %2 more stayed closed.")
                .arg(wanted.size() + 1)
                .arg(dropped),
            false);
    m_parallelRunsDropped = dropped;
    if (wanted == parallelRunIds())
        return;

    for (int index = m_parallelRunColumns.size() - 1; index >= 0; --index) {
        if (wanted.contains(m_parallelRunColumns.at(index).runId))
            continue;
        // Reparenting drops the splitter slot at once; the destruction waits
        // for the event loop because the close button that asks for a column to
        // go lives inside the pane and is still emitting clicked().
        QWidget *pane = m_parallelRunColumns.at(index).pane;
        pane->setParent(nullptr); // also hides it, so no stray top-level window
        pane->deleteLater();
        m_parallelRunColumns.removeAt(index);
    }

    for (int index = 0; index < wanted.size(); ++index) {
        const int runId = wanted.at(index);
        int existing = -1;
        for (int probe = 0; probe < m_parallelRunColumns.size(); ++probe) {
            if (m_parallelRunColumns.at(probe).runId == runId) {
                existing = probe;
                break;
            }
        }
        const bool fresh = existing < 0;
        if (fresh)
            m_parallelRunColumns.insert(index, makeParallelRunColumn(runId));
        else if (existing != index)
            m_parallelRunColumns.move(existing, index);
        // Slot 0 is the primary detail pane, so the columns start at 1.
        m_actionDetailSplitter->insertWidget(index + 1,
                                             m_parallelRunColumns.at(index).pane);
        // Only a brand new column reads the log back off disk. Re-reading one
        // that is already streaming would drop whatever the runner has emitted
        // since its last flush.
        refreshParallelRunColumn(runId, fresh);
    }

    // Share the detail area evenly: a column pinned to its size hint would
    // squeeze the others out as soon as a long log line arrived.
    const int panes = m_actionDetailSplitter->count();
    for (int index = 0; index < panes; ++index)
        m_actionDetailSplitter->setStretchFactor(index, 1);
    const int width = qMax(panes, m_actionDetailSplitter->width());
    m_actionDetailSplitter->setSizes(QList<int>(panes, width / panes));
}

void MainWindow::closeParallelRun(int runId)
{
    QList<int> remaining = parallelRunIds();
    if (!remaining.removeAll(runId))
        return;
    setParallelRunIds(remaining);
    selectActionRunRows(openActionRunIds()); // the table selection is the set
}

void MainWindow::refreshParallelRunColumn(int runId, bool reloadLog)
{
    for (const ParallelRunColumn &column : m_parallelRunColumns) {
        if (column.runId != runId)
            continue;
        const ActionRun *run = findRun(runId);
        if (!run)
            return;
        column.title->setFullText(run->workflowName);
        column.spinner->setVisible(run->status == ActionStatus::Running);
        column.meta->setFullText(actionRunMetaText(*run));
        // Same gates as the primary pane's row (see showRun).
        const bool live = run->status == ActionStatus::Running ||
                          run->status == ActionStatus::Queued;
        column.stopButton->setVisible(live);
        column.skipButton->setVisible(
            run->status == ActionStatus::Queued ||
            run->status == ActionStatus::AwaitingApproval);
        column.copyLogButton->setVisible(true);
        column.copyLogPathButton->setVisible(true);
        column.sendToAgentButton->setVisible(run->status ==
                                             ActionStatus::Failed);
        column.rerunButton->setVisible(true);
        column.log->setPlaceholderText(actionRunLogPlaceholder(*run));
        const QString logFile = actionRunLogLocation(*run);
        column.logPath->setFullText(
            logFile.isEmpty() ? QString()
                              : QStringLiteral("Log: %1").arg(logFile));
        column.logPath->setVisible(!logFile.isEmpty());
        if (reloadLog && m_actionStore) {
            column.log->setPlainText(
                displaySafePlainLog(m_actionStore->readLog(*run)));
            column.log->moveCursor(QTextCursor::End);
        }
        return;
    }
}

void MainWindow::selectActionRunRows(const QList<int> &runIds)
{
    if (!m_actionsTable || !m_actionsTable->selectionModel())
        return;
    QSignalBlocker block(m_actionsTable); // the caller already applied the set
    QItemSelection selection;
    const int lastColumn = m_actionsTable->columnCount() - 1;
    int primaryRow = -1;
    for (int row = 0; row < m_actionsTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_actionsTable->item(row, 0);
        if (!item)
            continue;
        const int runId = item->data(Qt::UserRole).toInt();
        if (!runIds.contains(runId))
            continue;
        selection.select(m_actionsTable->model()->index(row, 0),
                         m_actionsTable->model()->index(row, lastColumn));
        if (runId == m_selectedRunId)
            primaryRow = row;
    }
    m_actionsTable->selectionModel()->select(
        selection,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (primaryRow >= 0)
        m_actionsTable->selectionModel()->setCurrentIndex(
            m_actionsTable->model()->index(primaryRow, 0),
            QItemSelectionModel::NoUpdate);
}

void MainWindow::watchAllLiveRuns()
{
    if (!m_actionsTable)
        return;
    QList<int> live;
    for (int row = 0; row < m_actionsTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_actionsTable->item(row, 0);
        if (!item)
            continue;
        const int runId = item->data(Qt::UserRole).toInt();
        const ActionRun *run = findRun(runId);
        if (!run)
            continue;
        if (run->status == ActionStatus::Running ||
            run->status == ActionStatus::Queued ||
            run->status == ActionStatus::AwaitingApproval)
            live.append(runId);
    }
    if (live.isEmpty()) {
        flashMessage(QStringLiteral("No queued or running runs to watch."));
        return;
    }
    showRun(live.first());
    setParallelRunIds(live.mid(1));
    selectActionRunRows(openActionRunIds());
}

// Verify a held run still describes the repository state it was bound to, then
// record the approval and queue it. Shared by the Actions page's Approve button
// and the pull request's Checks tab, so a first-time contributor's run can be
// released from wherever the maintainer happens to be reading it.
bool MainWindow::approveActionRun(int runId, QString *error)
{
    ActionRun *run = findRun(runId);
    if (!run || run->status != ActionStatus::AwaitingApproval) {
        if (error)
            *error = QStringLiteral("This run is not waiting for approval.");
        return false;
    }
    const int repoIndex = repoIndexFor(run->owner, run->name);
    if (repoIndex < 0) {
        if (error)
            *error = QStringLiteral("This run's repository is not on this node.");
        return false;
    }
    const QString mirror = m_repositories.at(repoIndex).mirrorPath;
    // Everything below the git reads works off this copy: they pump the GUI
    // event loop, and a reload serviced during the pump rebuilds m_actionRuns —
    // `run` would dangle (git-pump UAF family). The list is re-entered by id
    // once the verification is done.
    const ActionRun pending = *run;
    ActionRun verified = pending;
    QString snapshotError;
    if (workflowContentAt(mirror, pending.commit, pending.workflowPath) !=
            pending.workflowContent ||
        !bindActionRepositoryState(&verified, mirror, &snapshotError) ||
        verified.repositoryTree != pending.repositoryTree ||
        verified.executionDigest != pending.executionDigest) {
        if (error)
            *error = QStringLiteral(
                "The repository state changed or could not be verified. Queue a "
                "fresh run before approving it.");
        return false;
    }
    ActionStore::approve(pending.repoKey(), pending.workflowPath,
                         pending.workflowContent, pending.repositoryTree,
                         pending.executionDigest);
    if (!ActionStore::isApproved(
            pending.repoKey(), pending.workflowPath, pending.workflowContent,
            pending.repositoryTree, pending.executionDigest)) {
        if (error)
            *error = QStringLiteral("The bound approval could not be saved.");
        return false;
    }
    ActionRun *live = findRun(runId);
    if (!live || live->status != ActionStatus::AwaitingApproval) {
        if (error)
            *error = QStringLiteral("This run is no longer waiting for approval.");
        return false; // the bound approval stands; whatever moved it wins
    }
    live->status = ActionStatus::Queued;
    m_actionStore->saveRun(*live);
    scheduleMirrorActionsSummary(0);
    if (!m_actionQueue.contains(live->id))
        m_actionQueue.append(live->id);
    logSystem(QStringLiteral("Actions: approved \"%1\" for %2/%3%4.")
                  .arg(pending.workflowName, pending.owner, pending.name,
                       pending.pullNumber > 0
                           ? QStringLiteral(" (pull request #%1)")
                                 .arg(pending.pullNumber)
                           : QString()));
    return true;
}

void MainWindow::approveSelectedRun()
{
    const ActionRun *selected = findRun(m_selectedRunId);
    if (!selected || selected->status != ActionStatus::AwaitingApproval)
        return;
    QString error;
    if (!approveActionRun(m_selectedRunId, &error)) {
        if (!error.isEmpty())
            flashMessage(error);
        return;
    }
    m_actionRuns = m_actionStore->loadAllRuns();
    refreshActionsTable();
    showRun(m_selectedRunId);
    updateNotificationButton();
    refreshOpenPullChecks();
    processActionQueue();
}

bool MainWindow::resolveActionApproval(const RepositoryRecord &repo,
                                       const ActionRun &run)
{
    const bool alreadyApproved = ActionStore::isApproved(
        run.repoKey(), run.workflowPath, run.workflowContent,
        run.repositoryTree, run.executionDigest);
    // A first-time contributor's pull request is the one case automatic
    // approval must not cover: the run executes their shell commands on this
    // node, and nobody here has vouched for them yet. The maintainer approves
    // each such pull request by hand; because approval is bound to the whole
    // repository state at that commit, that consent never carries to the next
    // pull request, or even to their next push on this one.
    if (alreadyApproved || !repo.actionsAutoApprove || run.firstTimePullAuthor)
        return alreadyApproved;

    ActionStore::approve(run.repoKey(), run.workflowPath, run.workflowContent,
                         run.repositoryTree, run.executionDigest);
    return ActionStore::isApproved(
        run.repoKey(), run.workflowPath, run.workflowContent,
        run.repositoryTree, run.executionDigest);
}

int MainWindow::autoApproveAwaitingRuns(const RepositoryRecord &repo)
{
    if (!m_actionStore || !repo.actionsEnabled || !repo.actionsAutoApprove)
        return 0;

    int queued = 0;
    for (ActionRun &run : m_actionRuns) {
        if (run.status != ActionStatus::AwaitingApproval ||
            run.owner != repo.owner || run.name != repo.name)
            continue;
        if (run.firstTimePullAuthor) {
            logSystem(QString::fromUtf8(
                          "Actions: \xE2\x80\x9C%1\xE2\x80\x9D for %2/%3 #%4 "
                          "stays held \xE2\x80\x94 %5 has no merged work here "
                          "yet, so their pull request needs your approval.")
                          .arg(run.workflowName, run.owner, run.name)
                          .arg(run.pullNumber)
                          .arg(run.pullAuthor.isEmpty()
                                   ? QStringLiteral("its author")
                                   : run.pullAuthor));
            continue;
        }

        ActionRun verified = run;
        QString snapshotError;
        if (workflowContentAt(repo.mirrorPath, run.commit, run.workflowPath) !=
                run.workflowContent ||
            !bindActionRepositoryState(&verified, repo.mirrorPath,
                                       &snapshotError) ||
            verified.repositoryTree != run.repositoryTree ||
            verified.executionDigest != run.executionDigest) {
            logSystem(QStringLiteral(
                          "Actions: keeping \"%1\" for %2/%3 @ %4 awaiting "
                          "review because its saved repository state could not "
                          "be reverified%5.")
                          .arg(run.workflowName, run.owner, run.name,
                               run.commit.left(8),
                               snapshotError.isEmpty()
                                   ? QString()
                                   : QStringLiteral(": ") + snapshotError));
            continue;
        }
        if (!resolveActionApproval(repo, run)) {
            logSystem(QStringLiteral(
                          "Actions: keeping \"%1\" for %2/%3 @ %4 awaiting "
                          "review because its automatic approval could not be "
                          "saved.")
                          .arg(run.workflowName, run.owner, run.name,
                               run.commit.left(8)));
            continue;
        }

        run.status = ActionStatus::Queued;
        m_actionStore->saveRun(run);
        if (!m_actionQueue.contains(run.id))
            m_actionQueue.append(run.id);
        ++queued;
        logSystem(QStringLiteral(
                      "Actions: automatically approved \"%1\" for %2/%3 "
                      "@ %4.")
                      .arg(run.workflowName, run.owner, run.name,
                           run.commit.left(8)));
    }
    if (queued > 0)
        scheduleMirrorActionsSummary(0);
    return queued;
}

void MainWindow::rejectSelectedRun()
{
    if (rejectActionRun(m_selectedRunId))
        showRun(m_selectedRunId);
}

bool MainWindow::rejectActionRun(int runId)
{
    ActionRun *run = findRun(runId);
    if (!run || run->status != ActionStatus::AwaitingApproval)
        return false;
    run->status = ActionStatus::Rejected;
    run->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_actionStore->saveRun(*run);
    scheduleMirrorActionsSummary(0);
    logSystem(QStringLiteral("Actions: rejected \"%1\" for %2/%3.")
                  .arg(run->workflowName, run->owner, run->name));
    m_actionRuns = m_actionStore->loadAllRuns();
    refreshActionsTable();
    updateNotificationButton();
    ringLocalControlRepos();
    return true;
}

void MainWindow::rerunSelectedRun()
{
    rerunActionRun(m_selectedRunId);
}

int MainWindow::rerunActionRun(int runId)
{
    if (m_actionRunsRestoreInFlight) {
        flashMessage(QStringLiteral(
            "Actions history is still loading. Try again in a moment."));
        return 0;
    }
    const ActionRun *prev = findRun(runId);
    if (!prev || !m_actionStore)
        return 0;

    ActionRun run;
    run.owner = prev->owner;
    run.name = prev->name;
    run.workflowPath = prev->workflowPath;
    run.workflowName = prev->workflowName;
    run.workflowContent = prev->workflowContent;
    run.commit = prev->commit;
    run.ref = prev->ref;
    const int repoIndex = repoIndexFor(run.owner, run.name);
    if (repoIndex < 0)
        return 0;
    const ActionWorkflow rerunWorkflow =
        ActionFile::parse(run.workflowPath, run.workflowContent);
    if (!workflowRunsOnThisNode(m_repositories.at(repoIndex), rerunWorkflow)) {
        flashMessage(QString::fromUtf8(
                         "\xE2\x80\x9C%1\xE2\x80\x9D runs on %2 \xE2\x80\x94 "
                         "rerun it from that node.")
                         .arg(run.workflowName,
                              workflowDedicationLabel(
                                  m_repositories.at(repoIndex), rerunWorkflow)));
        return 0;
    }
    QString snapshotError;
    if (workflowContentAt(m_repositories.at(repoIndex).mirrorPath,
                          run.commit, run.workflowPath) !=
            run.workflowContent ||
        !bindActionRepositoryState(
            &run, m_repositories.at(repoIndex).mirrorPath,
            &snapshotError)) {
        flashMessage(QStringLiteral(
                         "Could not verify the repository state for this rerun: %1")
                         .arg(snapshotError));
        return 0;
    }
    const bool approved = resolveActionApproval(
        m_repositories.at(repoIndex), run);
    run.status = approved ? ActionStatus::Queued : ActionStatus::AwaitingApproval;

    const ActionRun created = m_actionStore->createRun(run);
    m_actionRuns.prepend(created);
    scheduleMirrorActionsSummary(0);
    cancelSupersededRuns(created);
    if (approved)
        m_actionQueue.append(created.id);
    logSystem(QStringLiteral("Actions: rerun %1 \"%2\" for %3/%4 @ %5")
                  .arg(approved ? QStringLiteral("of")
                                : QStringLiteral("awaiting approval of"),
                       run.workflowName, run.owner, run.name, run.commit.left(8)));

    refreshActionsTable();
    const int column = parallelRunIds().indexOf(runId);
    if (column >= 0) {
        // Rerun from a column: the new run takes that column's place, and the
        // primary pane keeps what it was showing.
        QList<int> ids = parallelRunIds();
        ids[column] = created.id;
        setParallelRunIds(ids);
        selectActionRunRows(openActionRunIds());
    } else {
        showRun(created.id);
    }
    updateNotificationButton();
    processActionQueue();
    ringLocalControlRepos();
    return created.id;
}

void MainWindow::stopSelectedRun()
{
    stopActionRun(m_selectedRunId);
}

void MainWindow::stopActionRun(int runId)
{
    ActionRun *run = findRun(runId);
    if (!run)
        return;

    if (run->status == ActionStatus::Running) {
        if (ActionRunner *runner = runnerForRun(run->id)) {
            logSystem(QStringLiteral("Actions: stopping \"%1\" for %2/%3.")
                          .arg(run->workflowName, run->owner, run->name));
            runner->stop();
        }
        return;
    }

    if (run->status == ActionStatus::Queued) {
        m_actionQueue.removeAll(run->id);
        run->status = ActionStatus::Cancelled;
        run->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_actionStore->saveRun(*run);
        scheduleMirrorActionsSummary(0);
        logSystem(QStringLiteral("Actions: cancelled queued \"%1\" for %2/%3.")
                      .arg(run->workflowName, run->owner, run->name));
        if (deleteCancelledRunsAutomatically({*run}))
            return;
        m_actionRuns = m_actionStore->loadAllRuns();
        refreshActionsTable();
        if (runId == m_selectedRunId)
            showRun(m_selectedRunId);
        else
            refreshParallelRunColumn(runId);
        updateNotificationButton();
    }
}

void MainWindow::skipSelectedRun()
{
    skipActionRun(m_selectedRunId);
}

void MainWindow::skipActionRun(int runId)
{
    ActionRun *run = findRun(runId);
    if (!run)
        return;

    if (run->status != ActionStatus::Queued &&
        run->status != ActionStatus::AwaitingApproval)
        return;

    m_actionQueue.removeAll(run->id);
    run->status = ActionStatus::Skipped;
    run->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_actionStore->saveRun(*run);
    scheduleMirrorActionsSummary(0);
    logSystem(QStringLiteral("Actions: skipped \"%1\" for %2/%3.")
                  .arg(run->workflowName, run->owner, run->name));
    m_actionRuns = m_actionStore->loadAllRuns();
    refreshActionsTable();
    if (runId == m_selectedRunId)
        showRun(m_selectedRunId);
    else
        refreshParallelRunColumn(runId);
    updateNotificationButton();
}

void MainWindow::copyActionRunLog(int runId)
{
    const ActionRun *run = findRun(runId);
    QString log;
    if (run && m_actionStore)
        log = m_actionStore->readLog(*run);
    else if (runId == m_selectedRunId && m_actionLog)
        log = m_actionLog->toPlainText();
    if (log.isEmpty()) {
        flashMessage(QStringLiteral("No log to copy yet."));
        return;
    }
    QApplication::clipboard()->setText(log);
    flashMessage(QStringLiteral("Run log copied to the clipboard."));
}

void MainWindow::copyActionRunLogPath(int runId)
{
    const ActionRun *run = findRun(runId);
    const QString path = run ? actionRunLogLocation(*run) : QString();
    if (path.isEmpty()) {
        flashMessage(QStringLiteral("This run has no log file yet."));
        return;
    }
    QApplication::clipboard()->setText(path);
    flashMessage(QStringLiteral("Log path copied to the clipboard."));
}

// Where this run's log sits on this node. Every surface that shows a log -- the
// detail pane, each parallel column, the prompts handed to agents -- says it in
// the same words, so what the user reads on screen is what an agent is told.
QString MainWindow::actionRunLogLocation(const ActionRun &run) const
{
    return m_actionStore ? QDir::toNativeSeparators(m_actionStore->logPath(run))
                         : QString();
}

// What an agent is told about a run. The log's path leads the prompt because the
// whole file is on this node's disk: a pasted tail routinely stops before the
// line that actually broke, and the agent can read the rest for itself. The
// excerpt below it is only there so the agent knows what it is opening.
QString MainWindow::actionRunAgentPrompt(const ActionRun &run) const
{
    const QString logFile = actionRunLogLocation(run);

    const QString log = m_actionStore ? m_actionStore->readLog(run) : QString();
    constexpr int kMaxLogChars = 8000;
    const QString logTail =
        log.isEmpty()
            ? QStringLiteral("(nothing logged yet - read the file above)")
            : (log.size() <= kMaxLogChars
                   ? log
                   : QStringLiteral(
                         "...(excerpt truncated; the whole log is in the file "
                         "above)...\n") +
                         log.right(kMaxLogChars));

    QString prompt =
        QStringLiteral("Please fix the \"%1\" CI run for %2/%3 (run #%4, %5, "
                       "commit %6, ref %7).\n\n")
            .arg(run.workflowName, run.owner, run.name)
            .arg(run.id)
            .arg(run.status, run.commit.left(8), run.ref);
    if (!logFile.isEmpty())
        prompt += QStringLiteral("Full run log: %1\n").arg(logFile);
    prompt += QStringLiteral(
        "Read that whole file first - it is on this machine, and the tail "
        "pasted below is only an excerpt. Find what broke, fix it at the "
        "source, and make sure the workflow succeeds afterwards.\n\n"
        "Please also fix the recent CI tests: check the other recent runs of "
        "this workflow for failing tests and fix those too, rather than only "
        "the one failure below.\n\nLog excerpt:\n\n");
    prompt += logTail;
    return prompt;
}

void MainWindow::sendSelectedRunToAgent(const QString &provider, const QString &model)
{
    sendActionRunToAgent(m_selectedRunId, provider, model);
}

void MainWindow::sendActionRunToAgent(int runId, const QString &provider,
                                      const QString &model)
{
    const ActionRun *run = findRun(runId);
    if (!run)
        return;
    const int repoIndex = repoIndexFor(run->owner, run->name);
    if (repoIndex < 0) {
        flashMessage("Can't find this run's repository.", true);
        return;
    }

    const int sessionId = startAdHocAgentForRepo(
        repoIndex, actionRunAgentPrompt(*run), provider, /*createPr=*/false, model,
        QStringLiteral("Fix the \"%1\" CI run").arg(run->workflowName));
    if (sessionId > 0)
        flashMessage(QStringLiteral("Sent \"%1\" to a %2 agent with its log.")
                         .arg(run->workflowName, agentProviderName(provider)));
}

void MainWindow::maybeAutoFixFailedRun(const ActionRun &run)
{
    if (!QSettings().value(kAutoFixFailuresSetting, true).toBool())
        return;
    const QString branch = run.ref.startsWith(QLatin1String("refs/heads/"))
                               ? run.ref.mid(11)
                               : run.ref;
    if (branch.isEmpty())
        return;
    int sessionId = 0;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.owner == run.owner && s.name == run.name &&
            s.branchName == branch && !isExternalSession(s.id))
            sessionId = s.id; // sessions are stored oldest-first; keep the last match
    }
    if (sessionId <= 0)
        return;
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || session->status == AgentStatus::Running ||
        session->status == AgentStatus::Queued ||
        session->status == AgentStatus::Waiting)
        return;

    const QString log = m_actionStore ? m_actionStore->readLog(run) : QString();
    constexpr int kMaxLogChars = 12000;
    const QString logTail =
        log.size() <= kMaxLogChars
            ? log
            : QStringLiteral("...(log truncated; showing the tail)...\n") +
                  log.right(kMaxLogChars);
    // The same handover as the "Send to agent" button: the path first, because
    // a session that is already running on this branch can open the whole file,
    // and the tail below it may have cut off the line that actually broke.
    const QString logFile = actionRunLogLocation(run);
    QString prompt =
        QStringLiteral("The \"%1\" CI workflow failed for %2/%3 (commit %4, ref "
                       "%5). Find what broke and fix it so the workflow "
                       "succeeds.\n")
            .arg(run.workflowName, run.owner, run.name, run.commit.left(8),
                 run.ref);
    if (!logFile.isEmpty()) {
        prompt += QStringLiteral(
                      "\nFull run log: %1\nRead that whole file on this "
                      "machine; what follows is only its tail.\n")
                      .arg(logFile);
    }
    prompt += QStringLiteral("\nLog excerpt:\n\n") + logTail;
    const QString workflowName = run.workflowName;

    // Same routing as the composer: a session that has finished its turn often
    // still has a live CLI process, and continueAgentSession() alone does
    // nothing while one is up — the flash below would then claim a delivery that
    // never happened.
    sendPromptToAgentSession(sessionId, prompt);
    flashMessage(QStringLiteral("Sent \"%1\"'s failure back to its agent.")
                     .arg(workflowName));
}

bool MainWindow::deleteCancelledRunsAutomatically(const QList<ActionRun> &runs)
{
    if (!m_actionStore || runs.isEmpty())
        return false;

    QList<ActionRun> doomed;
    QSet<int> doomedIds;
    for (const ActionRun &run : runs) {
        if (run.id <= 0 || run.status != ActionStatus::Cancelled ||
            doomedIds.contains(run.id)) {
            continue;
        }
        doomed.append(run);
        doomedIds.insert(run.id);
    }
    if (doomed.isEmpty() ||
        !QSettings().value(kAutoDeleteCancelledRunsSetting, true).toBool())
        return false;

    // ActionStore drops its in-memory entries synchronously and unlinks the
    // complete run trees on a worker, so cancelled logs disappear from every
    // view without blocking the GUI after a burst of superseded pushes.
    m_actionStore->deleteRunsInBackground(doomed);
    m_actionRuns = m_actionStore->loadAllRuns();
    for (int runId : std::as_const(doomedIds)) {
        m_actionQueue.removeAll(runId);
        m_actionWaitingRuns.remove(runId);
    }
    scheduleMirrorActionsSummary(0);

    if (doomedIds.contains(m_selectedRunId))
        showRun(-1);
    QList<int> keptColumns = parallelRunIds();
    keptColumns.removeIf([&](int id) { return doomedIds.contains(id); });
    if (keptColumns.size() != m_parallelRunColumns.size())
        setParallelRunIds(keptColumns);
    refreshActionsTable();
    refreshCommitStatusGlyphs();
    refreshOpenPullChecks();
    updateNotificationButton();
    return true;
}

void MainWindow::clearActionRuns()
{
    if (!m_actionStore || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const QString owner = m_repositories.at(m_repoDetailIndex).owner;
    const QString name = m_repositories.at(m_repoDetailIndex).name;

    QList<ActionRun> doomed;
    for (const ActionRun &run : std::as_const(m_actionRuns)) {
        if (run.owner != owner || run.name != name)
            continue;
        if (!m_selectedWorkflowFilter.isEmpty() &&
            run.workflowPath != m_selectedWorkflowFilter)
            continue;
        if (run.status == ActionStatus::Running ||
            run.status == ActionStatus::Queued)
            continue;
        doomed.append(run);
    }
    if (doomed.isEmpty()) {
        flashMessage(QStringLiteral("No finished runs to clear."));
        return;
    }

    if (QMessageBox::question(
            this, QStringLiteral("Clear runs"),
            QStringLiteral("Delete %1 run%2 from this list, including their "
                           "logs? This can't be undone.")
                .arg(doomed.size())
                .arg(doomed.size() == 1 ? QString() : QStringLiteral("s")),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;

    removeActionRuns(doomed);
}

void MainWindow::removeActionRuns(const QList<ActionRun> &doomed)
{
    if (!m_actionStore || doomed.isEmpty())
        return;
    m_actionStore->deleteRunsInBackground(doomed);
    m_actionRuns = m_actionStore->loadAllRuns(); // already without the doomed runs
    scheduleMirrorActionsSummary(0);
    // refreshActionsTable() below drops the columns whose runs just went; a
    // cleared primary run closes the lot through showRun(-1).
    if (!findRun(m_selectedRunId)) {
        m_selectedRunId = -1;
        showRun(-1);
    }
    refreshActionsTable();
    updateNotificationButton();
}

QWidget *MainWindow::buildRepoActionsTab()
{
    auto *page = new QWidget;

    auto *wfPane = buildActionsWorkflowPane();

    auto *listPane = buildActionsRunsPane();

    auto *detailPane = new QWidget;
    detailPane->setMinimumWidth(320); // keeps the approval bar readable
    m_actionRunTitle = new QLabel("Select a run");
    m_actionRunTitle->setObjectName("channelTitle");
    m_actionRunSpinner = new BusySpinner(nullptr, 16);
    m_actionRunSpinner->setToolTip(QStringLiteral("Running"));
    m_actionRunSpinner->hide();
    m_actionRunMeta = new QLabel;
    m_actionRunMeta->setObjectName("statusLine");
    m_actionRunMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // The log below is a scroll view; this says which file it is reading, so
    // the run can be opened in a terminal, an editor or an agent. It elides
    // rather than widens -- an ordinary run path is longer than the pane, and
    // a plain QLabel would push the splitter open to fit it. The tooltip and
    // the Copy path button beside it carry the untruncated text.
    m_actionRunLogPath = new ElidingStatusLabel;
    m_actionRunLogPath->setObjectName("statusLine");
    m_actionRunLogPath->setElideMode(Qt::ElideMiddle);
    m_actionRunLogPath->hide();

    m_actionApprovalBanner = new QLabel(
        "This workflow is new or changed. Review the difference "
        "below, then Approve to run it (secrets are only exposed after approval).");
    m_actionApprovalBanner->setTextFormat(Qt::PlainText);
    m_actionApprovalBanner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_actionApprovalBanner->setWordWrap(true);
    m_actionApprovalBanner->setStyleSheet(Theme::remapSheet(
        "color:#d29922; background:#1c1908; border:1px solid #3a3416; "
        "border-radius:6px; padding:8px;"));
    m_actionApprovalBanner->hide();

    m_actionDiff = new QTextBrowser;
    m_actionDiff->setReadOnly(true);
    m_actionDiff->setOpenExternalLinks(false);
    m_actionDiff->setFontFamily(QStringLiteral("monospace"));
    m_actionDiff->hide();
    registerDiffView(m_actionDiff);

    m_actionApproveButton = new QPushButton("Approve && run");
    m_actionApproveButton->setObjectName("primaryButton");
    m_actionApproveButton->setCursor(Qt::PointingHandCursor);
    m_actionRejectButton = new QPushButton("Reject");
    m_actionRejectButton->setObjectName("dangerButton");
    m_actionRejectButton->setCursor(Qt::PointingHandCursor);
    connect(m_actionApproveButton, &QPushButton::clicked, this,
            &MainWindow::approveSelectedRun);
    connect(m_actionRejectButton, &QPushButton::clicked, this,
            &MainWindow::rejectSelectedRun);
    m_actionSplitButton = new QPushButton;
    m_actionSplitButton->setObjectName("ghostButton");
    m_actionSplitButton->setCursor(Qt::PointingHandCursor);
    m_actionSplitButton->setCheckable(true);
    m_actionSplitButton->setChecked(diffSplitPref());
    setOcticon(m_actionSplitButton, "diff", 16);
    updateDiffSplitButton(m_actionSplitButton);
    connect(m_actionSplitButton, &QPushButton::clicked, this, [this](bool on) {
        setDiffSplitPref(on);
        updateDiffSplitButton(m_actionSplitButton);
        showRun(m_selectedRunId); // re-render the diff in the new layout
    });
    m_actionApprovalBar = new QWidget;
    auto *approvalRow = new QHBoxLayout(m_actionApprovalBar);
    approvalRow->setContentsMargins(0, 0, 0, 0);
    approvalRow->addWidget(m_actionApproveButton);
    approvalRow->addWidget(m_actionRejectButton);
    approvalRow->addStretch();
    approvalRow->addWidget(m_actionSplitButton);
    m_actionApprovalBar->hide();

    m_actionLog = new QPlainTextEdit;
    m_actionLog->setReadOnly(true);
    m_actionLog->setObjectName("actionLog");
    applyLogFont(m_actionLog);
    m_actionLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    new AgentLogHighlighter(m_actionLog->document());
    m_actionLog->setMaximumBlockCount(20000);

    m_actionManualRunBar = new QWidget;
    m_actionManualRunButton = new QPushButton("Run workflow");
    m_actionManualRunButton->setObjectName("primaryButton");
    m_actionManualRunButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_actionManualRunButton, "rocket", 16);
    m_actionRunBranchCombo = new QComboBox;
    m_actionRunBranchCombo->setEditable(true); // allow any ref, not just listed
    m_actionRunBranchCombo->setMinimumWidth(160);
    m_actionRunBranchCombo->setToolTip("Branch to check out and run the workflow on");
    auto *branchLabel = new QLabel("on branch");
    branchLabel->setObjectName("statusLine");
    connect(m_actionManualRunButton, &QPushButton::clicked, this,
            &MainWindow::runSelectedWorkflowManually);
    auto *manualRow = new QHBoxLayout(m_actionManualRunBar);
    manualRow->setContentsMargins(0, 0, 0, 0);
    manualRow->setSpacing(8);
    manualRow->addWidget(m_actionManualRunButton);
    manualRow->addWidget(branchLabel);
    manualRow->addWidget(m_actionRunBranchCombo);
    manualRow->addStretch();
    m_actionManualRunBar->hide();

    m_actionRerunButton = new QPushButton("Rerun");
    m_actionRerunButton->setObjectName("ghostButton");
    m_actionRerunButton->setProperty("buttonSize", "sm");
    m_actionRerunButton->setCursor(Qt::PointingHandCursor);
    m_actionRerunButton->setToolTip("Run this action again with the same commit");
    setOcticon(m_actionRerunButton, "sync", 16);
    m_actionRerunButton->hide();
    connect(m_actionRerunButton, &QPushButton::clicked, this,
            &MainWindow::rerunSelectedRun);

    m_actionStopButton = new QPushButton("Stop");
    m_actionStopButton->setObjectName("dangerButton");
    m_actionStopButton->setProperty("buttonSize", "sm");
    m_actionStopButton->setCursor(Qt::PointingHandCursor);
    m_actionStopButton->setToolTip("Stop this run");
    setOcticon(m_actionStopButton, "stop", 16);
    m_actionStopButton->hide();
    connect(m_actionStopButton, &QPushButton::clicked, this,
            &MainWindow::stopSelectedRun);

    m_actionSkipButton = new QPushButton("Skip");
    m_actionSkipButton->setObjectName("ghostButton");
    m_actionSkipButton->setProperty("buttonSize", "sm");
    m_actionSkipButton->setCursor(Qt::PointingHandCursor);
    m_actionSkipButton->setToolTip("Skip this run without executing it");
    setOcticon(m_actionSkipButton, "circle-slash", 16);
    m_actionSkipButton->hide();
    connect(m_actionSkipButton, &QPushButton::clicked, this,
            &MainWindow::skipSelectedRun);

    m_actionCopyLogButton = new QPushButton;
    m_actionCopyLogButton->setObjectName("ghostButton");
    m_actionCopyLogButton->setProperty("buttonSize", "sm");
    m_actionCopyLogButton->setCursor(Qt::PointingHandCursor);
    m_actionCopyLogButton->setToolTip("Copy this run's full log to the clipboard");
    setOcticon(m_actionCopyLogButton, "copy", 16);
    m_actionCopyLogButton->hide();
    connect(m_actionCopyLogButton, &QPushButton::clicked, this,
            [this] { copyActionRunLog(m_selectedRunId); });

    m_actionCopyLogPathButton = new QPushButton;
    m_actionCopyLogPathButton->setObjectName("ghostButton");
    m_actionCopyLogPathButton->setProperty("buttonSize", "sm");
    m_actionCopyLogPathButton->setCursor(Qt::PointingHandCursor);
    m_actionCopyLogPathButton->setToolTip(
        "Copy the path of this run's log file on this machine");
    setOcticon(m_actionCopyLogPathButton, "file-directory", 16);
    m_actionCopyLogPathButton->hide();
    connect(m_actionCopyLogPathButton, &QPushButton::clicked, this,
            [this] { copyActionRunLogPath(m_selectedRunId); });

    m_actionSendToAgentButton = new QPushButton;
    m_actionSendToAgentButton->setObjectName("ghostButton");
    m_actionSendToAgentButton->setProperty("buttonSize", "sm");
    m_actionSendToAgentButton->setCursor(Qt::PointingHandCursor);
    m_actionSendToAgentButton->setToolTip(
        "Send to agent: start an agent on this run. It gets the log's path on "
        "this machine and the ask to fix this failure and the recent CI tests "
        "with it");
    setOcticon(m_actionSendToAgentButton, "paper-airplane", 16);
    m_actionSendToAgentButton->hide();
    connect(m_actionSendToAgentButton, &QPushButton::clicked, this, [this] {
        // Whoever is showing gets to choose; with the pickers off this row that
        // is the setting, read now rather than at startup so a default changed
        // in Settings since launch is the one that runs.
        const bool picked = m_actionFixAgentCombo &&
                            m_actionFixAgentCombo->isVisible();
        const QString provider =
            picked ? m_actionFixAgentCombo->currentData().toString()
                   : defaultAgentProvider();
        const QString model =
            picked && m_actionFixModelCombo
                ? m_actionFixModelCombo->currentData().toString()
                : QString();
        sendSelectedRunToAgent(provider, model);
    });

    m_actionFixButton = new QPushButton("Action detail");
    m_actionFixButton->setObjectName("ghostButton");
    m_actionFixButton->setProperty("buttonSize", "sm");
    m_actionFixButton->setCursor(Qt::PointingHandCursor);
    m_actionFixButton->setToolTip("Open the action run detail");
    setOcticon(m_actionFixButton, "rocket", 16);
    m_actionFixButton->hide();
    connect(m_actionFixButton, &QPushButton::clicked, this, [this] {
        if (const ActionRun *run = findRun(m_selectedRunId))
            openActionRunFromNotification(run->id);
    });

    m_actionFixAgentCombo = new QComboBox;
    m_actionFixAgentCombo->setObjectName("issueControlSm");
    m_actionFixAgentCombo->setCursor(Qt::PointingHandCursor);
    m_actionFixAgentCombo->setToolTip("Which agent fixes this run");
    m_actionFixAgentCombo->addItem(QStringLiteral("Claude"), QStringLiteral("claude"));
    m_actionFixAgentCombo->addItem(QStringLiteral("OpenAI"), QStringLiteral("openai"));
    m_actionFixAgentCombo->addItem(QStringLiteral("CC"),
                                   QStringLiteral("claude-code"));
    m_actionFixAgentCombo->hide();
    {
        const QString def = defaultAgentProvider();
        const QString want = def == QLatin1String("claude-api")
                                 ? QStringLiteral("claude")
                                 : def;
        const int idx = m_actionFixAgentCombo->findData(want);
        m_actionFixAgentCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    m_actionFixModelCombo = new QComboBox;
    m_actionFixModelCombo->setObjectName("issueControlSm");
    m_actionFixModelCombo->setCursor(Qt::PointingHandCursor);
    m_actionFixModelCombo->setToolTip("Which model the agent uses");
    m_actionFixModelCombo->setProperty("claudeModelCombo", true);
    m_actionFixModelCombo->view()->installEventFilter(this);
    m_actionFixModelCombo->hide();
    fillAgentFixModelCombo(m_actionFixModelCombo,
                          m_actionFixAgentCombo->currentData().toString());
    applyLiveClaudeModelsToCombos();
    connect(m_actionFixAgentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_actionFixAgentCombo && m_actionFixModelCombo) {
                    fillAgentFixModelCombo(
                        m_actionFixModelCombo,
                        m_actionFixAgentCombo->currentData().toString());
                    applyLiveClaudeModelsToCombos();
                }
            });

    auto *titleSlot = new QPushButton;
    titleSlot->setObjectName("ghostButton");
    titleSlot->setProperty("buttonSize", "sm");
    setOcticon(titleSlot, "x", 14);
    QSizePolicy titleSlotPolicy = titleSlot->sizePolicy();
    titleSlotPolicy.setRetainSizeWhenHidden(true);
    titleSlot->setSizePolicy(titleSlotPolicy);
    titleSlot->hide();

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleRow->addWidget(m_actionRunSpinner);
    titleRow->addWidget(m_actionRunTitle, 1);
    titleRow->addWidget(titleSlot);

    auto *controlRow = new QHBoxLayout;
    controlRow->setContentsMargins(0, 0, 0, 0);
    controlRow->setSpacing(4);
    controlRow->addWidget(m_actionStopButton);
    controlRow->addWidget(m_actionSkipButton);
    controlRow->addWidget(m_actionCopyLogButton);
    controlRow->addWidget(m_actionCopyLogPathButton);
    controlRow->addWidget(m_actionFixButton);
    controlRow->addWidget(m_actionSendToAgentButton);
    controlRow->addWidget(m_actionFixAgentCombo);
    controlRow->addWidget(m_actionFixModelCombo);
    controlRow->addStretch();
    controlRow->addWidget(m_actionRerunButton);

    auto *detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(12, 22, 12, 22);
    detailLayout->setSpacing(8);
    detailLayout->addWidget(m_actionManualRunBar);
    detailLayout->addLayout(titleRow);
    detailLayout->addLayout(controlRow);
    detailLayout->addWidget(m_actionRunMeta);
    detailLayout->addWidget(m_actionRunLogPath);
    detailLayout->addWidget(m_actionApprovalBanner);
    detailLayout->addWidget(m_actionApprovalBar);
    detailLayout->addWidget(m_actionDiff, 1);
    detailLayout->addWidget(m_actionLog, 2);

    m_actionDetailSplitter = new QSplitter(Qt::Horizontal);
    m_actionDetailSplitter->setObjectName("actionDetailSplitter");
    m_actionDetailSplitter->setChildrenCollapsible(false);
    m_actionDetailSplitter->addWidget(detailPane);
    m_actionDetailSplitter->setStretchFactor(0, 1);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(wfPane);
    splitter->addWidget(listPane);
    splitter->addWidget(m_actionDetailSplitter);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(splitter, 1);
    refreshWorkflowNodeCombo(); // never show the dropdown empty
    return page;
}

// Actions tab, left pane: the repository's workflows, the run-on-push and
// auto-approve switches, and the node the workflows run on.
QWidget *MainWindow::buildActionsWorkflowPane()
{
    auto *wfPane = new QWidget;
    wfPane->setMinimumWidth(320);
    wfPane->setMaximumWidth(440);
    auto *wfHeading = new QLabel("Workflows");
    wfHeading->setObjectName("sectionLabel");
    auto *wfHint = new QLabel(
        "Actions defined in .forkmesh/. They run when a fork pushes to this "
        "repo's mirror.");
    wfHint->setObjectName("statusLine");
    wfHint->setWordWrap(true);
    m_actionWorkflowList = new QListWidget;
    m_actionWorkflowList->setObjectName("actionWorkflowList");
    m_actionWorkflowList->setFrameShape(QFrame::NoFrame);
    m_actionWorkflowList->setAlternatingRowColors(true);
    connect(m_actionWorkflowList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item, QListWidgetItem *) {
                m_selectedWorkflowFilter =
                    item ? item->data(Qt::UserRole).toString() : QString();
                refreshActionsTable();
                showLatestVisibleActionRun();
                updateManualRunBar();
                refreshWorkflowNodeCombo(); // the dropdown edits the selection
                refreshWorkflowProgress();
            });

    m_actionsEnabledCheck = new QCheckBox("Run actions on push");
    m_actionsEnabledCheck->setToolTip(
        "When a fork pushes to this repo's local mirror, run its .forkmesh/ "
        "workflows. New snapshots run automatically when automatic approval "
        "is enabled below.");
    connect(m_actionsEnabledCheck, &QCheckBox::toggled, this,
            [this](bool on) { setRepoActionsEnabled(on); });

    m_actionsAutoApproveCheck = new QCheckBox("Automatically approve runs");
    m_actionsAutoApproveCheck->setToolTip(
        "Start workflows without waiting for an approval click. Approval still "
        "covers the exact pushed repository snapshot.");
    connect(m_actionsAutoApproveCheck, &QCheckBox::toggled, this,
            [this](bool on) { setRepoActionsAutoApprove(on); });

    m_actionNodeLabel = new QLabel("Run every workflow on");
    m_actionNodeLabel->setObjectName("statusLine");
    m_actionNodeCombo = new QComboBox;
    m_actionNodeCombo->setObjectName("actionNodeCombo");
    m_actionNodeCombo->setMinimumWidth(150);
    m_actionNodeCombo->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(m_actionNodeCombo, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (index < 0 || !m_actionNodeCombo)
                    return;
                setWorkflowNode(m_actionNodeCombo->itemData(index).toString());
            });
    auto *nodeRow = new QHBoxLayout;
    nodeRow->setContentsMargins(0, 0, 0, 0);
    nodeRow->setSpacing(8);
    nodeRow->addWidget(m_actionNodeLabel);
    nodeRow->addWidget(m_actionNodeCombo, 1);

    auto *wfLayout = new QVBoxLayout(wfPane);
    wfLayout->setContentsMargins(16, 22, 8, 22);
    wfLayout->setSpacing(8);
    wfLayout->addWidget(wfHeading);
    wfLayout->addWidget(wfHint);
    wfLayout->addWidget(m_actionsEnabledCheck);
    wfLayout->addWidget(m_actionsAutoApproveCheck);
    wfLayout->addLayout(nodeRow);
    wfLayout->addWidget(m_actionWorkflowList, 1);
    return wfPane;
}

// Actions tab, middle pane: the runs table with its Watch live / Clear header.
QWidget *MainWindow::buildActionsRunsPane()
{
    auto *listPane = new QWidget;
    listPane->setMinimumWidth(475);
    auto *heading = new QLabel("Runs");
    heading->setObjectName("channelTitle");
    auto *watchLiveButton = new QPushButton("Watch live");
    watchLiveButton->setObjectName("ghostButton");
    watchLiveButton->setProperty("buttonSize", "sm");
    watchLiveButton->setCursor(Qt::PointingHandCursor);
    watchLiveButton->setToolTip(
        "Open every queued and running run side by side, each streaming its "
        "own log");
    setOcticon(watchLiveButton, "eye", 16);
    connect(watchLiveButton, &QPushButton::clicked, this,
            &MainWindow::watchAllLiveRuns);
    auto *clearRunsButton = new QPushButton("Clear");
    clearRunsButton->setObjectName("ghostButton");
    clearRunsButton->setProperty("buttonSize", "sm");
    clearRunsButton->setCursor(Qt::PointingHandCursor);
    clearRunsButton->setToolTip("Delete the runs listed here, including their logs");
    setOcticon(clearRunsButton, "trash", 16);
    connect(clearRunsButton, &QPushButton::clicked, this,
            &MainWindow::clearActionRuns);
    auto *runsHeaderRow = new QHBoxLayout;
    runsHeaderRow->setContentsMargins(0, 0, 0, 0);
    runsHeaderRow->addWidget(heading);
    runsHeaderRow->addStretch();
    runsHeaderRow->addWidget(watchLiveButton);
    runsHeaderRow->addWidget(clearRunsButton);
    auto *subtitle = new QLabel(
        "Changed workflows wait for your approval before they run. "
        "Ctrl-click runs to watch several side by side.");
    subtitle->setObjectName("statusLine");
    subtitle->setWordWrap(true);

    m_actionsTable = new QTableWidget(0, 4);
    m_actionsTableSignature.clear(); // a fresh table has rendered nothing yet
    installColumnHeaderMenu(m_actionsTable);
    m_actionsTable->setObjectName("issueTable");
    m_actionsTable->setHorizontalHeaderLabels(
        {"Workflow", "Status", "When", "Duration"});
    m_actionsTable->horizontalHeader()->setStretchLastSection(true);
    m_actionsTable->horizontalHeader()->setHighlightSections(false);
    m_actionsTable->setColumnWidth(0, 200);
    m_actionsTable->verticalHeader()->setVisible(false);
    m_actionsTable->verticalHeader()->setDefaultSectionSize(
        m_actionsTable->fontMetrics().height() * 2 + 16);
    m_actionsTable->setShowGrid(false);
    m_actionsTable->setWordWrap(false);
    m_actionsTable->setAlternatingRowColors(true);
    makeColumnsResizable(m_actionsTable); // spreadsheet-style draggable columns (#263)
    m_actionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_actionsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_actionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_actionsTable->setItemDelegate(new ActionFailureBorderDelegate(m_actionsTable));
    connect(m_actionsTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QModelIndexList rows =
            m_actionsTable->selectionModel()->selectedRows();
        if (rows.isEmpty())
            return;
        // The row the user last touched keeps the full detail pane (approval,
        // diff, the run buttons); the rest of the selection opens as columns
        // beside it.
        const int currentRow = m_actionsTable->selectionModel()->currentIndex().row();
        int primaryRow = rows.first().row();
        for (const QModelIndex &index : rows) {
            if (index.row() == currentRow) {
                primaryRow = currentRow;
                break;
            }
        }
        QList<int> extras;
        for (const QModelIndex &index : rows) {
            if (index.row() == primaryRow)
                continue;
            if (QTableWidgetItem *item = m_actionsTable->item(index.row(), 0))
                extras.append(item->data(Qt::UserRole).toInt());
        }
        if (QTableWidgetItem *first = m_actionsTable->item(primaryRow, 0))
            showRun(first->data(Qt::UserRole).toInt());
        setParallelRunIds(extras);
    });

    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(12, 22, 12, 22);
    listLayout->setSpacing(8);
    listLayout->addLayout(runsHeaderRow);
    listLayout->addWidget(subtitle);
    listLayout->addWidget(m_actionsTable, 1);
    return listPane;
}

// ---- Settings: variables / secrets ----------------------------------------

void MainWindow::reloadVariablesList()
{
    if (!m_varsListLayout)
        return;
    const QMap<QString, QString> vars = ActionStore::variables();

    while (QLayoutItem *item = m_varsListLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    for (auto it = vars.constBegin(); it != vars.constEnd(); ++it) {
        const QString name = it.key();
        const QString value = it.value();

        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("variableCard"));
        card->setFrameShape(QFrame::StyledPanel);
        auto *cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(12, 10, 12, 10);
        cardLayout->setSpacing(8);

        auto *nameLabel = new QLabel(name);
        nameLabel->setObjectName(QStringLiteral("variableName"));
        nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardLayout->addWidget(nameLabel);

        auto *valueEdit = new QLineEdit(value);
        valueEdit->setReadOnly(true);
        valueEdit->setEchoMode(m_varsRevealed ? QLineEdit::Normal
                                              : QLineEdit::Password);
        valueEdit->setToolTip(m_varsRevealed
                                  ? QStringLiteral("Use Copy to copy this value")
                                  : QStringLiteral("Reveal values to view or copy them"));
        cardLayout->addWidget(valueEdit, 1);

        auto *editButton = new QPushButton(QString::fromUtf8("Edit\xE2\x80\xA6"));
        editButton->setObjectName(QStringLiteral("ghostButton"));
        editButton->setCursor(Qt::PointingHandCursor);
        connect(editButton, &QPushButton::clicked, this,
                [this, name] { addOrEditVariable(name); });
        cardLayout->addWidget(editButton);
        auto *copyButton = new QPushButton(QStringLiteral("Copy"));
        copyButton->setObjectName(QStringLiteral("ghostButton"));
        copyButton->setCursor(Qt::PointingHandCursor);
        copyButton->setEnabled(m_varsRevealed);
        copyButton->setToolTip(m_varsRevealed
                                   ? QStringLiteral("Copy this value to the clipboard")
                                   : QStringLiteral("Reveal values before copying"));
        connect(copyButton, &QPushButton::clicked, this, [this, value, name] {
            QApplication::clipboard()->setText(value);
            logSystem(QStringLiteral("Copied %1 to clipboard.").arg(name));
        });
        cardLayout->addWidget(copyButton);
        auto *deleteButton = new QPushButton(QStringLiteral("Delete"));
        deleteButton->setObjectName(QStringLiteral("ghostButton"));
        deleteButton->setCursor(Qt::PointingHandCursor);
        connect(deleteButton, &QPushButton::clicked, this,
                [this, name] { deleteVariable(name); });
        cardLayout->addWidget(deleteButton);
        m_varsListLayout->addWidget(card);
    }

    if (vars.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral(
            "No variables or secrets yet. Add one to make it available to every action run."));
        empty->setObjectName(QStringLiteral("statusLine"));
        empty->setWordWrap(true);
        m_varsListLayout->addWidget(empty);
    }
}

void MainWindow::addOrEditVariable(const QString &variableName)
{
    if (!m_varsListLayout)
        return;

    QString name, value;
    const bool editing = !variableName.isEmpty();
    if (editing) {
        name = variableName;
        value = ActionStore::variables().value(name);
    }

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, editing ? "Edit variable" : "Add variable",
        "Name (e.g. CLOUDFLARE_API_TOKEN):", QLineEdit::Normal, name, &ok);
    if (!ok || newName.trimmed().isEmpty())
        return;
    const QString newValue = QInputDialog::getText(
        this, editing ? "Edit variable" : "Add variable", "Value:",
        QLineEdit::Password, value, &ok);
    if (!ok)
        return;

    QMap<QString, QString> vars = ActionStore::variables();
    if (editing && newName.trimmed() != name)
        vars.remove(name);
    vars.insert(newName.trimmed(), newValue);
    ActionStore::setVariables(vars);
    reloadVariablesList();
}

void MainWindow::deleteVariable(const QString &name)
{
    if (name.isEmpty())
        return;
    QMap<QString, QString> vars = ActionStore::variables();
    vars.remove(name);
    ActionStore::setVariables(vars);
    reloadVariablesList();
}

void MainWindow::exportVariables()
{
    const QMap<QString, QString> vars = ActionStore::variables();
    if (vars.isEmpty()) {
        QMessageBox::information(this, "Export variables",
                                 "There are no variables or secrets to export.");
        return;
    }

    const int confirm = QMessageBox::warning(
        this, "Export variables",
        "This export writes secret values in clear text. Keep the file private.",
        QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
    if (confirm != QMessageBox::Ok)
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, "Export variables / secrets", "forkmesh-variables.json",
        "ForkMesh variables (*.json);;All files (*)");
    if (path.isEmpty())
        return;

    QJsonObject variables;
    for (auto it = vars.constBegin(); it != vars.constEnd(); ++it)
        variables.insert(it.key(), it.value());

    QJsonObject root;
    root.insert("kind", "forkmesh-action-variables-v1");
    root.insert("exportedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert("variables", variables);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Export failed",
                             "Could not write " + path);
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        QMessageBox::warning(this, "Export failed",
                             "Could not save " + path);
        return;
    }

    flashMessage(QStringLiteral("Exported %1 variable%2.")
                     .arg(vars.size())
                     .arg(vars.size() == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::importVariables()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Import variables / secrets", QString(),
        "ForkMesh variables (*.json);;JSON files (*.json);;All files (*)");
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Import failed",
                             "Could not read " + path);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(
            this, "Import failed",
            "That file is not valid variables JSON: " + parseError.errorString());
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonObject source =
        root.value("variables").isObject() ? root.value("variables").toObject()
                                           : root;
    QMap<QString, QString> imported;
    QStringList skipped;
    for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
        const QString name = it.key().trimmed();
        if (name.isEmpty() || !it.value().isString()) {
            skipped.append(it.key());
            continue;
        }
        imported.insert(name, it.value().toString());
    }

    if (imported.isEmpty()) {
        QMessageBox::warning(this, "Import failed",
                             "No string variables were found in that file.");
        return;
    }

    QMap<QString, QString> next = ActionStore::variables();
    const bool hasExisting = !next.isEmpty();
    if (hasExisting) {
        QMessageBox choice(this);
        choice.setWindowTitle("Import variables");
        choice.setText("Import " + QString::number(imported.size()) +
                       " variable" + (imported.size() == 1 ? "" : "s") + "?");
        choice.setInformativeText(
            "Merge keeps existing variables and overwrites matching names. "
            "Replace clears the current list first.");
        QPushButton *mergeButton =
            choice.addButton("Merge", QMessageBox::AcceptRole);
        QPushButton *replaceButton =
            choice.addButton("Replace", QMessageBox::DestructiveRole);
        choice.addButton(QMessageBox::Cancel);
        choice.setDefaultButton(mergeButton);
        choice.exec();
        if (choice.clickedButton() == replaceButton)
            next.clear();
        else if (choice.clickedButton() != mergeButton)
            return;
    }

    for (auto it = imported.constBegin(); it != imported.constEnd(); ++it)
        next.insert(it.key(), it.value());
    ActionStore::setVariables(next);
    reloadVariablesList();

    QString message = QStringLiteral("Imported %1 variable%2.")
                          .arg(imported.size())
                          .arg(imported.size() == 1 ? QString()
                                                    : QStringLiteral("s"));
    if (!skipped.isEmpty())
        message += QStringLiteral(" Skipped %1 non-string entr%2.")
                       .arg(skipped.size())
                       .arg(skipped.size() == 1 ? QStringLiteral("y")
                                                : QStringLiteral("ies"));
    flashMessage(message);
}

void MainWindow::toggleVariablesRevealed()
{
    m_varsRevealed = !m_varsRevealed;
    if (m_varsRevealButton)
        m_varsRevealButton->setText(m_varsRevealed ? "Hide" : "Reveal");
    reloadVariablesList();
}

void MainWindow::persistVariablesFromTable()
{
}

// Repository fetch: syncRepository's clone/fetch of a mirror from its
// source, and the pending auto-open/fork work that waits on it. Split out
// of MainWindowRepos.cpp.

#include "GitRefusal.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowReposInternal.h"
#include "NodeEventSocket.h"
#include "PublicMirrorRuntime.h"
#include "UpstreamCheckoutSync.h"

using namespace forkmesh::ui;
using namespace mainwindowrepos;

namespace {

// code-health: allow(blocking-wait) sync git primitive; its only caller is the QThread::create worker that tidies the mirror after syncRepository's fetch
void repairMirrorHead(const QString &mirrorPath, const QString &sourcePath)
{
    QString preferred;
    if (QDir(sourcePath).exists(QStringLiteral(".git"))) {
        QProcess source;
        source.start("git", {"-C", sourcePath, "symbolic-ref", "--short", "HEAD"});
        if (source.waitForFinished(5000) && source.exitCode() == 0)
            preferred = QString::fromUtf8(source.readAllStandardOutput()).trimmed();
    }

    const QString current = mirrorHeadBranch(mirrorPath);
    QStringList candidates{preferred, QStringLiteral("main"),
                           QStringLiteral("master"), current};
    QString branch;
    for (const QString &candidate : std::as_const(candidates)) {
        if (!candidate.isEmpty() &&
            !mirrorBranchCommit(mirrorPath, candidate).isEmpty()) {
            branch = candidate;
            break;
        }
    }
    if (branch.isEmpty()) {
        QProcess refs;
        refs.start("git", {"-C", mirrorPath, "for-each-ref",
                           "--format=%(refname:short)", "--sort=-committerdate",
                           "--count=1", "refs/heads/"});
        if (refs.waitForFinished(5000) && refs.exitCode() == 0)
            branch = QString::fromUtf8(refs.readAllStandardOutput()).trimmed();
    }
    if (branch.isEmpty() || branch == current)
        return;

    QProcess setHead;
    setHead.start("git", {"-C", mirrorPath, "symbolic-ref", "HEAD",
                          "refs/heads/" + branch});
    setHead.waitForFinished(5000);
}

bool isTransientSyncError(const QString &errors)
{
    return errors.contains(QStringLiteral("HTTP 50")) ||
           errors.contains(QStringLiteral("RPC failed")) ||
           errors.contains(QStringLiteral("curl 22")) ||
           errors.contains(QStringLiteral("502")) ||
           errors.contains(QStringLiteral("503")) ||
           errors.contains(QStringLiteral("504")) ||
           errors.contains(QStringLiteral("Could not resolve"),
                           Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("Couldn't connect"),
                           Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("Connection reset"),
                           Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("early EOF"), Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("unexpected disconnect"),
                           Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("HTTP2 framing layer"),
                           Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("HTTP/2 stream"),
                           Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("sideband"), Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("index-pack"), Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("fetch-pack"), Qt::CaseInsensitive) ||
           errors.contains(QStringLiteral("remote end hung up"),
                           Qt::CaseInsensitive);
}

} // namespace

void MainWindow::syncRepository(int index, bool quiet)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_syncingRepos.contains(index))
        return;
    // This check must precede private/public dispatch, mirror-directory
    // creation and origin rewriting. A corrupt record therefore cannot touch
    // either source path, and its alert can describe both identities without
    // pretending the record's owner/name was what Git tried to fetch.
    if (!validateMirrorSyncAssociation(index, quiet))
        return;

    RepositoryRecord &repo = m_repositories[index];
    const bool preview = repo.previewOnly;
    if (!preview && repo.isPrivate) {
        syncPrivateRepository(index, quiet);
        return;
    }
    if (!preview && (repo.mirrorPath.trimmed().isEmpty() ||
                     PublicMirrorRuntime::isArchiveId(
                         repo.publicArchiveId))) {
        // Public mirrors no longer pay the age seal/materialize cost. Migrate
        // every legacy encrypted record onto its deterministic durable bare
        // repository; the normal clone/fetch completion below removes the old
        // ciphertext only after this replacement is usable.
        repo.mirrorPath = repositoryMirrorRoot() + "/" +
                          repoSegment(repo.owner,
                                      QStringLiteral("owner")) +
                          "-" +
                          repoSegment(repo.name,
                                      QStringLiteral("repository")) +
                          ".git";
    }
    if (!QDir().mkpath(QFileInfo(repo.mirrorPath).absolutePath())) {
        if (!quiet)
            QMessageBox::warning(this, "Sync repository",
                                 "Could not create " +
                                     QFileInfo(repo.mirrorPath).absolutePath());
        return;
    }

    const bool hasMirror = QDir(repo.mirrorPath).exists();
    const QString source = repositorySource(repo);
    const bool managedCheckoutSource = false;
    QString managedUpstreamUrl;
    if (managedCheckoutSource) {
        const QUrl upstream(repo.cloneUrl.trimmed());
        if (upstream.isValid() && !upstream.host().isEmpty() &&
            upstream.host().compare(catalogApiUrl().host(),
                                    Qt::CaseInsensitive) == 0)
            managedUpstreamUrl = repo.cloneUrl.trimmed();
    }

    if (!preview && hasMirror && repo.publishToNetwork &&
        repo.localPath.trimmed().isEmpty() && repo.owner == accountOwner()) {
        if (!quiet)
            flashMessage(QStringLiteral("Nothing to sync for %1/%2 — this "
                                        "machine hosts it directly.")
                             .arg(repo.owner, repo.name));
        return;
    }

    const QString managedRefPrefix =
        managedCheckoutSource ? QString() : QStringLiteral("+");
    QStringList stableRefspecs = {
        managedRefPrefix + QStringLiteral("refs/heads/*:refs/heads/*"),
        managedRefPrefix + QStringLiteral("refs/tags/*:refs/tags/*")};
    const QString syncKey = (repo.owner + QLatin1Char('/') + repo.name).toLower();
    auto skipped = m_mirrorSkippedRefs.find(syncKey);
    if (skipped != m_mirrorSkippedRefs.end()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (auto ref = skipped->begin(); ref != skipped->end();) {
            if (ref.value() <= nowMs) {
                ref = skipped->erase(ref);
            } else {
                stableRefspecs.append(QLatin1Char('^') + ref.key());
                ++ref;
            }
        }
        if (skipped->isEmpty())
            m_mirrorSkippedRefs.erase(skipped);
    }
    // For our own private repo hosted through the mainnode, clone/fetch must carry
    // an owner-key-signed view token; viewAuthGitArgs returns the "-c
    // http.extraHeader=..." prefix (empty for public repos or non-mainnode sources)
    // generated fresh so the short-lived token never goes stale in stored config.
    const QStringList authArgs = viewAuthGitArgs(repo, source);
    const QStringList args =
        authArgs +
        (hasMirror ? QStringList{"-C", repo.mirrorPath, "fetch", "--progress",
                                "--prune", "origin"} +
                        stableRefspecs
                  : QStringList{"clone", "--progress", "--bare", source,
                                repo.mirrorPath});
    const QString mirrorPath = repo.mirrorPath;

    // Flag the repo "syncing" and reflect it in the UI right away — before any git
    // subprocess runs — so clicking "Sync" flips the button to "Syncing…"
    // instantly and never blocks the GUI thread. The insert also guards
    // re-entrancy so a concurrent auto-sync can't start a second fetch on this repo.
    m_syncingRepos.insert(index, quiet);
    refreshRepositoryList();
    if (!quiet) {
        const QString prefix =
            preview ? QStringLiteral("Preview cache: ")
                    : QStringLiteral("Mirror: ");
        logSystem(prefix +
                  (hasMirror ? QStringLiteral("fetching ") : QStringLiteral("cloning ")) +
                  repo.owner + "/" + repo.name + " from " + source + ".");
    }

    auto beforeDigest = std::make_shared<QString>();
    auto beforeHeadCommit = std::make_shared<QString>();
    auto upstreamSummary = std::make_shared<QString>();
    const QString managedCheckoutPath = repo.localPath.trimmed();
    QThread *prep = QThread::create(
        [mirrorPath, source, hasMirror, managedCheckoutPath,
         managedUpstreamUrl, beforeDigest, beforeHeadCommit,
         upstreamSummary] {
            if (!managedUpstreamUrl.isEmpty()) {
                *upstreamSummary =
                    forkmesh::upstream::refreshManagedCheckoutFromUpstream(
                        managedCheckoutPath, managedUpstreamUrl)
                        .summary();
            }
            *beforeDigest = mirrorRefsDigest(mirrorPath);
            *beforeHeadCommit =
                mirrorBranchCommit(mirrorPath, mirrorHeadBranch(mirrorPath));
            // Restating the URL it already has still rewrites the mirror's
            // config file, and that file's mtime is what keys the process-wide
            // config memo: every sync sent the next identity read back to a
            // blocking `git config` on the GUI thread.
            QByteArray currentUrl;
            const bool urlCurrent =
                cachedGitConfigLocal(mirrorPath,
                                     QStringLiteral("remote.origin.url"),
                                     &currentUrl) &&
                QString::fromUtf8(currentUrl).trimmed() == source;
            if (hasMirror && !source.isEmpty() && !urlCurrent &&
                !runGitCapture(mirrorPath,
                               {QStringLiteral("remote"), QStringLiteral("set-url"),
                                QStringLiteral("origin"), source},
                               nullptr, nullptr)) {
                runGitCapture(mirrorPath,
                              {QStringLiteral("remote"), QStringLiteral("add"),
                               QStringLiteral("origin"), source},
                              nullptr, nullptr);
            }
        });
    connect(prep, &QThread::finished, this,
            [this, prep, index, quiet, hasMirror, args, beforeDigest,
             beforeHeadCommit, upstreamSummary] {
                prep->deleteLater();
                if (index < 0 || index >= m_repositories.size()) {
                    m_syncingRepos.remove(index);
                    refreshRepositoryList();
                    return;
                }
                if (!upstreamSummary->isEmpty())
                    logSystem(QStringLiteral("Managed mirror %1/%2: %3")
                                  .arg(m_repositories.at(index).owner,
                                       m_repositories.at(index).name,
                                       *upstreamSummary));
                startSyncFetch(index, quiet, hasMirror, args, *beforeDigest,
                               *beforeHeadCommit);
            });
    prep->start();
}

bool MainWindow::completePendingRepoAutoOpen(int index)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_pendingAutoOpenRepoKey.isEmpty())
        return false;
    // By value: the loaders below pump the event loop, and a reference into
    // m_repositories cannot be held across that (git-pump UAF family).
    const bool previewOnly = m_repositories.at(index).previewOnly;
    const QString key =
        m_repositories.at(index).owner + "/" + m_repositories.at(index).name;
    if (previewOnly ||
        m_pendingAutoOpenRepoKey.compare(key, Qt::CaseInsensitive) != 0)
        return false;
    m_pendingAutoOpenRepoKey.clear();
    m_repoDetailIndex = index;
    renderPageNotices();
    QTimer::singleShot(0, this, [this] { notifyNativePackagesRepositoryChanged(); });
    refreshRepositoryList();
    m_repoInfo = RepoInfo();
    m_repoBranch.clear();
    loadRepoInfo();
    loadBranchesAndTags();
    updateFooterGitIdentity();
    updateFooterCommitInfo();
    refreshRepoTabCounts();
    QTimer::singleShot(0, this, [this] {
        showChatView();
        switchConversation(welcomeChannelForIdentity());
    });
    return true;
}

bool MainWindow::completePendingRepoFork(int index)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_pendingForkRepoKey.isEmpty()) {
        return false;
    }
    // Copy before openRepoDetail: repository loaders pump the event loop and
    // can invalidate references into m_repositories.
    const RepositoryRecord repo = m_repositories.at(index);
    const QString key = (repo.owner + QLatin1Char('/') + repo.name).toLower();
    if (m_pendingForkRepoKey.compare(key, Qt::CaseInsensitive) != 0 ||
        m_syncingRepos.contains(index) ||
        !QFileInfo(QDir(repo.mirrorPath).filePath(QStringLiteral("HEAD")))
             .isFile()) {
        return false;
    }
    m_pendingForkRepoKey.clear();
    openRepoDetail(index);
    QTimer::singleShot(0, this, [this, key] {
        if (m_repoDetailIndex < 0 ||
            m_repoDetailIndex >= m_repositories.size()) {
            return;
        }
        const RepositoryRecord selected =
            m_repositories.at(m_repoDetailIndex);
        if ((selected.owner + QLatin1Char('/') + selected.name)
                .compare(key, Qt::CaseInsensitive) != 0) {
            return;
        }
        forkCurrentRepo();
    });
    return true;
}

void MainWindow::startSyncFetch(int index, bool quiet, bool hasMirror,
                                const QStringList &args,
                                const QString &beforeDigest,
                                const QString &beforeHeadCommit)
{
    auto *process = new QProcess(this);
    setRepoSyncActivity(index, hasMirror
                                   ? QStringLiteral("Fetching from the source…")
                                   : QStringLiteral("Cloning the source…"));
    auto fetchErrors = streamGitProgressActivity(process, index, QString());
    connect(process, &QProcess::finished, this,
            [this, process, index, quiet, beforeDigest, beforeHeadCommit,
             hasMirror, fetchErrors](
                int exitCode, QProcess::ExitStatus) {
                fetchErrors->append(
                    QString::fromUtf8(process->readAllStandardError()));
                const QString errors = gitErrorsWithoutProgress(*fetchErrors);
                process->deleteLater();

                if (index < 0 || index >= m_repositories.size()) {
                    m_syncingRepos.remove(index);
                    clearRepoSyncActivity(index);
                    refreshRepositoryList();
                    return;
                }

                RepositoryRecord &repo = m_repositories[index];
                if (exitCode == 0) {
                    const QString mirrorPath = repo.mirrorPath;
                    const QString localPath = repo.localPath;
                    const QString identity =
                        repositoryIdentityKey(repo).toLower();
                    setRepoSyncActivity(
                        index,
                        QStringLiteral("Tidying the mirror's refs…"));
                    auto afterDigest = std::make_shared<QString>();
                    auto headBranch = std::make_shared<QString>();
                    auto headCommit = std::make_shared<QString>();
                    QThread *worker = QThread::create(
                        [mirrorPath, localPath, identity, afterDigest,
                         headBranch, headCommit] {
                            repairMirrorHead(mirrorPath, localPath);
                            pruneNonStableMirrorRefs(mirrorPath);
                            *afterDigest = mirrorRefsDigest(mirrorPath);
                            *headBranch = mirrorHeadBranch(mirrorPath);
                            *headCommit =
                                mirrorBranchCommit(mirrorPath, *headBranch);
                            stampGitRepositoryIdentity(mirrorPath, identity);
                        });
                    connect(worker, &QThread::finished, this,
                            [this, worker, index, quiet, hasMirror, beforeDigest,
                             beforeHeadCommit, afterDigest, headBranch,
                             headCommit] {
                        worker->deleteLater();
                        m_syncingRepos.remove(index);
                        clearRepoSyncActivity(index);
                        if (index < 0 || index >= m_repositories.size()) {
                            refreshRepositoryList();
                            return;
                        }
                        RepositoryRecord &repo = m_repositories[index];
                        const QString repositoryKey =
                            repositoryIdentityKey(repo).toLower();
                        m_mirrorRefRaceRetries.remove(
                            (repo.owner + QLatin1Char('/') + repo.name)
                                .toLower());
                        m_mirrorTransientFailureAlertMs.remove(repositoryKey);
                        const bool stillPreview = repo.previewOnly;
                        const bool changed =
                            !hasMirror || *afterDigest != beforeDigest;
                        repo.lastSyncMs = QDateTime::currentMSecsSinceEpoch();
                        if (!stillPreview && !repo.isPrivate &&
                            PublicMirrorRuntime::isArchiveId(
                                repo.publicArchiveId)) {
                            const QString archiveId = repo.publicArchiveId;
                            m_publicMirrorMaterializations.remove(archiveId);
                            const bool ciphertextRemoved = QFile::remove(
                                PublicMirrorRuntime::ciphertextPath(
                                    publicArchiveRoot(), archiveId));
                            const bool metadataRemoved = QFile::remove(
                                QDir(publicArchiveRoot())
                                    .filePath(archiveId +
                                              QStringLiteral(".json")));
                            repo.publicArchiveId.clear();
                            logSystem(QStringLiteral(
                                          "Public mirror: migrated %1/%2 to "
                                          "plaintext storage; legacy age "
                                          "archive cleanup %3.")
                                          .arg(repo.owner, repo.name,
                                               (ciphertextRemoved ||
                                                metadataRemoved)
                                                   ? QStringLiteral("completed")
                                                   : QStringLiteral(
                                                         "was already complete")));
                        }
                        if (!stillPreview)
                            saveRepositories();
                        refreshRepositoryList();
                        if (!stillPreview)
                            ensurePushHook(repo);
                        if (changed && hasMirror && !stillPreview &&
                            m_actionStore && !headBranch->isEmpty() &&
                            !headCommit->isEmpty() &&
                            *headCommit != beforeHeadCommit) {
                            enqueuePushEvent(repo.owner, repo.name, *headCommit,
                                             "refs/heads/" + *headBranch);
                        }
                        if (changed && index == m_repoDetailIndex)
                            refreshOpenRepoDetail();
                        if (changed && !stillPreview)
                            scanRepoMentionsFor(repo);
                        if (!quiet || changed) {
                            logSystem((stillPreview ? QStringLiteral("Preview cache: cached ")
                                                    : QStringLiteral("Mirror: synced ")) +
                                      repo.owner + "/" + repo.name + " into " +
                                      repo.mirrorPath + ".");
                        }
                        if (!quiet) {
                            flashMessage(stillPreview
                                             ? "Cached preview for " + repo.owner + "/" +
                                                   repo.name +
                                                   (changed ? QString()
                                                            : " (already up to date)")
                                             : "Synced " + repo.owner + "/" + repo.name +
                                                   (changed ? QString()
                                                            : " (already up to date)"));
                        }
                        if (!stillPreview && repo.publishToNetwork) {
                            startNodeEventSocket();
                        }
                        if (!stillPreview && hasMirror && (changed || !quiet))
                            replicateReleaseArtifacts(index);
                        // A periodic/source sync is also the recovery path for
                        // a commit hook or relay event that was missed. Re-fan
                        // the current stable refs to every SSH mirror even when
                        // the local fetch was a no-op; git makes an already
                        // current push cheap, and the gateway health tracker
                        // rotates genuinely unavailable hosts out.
                        if (!stillPreview)
                            pushToSshMirrorRemotes(
                                index, /*wakeManagedIfUnchanged=*/false);
                        // Last, because it reads the freshly landed mirror and so
                        // pumps the event loop: `repo` above is a reference into
                        // m_repositories and must not be held across it
                        // (git-pump UAF family).
                        if (!completePendingRepoFork(index))
                            completePendingRepoAutoOpen(index);
                    });
                    worker->start();
                } else {
                    m_syncingRepos.remove(index);
                    clearRepoSyncActivity(index);
                    refreshRepositoryList();
                    const QString failedKey =
                        (repo.owner + QLatin1Char('/') + repo.name).toLower();
                    const bool refTransactionRaced =
                        errors.contains(QStringLiteral("cannot lock ref"),
                                        Qt::CaseInsensitive) &&
                        errors.contains(QStringLiteral("but expected"),
                                        Qt::CaseInsensitive);
                    const int refRaceAttempt =
                        m_mirrorRefRaceRetries.value(failedKey, 0);
                    const auto resyncFailedRepo = [this, failedKey] {
                        for (int i = 0; i < m_repositories.size(); ++i) {
                            const RepositoryRecord &candidate =
                                m_repositories.at(i);
                            const QString candidateKey =
                                (candidate.owner + QLatin1Char('/') +
                                 candidate.name)
                                    .toLower();
                            if (candidateKey == failedKey) {
                                syncRepository(i, /*quiet=*/true);
                                return;
                            }
                        }
                    };
                    if (refTransactionRaced && refRaceAttempt < 3) {
                        m_mirrorRefRaceRetries.insert(failedKey,
                                                      refRaceAttempt + 1);
                        logSystem(QStringLiteral(
                                      "Mirror: source refs advanced during the "
                                      "snapshot for %1; retrying automatically "
                                      "(%2/3).")
                                      .arg(failedKey)
                                      .arg(refRaceAttempt + 1));
                        QTimer::singleShot(250 * (refRaceAttempt + 1), this,
                                           resyncFailedRepo);
                        return;
                    }
                    constexpr int kUnreadableRefRepairs = 8;
                    constexpr qint64 kSkippedRefRetryMs = 30 * 60 * 1000;
                    const QStringList namedBadRefs =
                        hasMirror
                            ? forkmesh::upstream::refsNamedAsBadObjects(errors)
                            : QStringList();
                    if (!namedBadRefs.isEmpty() &&
                        refRaceAttempt < kUnreadableRefRepairs) {
                        m_mirrorRefRaceRetries.insert(failedKey,
                                                      refRaceAttempt + 1);
                        const QString mirrorPath = repo.mirrorPath;
                        auto dropped = std::make_shared<QStringList>();
                        QThread *repair = QThread::create(
                            [mirrorPath, errors, dropped] {
                                *dropped =
                                    forkmesh::upstream::dropUnreadableRefs(
                                        mirrorPath, errors);
                            });
                        connect(
                            repair, &QThread::finished, this,
                            [this, repair, dropped, failedKey, namedBadRefs,
                             resyncFailedRepo] {
                                repair->deleteLater();
                                if (!dropped->isEmpty()) {
                                    logSystem(QStringLiteral(
                                                  "Mirror: %1 in the %2 mirror "
                                                  "pointed at an object it "
                                                  "cannot read; dropped it and "
                                                  "fetching again.")
                                                  .arg(dropped->join(
                                                           QStringLiteral(", ")),
                                                       failedKey));
                                } else {
                                    const qint64 until =
                                        QDateTime::currentMSecsSinceEpoch() +
                                        kSkippedRefRetryMs;
                                    QHash<QString, qint64> &skip =
                                        m_mirrorSkippedRefs[failedKey];
                                    for (const QString &ref : namedBadRefs)
                                        skip.insert(ref, until);
                                    logSystem(QStringLiteral(
                                                  "Mirror: warning: skipping "
                                                  "%1 for %2; the source's copy "
                                                  "points at an object that "
                                                  "cannot be read. Every other "
                                                  "branch keeps syncing; the "
                                                  "skipped ref is retried in "
                                                  "30 minutes.")
                                                  .arg(namedBadRefs.join(
                                                           QStringLiteral(", ")),
                                                       failedKey));
                                }
                                resyncFailedRepo();
                            });
                        repair->start();
                        return;
                    }
                    m_mirrorRefRaceRetries.remove(failedKey);
                    if (m_pendingForkRepoKey.compare(
                            failedKey, Qt::CaseInsensitive) == 0) {
                        m_pendingForkRepoKey.clear();
                        flashMessage(
                            QStringLiteral(
                                "Could not cache %1/%2 from the mesh; the fork was not started.")
                                .arg(repo.owner, repo.name),
                            true);
                    }
                    const bool transient = isTransientSyncError(errors);
                    constexpr qint64 kTransientFailureRepeatMs =
                        15 * 60 * 1000;
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    const qint64 lastTransient =
                        m_mirrorTransientFailureAlertMs.value(failedKey);
                    const bool announceFailure =
                        !transient || !quiet || lastTransient <= 0 ||
                        now - lastTransient >= kTransientFailureRepeatMs;
                    if (transient && announceFailure)
                        m_mirrorTransientFailureAlertMs.insert(failedKey, now);
                    else if (!transient)
                        m_mirrorTransientFailureAlertMs.remove(failedKey);
                    if (announceFailure) {
                        logSystem((repo.previewOnly
                                       ? QStringLiteral(
                                             "Preview cache: sync failed for ")
                                       : QStringLiteral(
                                             "Mirror: sync failed for ")) +
                                  repo.owner + "/" + repo.name +
                                  (transient
                                       ? QStringLiteral(" (host temporarily "
                                                        "unavailable): ")
                                       : QStringLiteral(": ")) +
                                  errors.right(300));
                    }
                    const QString explained =
                        forkmesh::gitrefusal::explainFailure(errors);
                    if (!quiet && transient) {
                        flashMessage(
                            explained.isEmpty()
                                ? QStringLiteral("Sync deferred for %1/%2 — host "
                                                 "temporarily unavailable.")
                                      .arg(repo.owner, repo.name)
                                : QStringLiteral("Sync deferred for %1/%2: %3")
                                      .arg(repo.owner, repo.name, explained),
                            /*error=*/false);
                    } else if (!quiet) {
                        const QString detail =
                            explained.isEmpty() ? errors.right(160) : explained;
                        flashMessage(
                            (repo.previewOnly ? QStringLiteral("Preview failed for ")
                                              : QStringLiteral("Sync failed for ")) +
                                repo.owner + "/" + repo.name +
                                (detail.isEmpty() ? QString() : ": " + detail),
                            /*error=*/true);
                    }
                }
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, index, quiet] {
                process->deleteLater();
                m_syncingRepos.remove(index);
                clearRepoSyncActivity(index);
                refreshRepositoryList();
                if (index >= 0 && index < m_repositories.size()) {
                    const RepositoryRecord repo = m_repositories.at(index);
                    const QString failedKey =
                        (repo.owner + QLatin1Char('/') + repo.name).toLower();
                    if (m_pendingForkRepoKey.compare(
                            failedKey, Qt::CaseInsensitive) == 0) {
                        m_pendingForkRepoKey.clear();
                        flashMessage(
                            QStringLiteral(
                                "Could not start Git for %1/%2; the fork was not started.")
                                .arg(repo.owner, repo.name),
                            true);
                    }
                }
                if (!quiet)
                    flashMessage(
                        "Could not run git. Install Git and try again.",
                        /*error=*/true);
            });
    trackProcessActivity(process, QStringLiteral("sync"),
                         gitArgsLine(QStringLiteral("git"), args));
    process->start("git", args);
}

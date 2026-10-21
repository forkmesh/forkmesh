#include <QSettings>
#include <algorithm>
#include "MainWindow.h"
#include "MessageBoxClicks.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "AgentActivityFeed.h"
#include "AgentCloudSessions.h"
#include "AgentPromptImages.h"
#include "OrgAgentJobAttachments.h"
#include "ClaudeTranscriptView.h"
#include "LocalControlHandoff.h"
#include "UpstreamCheckoutSync.h"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStackedWidget>
#include <QTimer>
#include <QUrlQuery>

// ---- Cloud agent sessions ---------------------------------------------------
// The Agents list shows what this desktop runs. The account also runs agents
// where no desktop is: a session queued from the website or a phone, or handed
// over with "Move to cloud", is claimed by one of the account's mirrors and
// worked there. Those runs live on the relay's owner agent queue, and this file
// brings them into the same list — marked with a cloud, filterable, and open to
// the things a cloud run can take: read its conversation, send it a follow-up,
// open its branch or pull request, delete it.
//
// A cloud row is an AgentSession with `cloud` set and an id inside the external
// band (kCloudIdBase and below), rebuilt from m_cloudAgentSessions on every
// reload exactly the way surfaced external sessions are. Nothing about it is
// written to the agent store.

using forkmesh::agents::CloudAgentSession;

namespace {

// How stale the account's cloud rows may get before a reload, a tab change or
// a window activation asks the relay again. The relay rings the account event
// socket ("agents") whenever a mirror claims, starts or finishes one of these
// runs, and that is what normally brings them in; these floors only bound the
// catch-up reads for the frames rung while the socket was down.
constexpr qint64 kCloudAgentRefreshMs = 5 * 60 * 1000;
// With a run in flight there is something to be wrong about, so catch up
// oftener — still minutes, not the twenty seconds this once polled at.
constexpr qint64 kCloudAgentActiveRefreshMs = 2 * 60 * 1000;
constexpr qint64 kCloudAgentWatchRefreshMs = 5 * 1000;
constexpr qint64 kCloudAgentImageKeepMs = 7LL * 24 * 60 * 60 * 1000;
constexpr int kCloudAgentImageFetches = 3;

QString cloudRepoKey(const QString &owner, const QString &name)
{
    return owner.toLower() + QLatin1Char('/') + name.toLower();
}

// The one entry in m_cloudAgentFetchesInFlight / m_cloudAgentFetchedAtMs: the
// account's queue is read whole, so there is one read to throttle, not one per
// repository. Not a repository key — it can never collide with one.
const QString kCloudAccountKey = QStringLiteral("*account*");

} // namespace

QUrl MainWindow::cloudAgentAccountSessionsUrl() const
{
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/account/agent-sessions"));
    return url;
}

QUrl MainWindow::cloudAgentBranchWebUrl(int sessionId) const
{
    const auto cloud = m_cloudAgentSessions.constFind(sessionId);
    if (cloud == m_cloudAgentSessions.constEnd() || cloud->branchName.isEmpty() ||
        cloud->owner.isEmpty() || cloud->name.isEmpty())
        return QUrl();
    const auto encodeRef = [](const QString &ref) {
        QStringList parts;
        for (const QString &part : ref.split(QLatin1Char('/')))
            parts.append(QString::fromLatin1(QUrl::toPercentEncoding(part)));
        return parts.join(QLatin1Char('/'));
    };
    const QString base = !cloud->baseBranch.isEmpty() ? cloud->baseBranch
                         : !cloud->baseRef.isEmpty()  ? cloud->baseRef
                                                      : QStringLiteral("main");
    QUrl url = catalogApiUrl();
    url.setQuery(QString());
    url.setFragment(QString());
    url.setPath(QLatin1Char('/') + encodeRef(cloud->relayRepoOwner()) + QLatin1Char('/') +
                    encodeRef(cloud->name) + QStringLiteral("/compare/") +
                    encodeRef(base) + QStringLiteral("...") +
                    encodeRef(cloud->branchName),
                QUrl::TolerantMode);
    return url;
}

void MainWindow::syncCloudAgentBranches()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#endif
    constexpr qint64 kCloudBranchSyncFloorMs = 20 * 1000;
    struct Pending {
        QString relayOwner;
        QStringList branches;
        QHash<QString, QString> marks;
    };
    QMap<int, Pending> byRepo;
    for (const CloudAgentSession &cloud : std::as_const(m_cloudAgentSessions)) {
        const QString branch = forkmesh::agents::cloudAgentContinueBranch(cloud);
        if (branch.isEmpty())
            continue;
        const int repoIndex = cloudAgentLocalRepoIndex(cloud);
        if (repoIndex < 0)
            continue;
        const QString key = cloudRepoKey(cloud.owner, cloud.name) +
                            QLatin1Char(' ') + branch;
        const QString sig = QStringLiteral("%1 %2 %3 %4")
                                .arg(cloud.status)
                                .arg(cloud.updatedAtMs)
                                .arg(cloud.finishedAtMs)
                                .arg(cloud.diffAhead);
        if (m_cloudBranchSyncSig.value(key) == sig)
            continue;
        Pending &pending = byRepo[repoIndex];
        if (pending.relayOwner.isEmpty())
            pending.relayOwner = cloud.relayRepoOwner();
        if (!pending.branches.contains(branch))
            pending.branches.append(branch);
        pending.marks.insert(key, sig);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool deferred = false;
    for (auto it = byRepo.constBegin(); it != byRepo.constEnd(); ++it) {
        const RepositoryRecord &repo = m_repositories.at(it.key());
        const QString repoKey = cloudRepoKey(repo.owner, repo.name);
        if (m_cloudBranchSyncInFlight.contains(repoKey) ||
            now - m_cloudBranchSyncAtMs.value(repoKey) < kCloudBranchSyncFloorMs) {
            deferred = true;
            continue;
        }
        m_cloudBranchSyncInFlight.insert(repoKey);
        m_cloudBranchSyncAtMs.insert(repoKey, now);
        const QHash<QString, QString> marks = it->marks;
        syncCloudAgentBranchesIn(
            it.key(), it->relayOwner, it->branches,
            [this, repoKey, marks](const QStringList &, bool reached) {
                m_cloudBranchSyncInFlight.remove(repoKey);
                if (!reached)
                    return;
                for (auto mark = marks.constBegin(); mark != marks.constEnd();
                     ++mark)
                    m_cloudBranchSyncSig.insert(mark.key(), mark.value());
            });
    }
    if (deferred && !m_cloudBranchSyncRetryQueued) {
        m_cloudBranchSyncRetryQueued = true;
        QTimer::singleShot(kCloudBranchSyncFloorMs, this, [this] {
            m_cloudBranchSyncRetryQueued = false;
            syncCloudAgentBranches();
        });
    }
}

void MainWindow::syncCloudAgentBranchesIn(
    int repoIndex, const QString &relayOwner, const QStringList &branches,
    std::function<void(const QStringList &present, bool reached)> done)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size() ||
        m_repositories.at(repoIndex).localPath.trimmed().isEmpty()) {
        if (done)
            done({}, false);
        return;
    }
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    const QString checkout = repo.localPath.trimmed();
    QStringList sources;
    if (!repo.isPrivate || repo.publishToNetwork)
        sources << repositoryNetworkCloneUrl(
                       relayOwner.isEmpty() ? repo.owner : relayOwner, repo.name)
                << repo.cloneUrl;
    sources << repo.mirrorPath;
    const QString owner = repo.owner;
    const QString name = repo.name;
    runOffThread<forkmesh::upstream::ServedBranchSync>(
        [checkout, sources, branches] {
            return forkmesh::upstream::syncServedAgentBranches(checkout, sources,
                                                               branches);
        },
        [this, owner, name, done](forkmesh::upstream::ServedBranchSync outcome) {
            if (!outcome.updated.isEmpty()) {
                logSystem(QStringLiteral("Synced cloud agent branch%1 %2 into "
                                         "%3/%4")
                              .arg(outcome.updated.size() == 1
                                       ? QString()
                                       : QStringLiteral("es"),
                                   outcome.updated.join(QStringLiteral(", ")),
                                   owner, name));
                if (m_repoDetailIndex >= 0 &&
                    m_repoDetailIndex < m_repositories.size() &&
                    cloudRepoKey(m_repositories.at(m_repoDetailIndex).owner,
                                 m_repositories.at(m_repoDetailIndex).name) ==
                        cloudRepoKey(owner, name) &&
                    m_branchDiffPullNumber < 0 && m_branchDiffView &&
                    m_branchDiffView->isVisible() &&
                    outcome.updated.contains(m_branchDiffBranch))
                    showBranchDiff(m_branchDiffBranch, m_branchDiffAgentSessionId);
            }
            if (done)
                done(outcome.present, outcome.reachedSource);
        });
}

void MainWindow::openCloudAgentBranch(int sessionId)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return;
    const CloudAgentSession cloud = it.value();
    const QString branch = cloud.branchName.trimmed();
    if (branch.isEmpty())
        return;
    const QUrl webUrl = cloudAgentBranchWebUrl(sessionId);
    const auto openOnWebsite = [webUrl] {
        if (webUrl.isValid())
            QDesktopServices::openUrl(webUrl);
    };
    const int repoIndex = cloudAgentLocalRepoIndex(cloud);
    if (repoIndex < 0) {
        openOnWebsite();
        return;
    }
    const bool haveLocal =
        localBranchExists(m_repositories.at(repoIndex).localPath, branch);
    const bool syncable = forkmesh::localcontrol::handoffBranchValid(branch);
    if (haveLocal)
        openAgentBranchInGitView(repoIndex, branch, cloud.baseBranch, sessionId);
    if (!syncable) {
        if (!haveLocal)
            openOnWebsite();
        return;
    }
    if (!haveLocal)
        flashMessage(QStringLiteral("Fetching %1 from the mirror…").arg(branch));
    syncCloudAgentBranchesIn(
        repoIndex, cloud.relayRepoOwner(), {branch},
        [this, cloud, branch, sessionId, haveLocal,
         openOnWebsite](const QStringList &present, bool) {
            if (haveLocal)
                return;
            const int index = cloudAgentLocalRepoIndex(cloud);
            if (index < 0 || !present.contains(branch)) {
                flashMessage(QStringLiteral("%1 is not on the mirror yet, so it "
                                            "opens on the website.")
                                 .arg(branch));
                openOnWebsite();
                return;
            }
            openAgentBranchInGitView(
                index, branch, cloud.baseBranch,
                m_cloudAgentSessions.contains(sessionId) ? sessionId : -1);
        });
}

QUrl MainWindow::cloudAgentSessionsUrl(const QString &owner, const QString &name,
                                       const QString &sessionId) const
{
    QUrl url = catalogApiUrl();
    QString path = QStringLiteral("/api/repo/") +
                   repoSegment(owner, QStringLiteral("owner")) +
                   QLatin1Char('/') +
                   repoSegment(name, QStringLiteral("repository")) +
                   QStringLiteral("/agent-sessions");
    if (!sessionId.isEmpty())
        path += QLatin1Char('/') + sessionId;
    url.setPath(path);
    return url;
}

QUrl MainWindow::cloudAgentSessionUrl(const CloudAgentSession &cloud) const
{
    if (!cloud.organization)
        return cloudAgentSessionsUrl(cloud.relayRepoOwner(), cloud.name,
                                     cloud.sessionId);
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/orgs/") +
                repoSegment(cloud.relayRepoOwner(), QStringLiteral("owner")) +
                QStringLiteral("/repos/") +
                repoSegment(cloud.name, QStringLiteral("repository")) +
                QStringLiteral("/agent-bots/") + cloud.sessionId);
    return url;
}

// A desktop that launched normally proved it holds the account's key and was
// given no session token, so the key signs the request: the operation, the
// account, the exact path, the digest of a follow-up, and the time. A desktop
// that cannot sign for the account but did log in with a password may still
// read with its token; writes need the signature, because the relay's token
// path is the website's and checks a browser origin this client does not have.
bool MainWindow::authenticateCloudAgentRequest(QUrl &url, QNetworkRequest &request,
                                               const QString &proof,
                                               const QString &prompt) const
{
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QString signer = accountOwner().trimmed().toLower();
    if (!signer.isEmpty() && hasOwnerSigningCapability(signer)) {
        const QString stamp =
            QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString signature = m_profileIdentity.signData(
            forkmesh::agents::cloudAgentProofCanonical(
                proof, signer, url.path(QUrl::FullyEncoded), prompt, stamp));
        if (!signature.isEmpty()) {
            QUrlQuery query(url);
            query.addQueryItem(QStringLiteral("user"), signer);
            query.addQueryItem(QStringLiteral("ts"), stamp);
            query.addQueryItem(QStringLiteral("sig"), signature);
            url.setQuery(query);
            request.setUrl(url);
            return true;
        }
    }
    if (proof != forkmesh::agents::cloudAgentListProof() &&
        proof != forkmesh::agents::cloudAgentAccountListProof())
        return false;
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (sessionToken.isEmpty())
        return false;
    request.setUrl(url);
    request.setRawHeader("Authorization",
                         QByteArrayLiteral("Bearer ") + sessionToken.toUtf8());
    return true;
}

void MainWindow::refreshCloudAgentSessions(bool force)
{
    if (!m_networkAccess || (!m_agentTable && !forkmesh::agentfeed::watched()))
        return;
    // The account's runs, not the open repository's: a mirror works whichever
    // repository the job named, which is hardly ever the one on screen. One
    // read answers for all of them, so the list, the Agents badge and the
    // dot matrix know about a cloud run wherever the reader happens to be.
    const QString account = accountOwner().trimmed();
    if (account.isEmpty())
        return;
    if (m_cloudAgentFetchesInFlight.contains(kCloudAccountKey))
        return;
    bool cloudRunning = false;
    for (const CloudAgentSession &cloud : std::as_const(m_cloudAgentSessions))
        if (cloud.status == QLatin1String("running") ||
            cloud.status == QLatin1String("queued"))
            cloudRunning = true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool watching = forkmesh::agentfeed::watched();
    const qint64 maxAge =
        cloudRunning
            ? (watching ? kCloudAgentWatchRefreshMs : kCloudAgentActiveRefreshMs)
            : kCloudAgentRefreshMs;
    if (!force && now - m_cloudAgentFetchedAtMs.value(kCloudAccountKey) < maxAge)
        return;

    QUrl url = cloudAgentAccountSessionsUrl();
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(
            url, request, forkmesh::agents::cloudAgentAccountListProof()))
        return;
    m_cloudAgentFetchesInFlight.insert(kCloudAccountKey);
    m_cloudAgentFetchedAtMs.insert(kCloudAccountKey, now);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_cloudAgentFetchesInFlight.remove(kCloudAccountKey);
        // A failed read keeps the rows it had: the list going empty because
        // the relay hiccuped would read as the cloud runs being gone.
        if (!networkOk || !payload.value(QStringLiteral("ok")).toBool())
            return;
        applyCloudAgentAccountSessions(
            forkmesh::agents::parseCloudAgentAccountSessions(
                payload, machineNodeName()),
            forkmesh::agents::cloudAgentAccountFinishedBefore(payload));
    });
}

void MainWindow::applyCloudAgentSessions(
    const QString &owner, const QString &name,
    const QList<CloudAgentSession> &sessions)
{
    applyCloudAgentSessionSet(sessions, cloudRepoKey(owner, name));
}

void MainWindow::applyCloudAgentAccountSessions(
    const QList<CloudAgentSession> &sessions, qint64 finishedBeforeMs)
{
    applyCloudAgentSessionSet(sessions, QString(), finishedBeforeMs);
}

// `repoKey` is what the answer speaks for: one repository, or every one of
// them when it is empty. Rows inside that scope which the answer no longer
// lists were deleted somewhere else and go with it.
void MainWindow::applyCloudAgentSessionSet(
    const QList<CloudAgentSession> &rawSessions, const QString &repoKey,
    qint64 finishedBeforeMs)
{
    const bool firstAnswer = !m_cloudAgentAnswered;
    m_cloudAgentAnswered = true;
    loadRetiredCloudAgentSessions();
    QSet<int> kept;
    QList<CloudAgentSession> arrivals;
    bool changed = false;
    QList<CloudAgentSession> sessions = rawSessions;
    QList<QPair<QString, QString>> localRepos;
    for (const RepositoryRecord &repo : std::as_const(m_repositories))
        localRepos.append({repo.owner, repo.name});
    forkmesh::agents::fileCloudAgentSessionsUnderLocalRepos(sessions,
                                                           localRepos);
    for (const CloudAgentSession &cloud : std::as_const(sessions)) {
        // A run this desktop took off the queue itself is already in the list
        // as one of its own sessions, with the live transcript, the worktree
        // and the branch behind it. Show that row, not the relay's shadow of
        // it. Judged by the link the local session keeps, never by the node
        // name on the relay's row: two nodes of one account can share a name,
        // and a run aimed here that nothing has started yet must still show.
        if (localSessionForCloudSession(cloud.sessionId) > 0 ||
            m_cloudRetiredSessionIds.contains(cloud.sessionId))
            continue;
        const QString key = cloud.key();
        int id = m_cloudAgentIds.value(key, 0);
        if (id == 0) {
            id = m_nextCloudAgentId--;
            m_cloudAgentIds.insert(key, id);
            changed = true;
            // A run this desktop has never seen, started somewhere else and
            // already under way: worth saying out loud, because the Agents
            // list only shows the repository that is open.
            if (!firstAnswer &&
                (cloud.status == QLatin1String("running") ||
                 cloud.status == QLatin1String("queued")))
                arrivals.append(cloud);
        }
        kept.insert(id);
        const auto existing = m_cloudAgentSessions.constFind(id);
        if (existing == m_cloudAgentSessions.constEnd() ||
            existing->updatedAtMs != cloud.updatedAtMs ||
            existing->status != cloud.status ||
            existing->history.size() != cloud.history.size() ||
            existing->prNumber != cloud.prNumber ||
            existing->branchName != cloud.branchName ||
            existing->merged != cloud.merged ||
            existing->diagnostic != cloud.diagnostic)
            changed = true;
        // New turns are this run's stream: played and drawn like a local
        // run's. Only what is new - a row seen for the first time is history.
        QList<QJsonObject> fresh;
        if (existing != m_cloudAgentSessions.constEnd() &&
            cloud.history.size() > existing->history.size()) {
            const int seen =
                forkmesh::agents::cloudAgentTranscriptEvents(*existing).size();
            fresh = forkmesh::agents::cloudAgentTranscriptEvents(cloud).mid(seen);
        }
        m_cloudAgentSessions.insert(id, cloud);
        for (const QJsonObject &event : std::as_const(fresh))
            feedAgentActivity(id, event);
    }
    for (auto it = m_cloudAgentSessions.begin();
         it != m_cloudAgentSessions.end();) {
        if ((repoKey.isEmpty() ||
             cloudRepoKey(it->owner, it->name) == repoKey) &&
            !kept.contains(it.key()) &&
            !forkmesh::agents::cloudAgentBeyondListedWindow(
                it.value(), finishedBeforeMs)) {
            m_cloudAgentIds.remove(it->key());
            m_cloudAgentDetailFetched.remove(it.key());
            if (m_selectedAgentSessionId == it.key())
                m_selectedAgentSessionId = -1;
            it = m_cloudAgentSessions.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    syncCloudAgentBranches();
    fetchCloudAgentImages();
    if (!changed)
        return;
    injectExternalSessions(); // rebuilds the cloud rows too
    refreshAgentTable();
    refreshAgentDotMatrix();
    updateAgentsNavBadge();
    if (isCloudSession(m_selectedAgentSessionId))
        showAgentSession(m_selectedAgentSessionId);
    announceCloudAgentArrivals(arrivals);
    pumpCloudAgentMoves();
    pumpCloudAgentStream();
}

// What a reader looking at something else gets to see: one line naming the
// mirror and the repository, so a run started from the website or a phone is
// not a thing you only find by opening the right repository's Agents tab.
void MainWindow::announceCloudAgentArrivals(
    const QList<CloudAgentSession> &arrivals)
{
    if (arrivals.isEmpty())
        return;
    m_pendingCloudArrivals.append(arrivals);
    if (m_cloudArrivalFlushScheduled)
        return;
    m_cloudArrivalFlushScheduled = true;
    QTimer::singleShot(10000, this, [this] { flushCloudAgentArrivals(); });
}

void MainWindow::flushCloudAgentArrivals()
{
    m_cloudArrivalFlushScheduled = false;
    const QList<CloudAgentSession> arrivals = m_pendingCloudArrivals;
    m_pendingCloudArrivals.clear();
    if (arrivals.isEmpty())
        return;
    if (arrivals.size() > 1) {
        int running = 0;
        for (const CloudAgentSession &cloud : arrivals)
            if (cloud.status == QLatin1String("running"))
                ++running;
        const int queued = arrivals.size() - running;
        QStringList parts;
        if (queued > 0)
            parts << QStringLiteral("%1 queued").arg(queued);
        if (running > 0)
            parts << QStringLiteral("%1 running").arg(running);
        flashMessage(QStringLiteral("%1 new cloud agent runs (%2).")
                         .arg(arrivals.size())
                         .arg(parts.join(QStringLiteral(", "))));
        return;
    }
    const CloudAgentSession &cloud = arrivals.first();
    const QString title =
        cloud.title.trimmed().isEmpty()
            ? QStringLiteral("A cloud agent")
            : QStringLiteral("\"%1\"").arg(
                  forkmesh::agents::cloudAgentShortTitle(cloud.title, 60));
    const QString node = cloud.targetNode.isEmpty() ? QStringLiteral("the cloud")
                                                    : cloud.targetNode;
    const QString verb = cloud.status == QLatin1String("running")
                             ? QStringLiteral("is running on")
                             : QStringLiteral("is queued for");
    flashMessage(QStringLiteral("%1 %2 %3 in %4/%5.")
                     .arg(title, verb, node, cloud.owner, cloud.name));
}

QList<int> MainWindow::movableCloudAgentSessionIds() const
{
    QList<QPair<qint64, int>> ordered;
    for (auto it = m_cloudAgentSessions.constBegin();
         it != m_cloudAgentSessions.constEnd(); ++it) {
        if (it->onRunner || m_cloudMovedSessionIds.contains(it->sessionId) ||
            m_cloudAgentWritesInFlight.contains(it.key()) ||
            localSessionForCloudSession(it->sessionId) > 0 ||
            cloudAgentLocalRepoIndex(it.value()) < 0)
            continue;
        ordered.append({it->createdAtMs, it.key()});
    }
    std::sort(ordered.begin(), ordered.end());
    QList<int> ids;
    for (const auto &entry : std::as_const(ordered))
        ids.append(entry.second);
    return ids;
}

bool MainWindow::cloudRunsMoveToDesktop() const
{
    return QSettings().value(QStringLiteral("agents/cloudRunsOnDesktop"), true)
        .toBool();
}

void MainWindow::updateMoveToDesktopButton()
{
    if (!m_agentMoveToDesktopButton)
        return;
    const int waiting = movableCloudAgentSessionIds().size();
    m_agentMoveToDesktopButton->setVisible(waiting > 0);
    m_agentMoveToDesktopButton->setToolTip(
        QStringLiteral("Move the %1 cloud run%2 on the relay to this desktop "
                       "as local agents and empty the web queue")
            .arg(waiting)
            .arg(waiting == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::moveQueuedCloudAgentsHere()
{
    const int count = movableCloudAgentSessionIds().size();
    if (count == 0) {
        flashMessage(QStringLiteral("No cloud run is left on the relay to move "
                                    "here."));
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Move cloud runs to this desktop"),
        QStringLiteral("Move all %1 cloud run%2 to this desktop? Each becomes a "
                       "local agent with its prompt, its conversation so far "
                       "and its task link, and its copy on the relay is "
                       "deleted. They start here within your running-agent "
                       "limit.")
            .arg(count)
            .arg(count == 1 ? QString() : QStringLiteral("s")));
    if (answer != QMessageBox::Yes)
        return;
    QSettings().setValue(QStringLiteral("agents/cloudRunsOnDesktop"), true);
    moveAllCloudAgentsToDesktop();
}

void MainWindow::moveAllCloudAgentsToDesktop()
{
    if (m_cloudMovePumping || !m_networkAccess)
        return;
    if (!hasOwnerSigningCapability(accountOwner()))
        return;
    m_cloudMovePumping = true;
    QStringList moved = QSettings()
                            .value(QStringLiteral("agents/cloudRunsMoved"))
                            .toStringList();
    for (const QString &done : std::as_const(moved))
        m_cloudMovedSessionIds.insert(done);
    QList<int> leftovers;
    for (auto it = m_cloudAgentSessions.constBegin();
         it != m_cloudAgentSessions.constEnd(); ++it) {
        if (m_cloudMovedSessionIds.contains(it->sessionId) &&
            !m_cloudLeftoverDeletes.contains(it->sessionId) &&
            !m_cloudAgentWritesInFlight.contains(it.key())) {
            m_cloudLeftoverDeletes.insert(it->sessionId);
            leftovers.append(it.key());
        }
    }
    for (int leftover : std::as_const(leftovers))
        requestCloudAgentSessionDelete(leftover);
    int started = 0;
    for (int sessionId : movableCloudAgentSessionIds()) {
        const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
        const int repoIndex = cloudAgentLocalRepoIndex(cloud);
        const bool ran = cloud.localAgentId > 0 || cloud.history.size() > 1 ||
                         cloud.status != QLatin1String("queued");
        const QString prompt =
            ran ? forkmesh::agents::cloudAgentContinuationPrompt(cloud)
                : forkmesh::agents::cloudAgentOriginalPrompt(cloud);
        if (prompt.trimmed().isEmpty())
            continue;
        const int localId = startAdHocAgentForRepo(
            repoIndex, prompt,
            cloud.provider.isEmpty() ? QStringLiteral("claude-code")
                                     : cloud.provider,
            cloud.createPr, cloud.model, cloud.title, /*genie=*/false,
            /*switchToTab=*/false, cloud.taskId);
        if (localId <= 0)
            continue;
        ++started;
        m_cloudMovedSessionIds.insert(cloud.sessionId);
        moved.append(cloud.sessionId);
        requestCloudAgentSessionDelete(sessionId);
    }
    while (moved.size() > 500)
        moved.removeFirst();
    QSettings().setValue(QStringLiteral("agents/cloudRunsMoved"), moved);
    m_cloudMovePumping = false;
    if (started > 0)
        flashMessage(QStringLiteral("Moved %1 cloud run%2 to this desktop.")
                         .arg(started)
                         .arg(started == 1 ? QString() : QStringLiteral("s")));
    updateMoveToDesktopButton();
}

void MainWindow::pumpCloudAgentMoves()
{
    if (cloudRunsMoveToDesktop() && !movableCloudAgentSessionIds().isEmpty())
        moveAllCloudAgentsToDesktop();
    else
        updateMoveToDesktopButton();
}

// Called from injectExternalSessions(), which has just dropped every row in the
// external band — the cloud rows with them.
void MainWindow::injectCloudAgentSessions()
{
    for (auto it = m_cloudAgentSessions.constBegin();
         it != m_cloudAgentSessions.constEnd(); ++it)
        m_agentSessions.append(
            forkmesh::agents::cloudAgentListEntry(it.value(), it.key()));
}

// The account-wide list carries the last few turns of every run, which is the
// whole conversation for most of them. Opening one that had more reads the
// rest from the session's own route, once, and re-draws when it lands.
void MainWindow::fetchCloudAgentDetail(int sessionId)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd() || !it->historyTruncated)
        return;
    if (!m_networkAccess || m_cloudAgentDetailFetched.contains(sessionId))
        return;
    const CloudAgentSession cloud = it.value();
    QUrl url = cloudAgentSessionUrl(cloud);
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(url, request,
                                       forkmesh::agents::cloudAgentReadProof()))
        return;
    m_cloudAgentDetailFetched.insert(sessionId);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, owner = cloud.owner, name = cloud.name,
             relayOwner = cloud.relayRepoOwner(),
             organization = cloud.organization] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (!networkOk || !payload.value(QStringLiteral("ok")).toBool()) {
            // Let the next open try again rather than leaving the tail alone
            // forever because the relay was busy once.
            m_cloudAgentDetailFetched.remove(sessionId);
            return;
        }
        const QList<CloudAgentSession> full =
            forkmesh::agents::parseCloudAgentSessions(
                QJsonObject{{QStringLiteral("sessions"),
                             QJsonArray{payload.value(
                                 QStringLiteral("session"))}}},
                owner, name, machineNodeName());
        if (full.isEmpty())
            return;
        const auto row = m_cloudAgentSessions.find(sessionId);
        if (row == m_cloudAgentSessions.end() ||
            row->sessionId != full.first().sessionId)
            return;
        *row = full.first();
        row->relayOwner = relayOwner;
        row->organization = organization;
        m_renderedCloudTurns = -1; // the pane is holding the tail; redraw it
        if (m_selectedAgentSessionId == sessionId)
            renderCloudAgentTranscript(sessionId);
        fetchCloudAgentImages();
    });
}

QString MainWindow::cloudAgentImageDirectory() const
{
    return AgentPromptImages::directory() + QStringLiteral("/cloud");
}

void MainWindow::fetchCloudAgentImages()
{
    if (!m_networkAccess)
        return;
    const QString directory = cloudAgentImageDirectory();
    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kCloudAgentImageKeepMs;
    for (auto it = m_cloudAgentSessions.constBegin(); it != m_cloudAgentSessions.constEnd();
         ++it) {
        const CloudAgentSession &cloud = it.value();
        for (const forkmesh::agents::CloudAgentTurn &turn : cloud.history) {
            if (turn.atMs > 0 && turn.atMs < oldest)
                continue;
            for (const forkmesh::agents::CloudAgentImage &image : turn.images) {
                if (m_cloudAgentImagesInFlight.size() >= kCloudAgentImageFetches)
                    return;
                const QString name = forkmesh::agents::cloudAgentImageFileName(cloud, image);
                if (name.isEmpty() || m_cloudAgentImagesInFlight.contains(name) ||
                    m_cloudAgentImagesGone.contains(name) ||
                    QFileInfo::exists(QDir(directory).filePath(name)))
                    continue;
                QUrl url = cloudAgentSessionUrl(cloud);
                url.setPath(url.path() + QStringLiteral("/attachments/") + image.id);
                QNetworkRequest request(url);
                if (!authenticateCloudAgentRequest(url, request,
                                                   forkmesh::agents::cloudAgentReadProof())) {
                    m_cloudAgentImagesGone.insert(name);
                    continue;
                }
                m_cloudAgentImagesInFlight.insert(name);
                QNetworkReply *reply = m_networkAccess->get(request);
                connect(reply, &QNetworkReply::downloadProgress, reply,
                        [reply, limit = image.size](qint64 received, qint64) {
                    if (received > limit)
                        reply->abort();
                });
                connect(reply, &QNetworkReply::finished, this,
                        [this, reply, name, image, directory, key = cloud.key()] {
                    const int status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const bool networkOk = reply->error() == QNetworkReply::NoError;
                    const bool oversized =
                        reply->error() == QNetworkReply::OperationCanceledError;
                    const QByteArray data = reply->readAll();
                    reply->deleteLater();
                    m_cloudAgentImagesInFlight.remove(name);
                    bool saved = false;
                    if (networkOk &&
                        forkmesh::orgagentjobs::checkImage(image.mediaType, image.size, data)
                            .isEmpty() &&
                        QDir().mkpath(directory)) {
                        QSaveFile file(QDir(directory).filePath(name));
                        saved = file.open(QIODevice::WriteOnly) &&
                                file.write(data) == data.size() && file.commit();
                    }
                    const bool gone =
                        !saved && (networkOk || oversized || status == 403 || status == 404 ||
                                    status == 410);
                    if (gone)
                        m_cloudAgentImagesGone.insert(name);
                    const auto selected = m_cloudAgentSessions.constFind(m_selectedAgentSessionId);
                    if (saved && selected != m_cloudAgentSessions.constEnd() &&
                        selected->key() == key) {
                        m_renderedCloudTurns = -1;
                        renderCloudAgentTranscript(m_selectedAgentSessionId);
                    }
                    if (saved || gone)
                        fetchCloudAgentImages();
                });
            }
        }
    }
}

// ---- "Continue on this desktop" ---------------------------------------------
// The mirror image of "Continue in the cloud". A run queued from the website
// waits for the node it was aimed at, and when that node is not there — turned
// off, no longer serving the repository, no provider CLI on it — the run keeps
// waiting and this list is the only place it can be seen. Taking it over
// claims the very lease the periodic drain would have claimed, so from the
// relay's side the work simply started on another node of the same account:
// the run reports its result, lands its branch and opens its pull request the
// way it would have there, and the website's row follows it here.
//
// Only a run no node has started can be taken. Once one has, the conversation
// lives on that node and the way to reach it is a follow-up.

// The local session that already stands for a website run, or 0. The link is
// durable (AgentSession::webSessionId), so it still answers after the run has
// finished and its lease has been handed back.
int MainWindow::localSessionForCloudSession(const QString &cloudSessionId) const
{
    if (cloudSessionId.trimmed().isEmpty())
        return 0;
    for (const AgentSession &session : m_agentSessions) {
        if (session.cloud || session.id <= 0)
            continue;
        if (session.webSessionId == cloudSessionId)
            return session.id;
    }
    return 0;
}

// The local checkout a queued run could be started in, or -1. Same gate as the
// drain's (startApprovedOrgAgentJob): a record without a working copy on this
// disk is a repository this node only mirrors.
int MainWindow::cloudAgentLocalRepoIndex(const CloudAgentSession &cloud) const
{
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &candidate = m_repositories.at(i);
        if (candidate.previewOnly)
            continue;
        if (candidate.owner.compare(cloud.owner, Qt::CaseInsensitive) == 0 &&
            candidate.name.compare(cloud.name, Qt::CaseInsensitive) == 0 &&
            !candidate.localPath.trimmed().isEmpty())
            return i;
    }
    return -1;
}

// Why this run cannot be continued here, or empty when it can. Offered on
// every cloud row so the answer to "why is this greyed out" is the tooltip;
// kept cheap, because the list asks on every refresh — nothing here shells
// git or the provider CLI, and the click does those checks.
QString MainWindow::cloudAgentContinueHereBlocker(int sessionId) const
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return QStringLiteral("Select a cloud agent session first.");
    const CloudAgentSession &cloud = it.value();
    if (m_cloudAgentWritesInFlight.contains(sessionId))
        return QStringLiteral("This run is already being taken over.");
    if (!cloud.claimable() && !forkmesh::agents::cloudAgentFinished(cloud) &&
        !forkmesh::agents::cloudAgentStoppable(cloud))
        return QStringLiteral("This run is not waiting for a node any more, "
                              "so there is nothing left to take over.");
    if (cloud.organization)
        return QStringLiteral("This run belongs to the %1 organization's "
                              "queue, so the node serving %1/%2 picks it up.")
            .arg(cloud.owner, cloud.name);
    if (!hasOwnerSigningCapability(accountOwner()))
        return QStringLiteral("This device cannot sign for %1, so it cannot "
                              "claim that account's queued agents.")
            .arg(accountOwner());
    if (cloudAgentLocalRepoIndex(cloud) < 0)
        return QStringLiteral("%1/%2 has no working copy on this node, so "
                              "there is nowhere here to run it.")
            .arg(cloud.owner, cloud.name);
    return QString();
}

void MainWindow::continueCloudAgentHere(int sessionId)
{
    const QString blocker = cloudAgentContinueHereBlocker(sessionId);
    if (!blocker.isEmpty()) {
        flashMessage(blocker, true);
        return;
    }
    const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
    if (forkmesh::agents::cloudAgentStoppable(cloud)) {
        stopCloudRunAndContinueHere(sessionId);
        return;
    }
    if (!cloud.claimable()) {
        continueFinishedCloudAgentHere(sessionId);
        return;
    }
    if (!m_networkAccess) {
        flashMessage(QStringLiteral("ForkMesh is offline, so this run cannot "
                                    "be claimed from the relay yet."),
                     true);
        return;
    }
    // Ask before claiming, not after. The lease is the person's queued run:
    // claiming it on a machine with no CLI would fail their session for them,
    // where refusing here leaves it queued for a node that can take it.
    const QJsonObject availability = agentProviderAvailability(cloud.provider);
    if (!availability.value(QStringLiteral("binaryFound")).toBool() ||
        availability.value(QStringLiteral("loginState")).toString() ==
            QLatin1String("missing")) {
        const QString message =
            availability.value(QStringLiteral("message")).toString().trimmed();
        flashMessage(message.isEmpty()
                         ? QStringLiteral("This desktop cannot run %1 agents.")
                               .arg(agentProviderName(cloud.provider))
                         : message,
                     true);
        return;
    }
    QUrl url = cloudAgentSessionsUrl(cloud.relayRepoOwner(), cloud.name,
                                     cloud.sessionId);
    url.setPath(forkmesh::agents::cloudAgentClaimPath(url.path()));
    // The node proof the job queue already uses, not the list proofs: the
    // relay must see the same claimant on a claim as it does on a drain, and
    // answer to it with the same lease.
    const QUrlQuery query = signedInboxQuery(
        repoSegment(accountOwner().trimmed().toLower(), QStringLiteral("owner")));
    if (query.isEmpty()) {
        flashMessage(QStringLiteral("This device could not sign the claim, so "
                                    "the run stays where it is."),
                     true);
        return;
    }
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    m_cloudAgentWritesInFlight.insert(sessionId);
    updateAgentActionState();
    QNetworkReply *reply =
        m_networkAccess->post(request, QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, cloud] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_cloudAgentWritesInFlight.remove(sessionId);
        if (!networkOk || !payload.value(QStringLiteral("ok")).toBool()) {
            const QString message =
                payload.value(QStringLiteral("message")).toString().trimmed();
            flashMessage(
                QStringLiteral("This desktop did not get the run: %1")
                    .arg(message.isEmpty()
                             ? QStringLiteral("the relay refused the claim")
                             : message),
                true);
            // Whatever the relay knows now is newer than what the row says.
            refreshCloudAgentSessions(/*force=*/true);
            updateAgentActionState();
            return;
        }
        finishCloudAgentClaim(sessionId, cloud,
                              payload.value(QStringLiteral("job")).toObject());
    });
}

// The lease is ours. Start the job exactly as a drained one is started — same
// envelope checks, same launch, same result report — and put the new local run
// on screen in place of the cloud row it came from.
void MainWindow::finishCloudAgentClaim(int sessionId,
                                       const CloudAgentSession &cloud,
                                       const QJsonObject &job)
{
    if (job.value(QStringLiteral("sessionId")).toString() != cloud.sessionId) {
        flashMessage(QStringLiteral("The relay answered with a different run, "
                                    "so nothing was started here."),
                     true);
        refreshCloudAgentSessions(/*force=*/true);
        return;
    }
    // Addressed by name: the claim was a round trip, and m_repositories may
    // have been rebuilt under us since the index was resolved.
    RepositoryRecord repo;
    repo.owner = cloud.owner;
    repo.name = cloud.name;
    if (const int index = cloudAgentLocalRepoIndex(cloud); index >= 0)
        repo = m_repositories.at(index);
    applyOrgAgentJobsPayload(repo, QJsonArray{job});
    const int localAgentId = localSessionForCloudSession(cloud.sessionId);
    if (localAgentId <= 0) {
        // startApprovedOrgAgentJob has already told the relay why, and the
        // refreshed row carries that sentence as its diagnostic.
        flashMessage(QStringLiteral("This desktop could not start the run — "
                                    "the session says why."),
                     true);
        refreshCloudAgentSessions(/*force=*/true);
        return;
    }
    // The cloud row has a local session standing for it now; drop it here
    // rather than leave a duplicate until the next poll.
    m_cloudAgentIds.remove(cloud.key());
    m_cloudAgentSessions.remove(sessionId);
    if (!cloud.taskId.isEmpty() && m_agentStore) {
        for (AgentSession &local : m_agentSessions) {
            if (local.id != localAgentId || !local.orgTaskId.isEmpty())
                continue;
            local.orgTask = true;
            local.orgTaskId = cloud.taskId;
            m_agentStore->saveSession(local);
            break;
        }
    }
    const bool bulkMove = m_cloudMovedSessionIds.contains(cloud.sessionId);
    if (m_selectedAgentSessionId == sessionId)
        m_selectedAgentSessionId = localAgentId;
    reloadAgents();
    if (!bulkMove) {
        showAgentSession(localAgentId);
        flashMessage(QStringLiteral("Continuing here as agent #%1.")
                         .arg(localAgentId));
    }
    refreshCloudAgentSessions(/*force=*/true);
}

void MainWindow::continueFinishedCloudAgentHere(int sessionId)
{
    continueCloudRunOnItsBranch(sessionId, m_cloudAgentSessions.value(sessionId),
                                QString());
}

void MainWindow::continueCloudRunOnItsBranch(int sessionId, CloudAgentSession cloud,
                                             const QString &stoppedOn)
{
    const QString provider = cloud.provider.isEmpty()
                                 ? QStringLiteral("claude-code")
                                 : cloud.provider;
    const QJsonObject availability = agentProviderAvailability(provider);
    if (!availability.value(QStringLiteral("binaryFound")).toBool() ||
        availability.value(QStringLiteral("loginState")).toString() ==
            QLatin1String("missing")) {
        const QString message =
            availability.value(QStringLiteral("message")).toString().trimmed();
        flashMessage(message.isEmpty()
                         ? QStringLiteral("This desktop cannot run %1 agents.")
                               .arg(agentProviderName(provider))
                         : message,
                     true);
        return;
    }
    int repoIndex = cloudAgentLocalRepoIndex(cloud);
    if (repoIndex < 0) {
        flashMessage(QStringLiteral("%1/%2 has no working copy on this node.")
                         .arg(cloud.owner, cloud.name),
                     true);
        return;
    }
    if (!stoppedOn.isEmpty() && !forkmesh::agents::cloudAgentFinished(cloud))
        cloud.status = QStringLiteral("stopped");
    const RepositoryRecord local = writableRecordFor(m_repositories.at(repoIndex));
    const QString branch = forkmesh::agents::cloudAgentContinueBranch(cloud);
    m_cloudAgentWritesInFlight.insert(sessionId);
    updateAgentActionState();
    const bool fetched = !branch.isEmpty() &&
                         !local.localPath.trimmed().isEmpty() &&
                         fetchServedBranch(local, branch, /*update=*/true);
    m_cloudAgentWritesInFlight.remove(sessionId);
    const forkmesh::agents::CloudAgentContinuePlan plan =
        forkmesh::agents::cloudAgentContinuePlan(cloud, fetched);
    repoIndex = cloudAgentLocalRepoIndex(cloud);
    if (repoIndex < 0) {
        updateAgentActionState();
        return;
    }
    const QString checkout = m_repositories.at(repoIndex).localPath;
    const bool onBranch =
        plan.start == forkmesh::agents::CloudContinueStart::SessionBranch;
    QString startBranch = plan.startBranch;
    if (!startBranch.isEmpty() &&
        worktreeBranchCommit(checkout, startBranch).isEmpty()) {
        if (onBranch) {
            flashMessage(QStringLiteral("Fetched %1, but it does not resolve "
                                        "to a commit here, so nothing was "
                                        "started.")
                             .arg(startBranch),
                         true);
            updateAgentActionState();
            return;
        }
        startBranch.clear();
    }
    const int localAgentId = startAdHocAgentForRepo(
        repoIndex,
        forkmesh::agents::cloudAgentBranchContinuationPrompt(cloud, plan),
        provider, cloud.createPr, cloud.model,
        forkmesh::agents::cloudAgentContinuationTitle(cloud), /*genie=*/false,
        /*switchToTab=*/true, QString(), QString(), startBranch);
    if (localAgentId <= 0) {
        updateAgentActionState();
        return;
    }
    const QString runner = forkmesh::agents::cloudAgentRunnerName(cloud);
    if (AgentSession *session = findAgentSession(localAgentId)) {
        session->webSessionId = cloud.sessionId;
        if (onBranch && !plan.landingBase.isEmpty() &&
            !worktreeBranchCommit(checkout, plan.landingBase).isEmpty())
            session->baseBranch = plan.landingBase;
        if (m_agentStore) {
            m_agentStore->saveSession(*session);
            if (!stoppedOn.isEmpty())
                m_agentStore->appendLog(
                    *session,
                    QStringLiteral("==> Stopped cloud run %1 on %2 to continue "
                                   "it here.\n")
                        .arg(cloud.sessionId, stoppedOn));
            m_agentStore->appendLog(
                *session,
                onBranch
                    ? QStringLiteral("==> Continuing cloud run %1 from %2 on "
                                     "top of %3 (upstream %3 at %4).\n")
                          .arg(cloud.sessionId, runner, plan.branch,
                               session->baseRef.left(12))
                    : QStringLiteral("==> Continuing cloud run %1 from %2. "
                                     "%3\n")
                          .arg(cloud.sessionId, runner, plan.notice));
        }
    }
    m_cloudAgentIds.remove(cloud.key());
    m_cloudAgentSessions.remove(sessionId);
    if (m_selectedAgentSessionId == sessionId)
        m_selectedAgentSessionId = localAgentId;
    reloadAgents();
    showAgentSession(localAgentId);
    if (onBranch)
        flashMessage(QStringLiteral("Continuing the cloud run here as agent "
                                    "#%1, on top of %2.")
                         .arg(localAgentId)
                         .arg(plan.branch));
    else
        flashMessage(QStringLiteral("Continuing the cloud run here as agent "
                                    "#%1. %2")
                         .arg(localAgentId)
                         .arg(plan.notice));
}

QString MainWindow::cloudAgentContinueHereTip(int sessionId) const
{
    const QString blocker = cloudAgentContinueHereBlocker(sessionId);
    if (!blocker.isEmpty())
        return blocker;
    const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
    if (cloud.claimable())
        return QStringLiteral("Continue this run on this desktop: claim it "
                              "from the relay, run it in this machine's "
                              "checkout, and report the result back to the "
                              "website");
    if (forkmesh::agents::cloudAgentStoppable(cloud))
        return QStringLiteral("Stop the cloud run and continue here: stop it "
                              "on %1, fetch its branch from the mirror, and "
                              "continue on top of it in this machine's "
                              "checkout")
            .arg(forkmesh::agents::cloudAgentRunnerName(cloud));
    return QStringLiteral("Continue this ended run on this desktop: fetch its "
                          "branch from the mirror and start a local agent on "
                          "top of it with the run's original prompt and its "
                          "conversation so far, linked to the cloud run");
}

void MainWindow::stopCloudRunAndContinueHere(int sessionId)
{
    const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
    const QString runner = forkmesh::agents::cloudAgentRunnerName(cloud);
    auto *box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("agentCloudStopContinueQuestion"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(QStringLiteral("Stop the cloud run and continue here"));
    box->setText(QStringLiteral("%1 has already started this run.").arg(runner));
    box->setInformativeText(
        QStringLiteral("Stop it on %1 and continue here? Its follow-ups still "
                       "waiting there are withdrawn, its branch is fetched "
                       "from the mirror, and a local agent continues on top "
                       "of it with the run's prompt and conversation so far.")
            .arg(runner));
    QPushButton *stop = box->addButton(
        QStringLiteral("Stop the cloud run and continue here"),
        QMessageBox::AcceptRole);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(stop);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, sessionId, stop](QAbstractButton *clicked) {
                if (clicked == stop)
                    postCloudRunStopThenContinue(sessionId);
            });
    box->show();
}

void MainWindow::postCloudRunStopThenContinue(int sessionId)
{
    const QString blocker = cloudAgentContinueHereBlocker(sessionId);
    if (!blocker.isEmpty()) {
        flashMessage(blocker, true);
        return;
    }
    const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
    if (!forkmesh::agents::cloudAgentStoppable(cloud)) {
        continueCloudAgentHere(sessionId);
        return;
    }
    if (!m_networkAccess) {
        flashMessage(QStringLiteral("ForkMesh is offline, so the cloud run "
                                    "cannot be stopped yet."),
                     true);
        return;
    }
    const QString provider = cloud.provider.isEmpty()
                                 ? QStringLiteral("claude-code")
                                 : cloud.provider;
    const QJsonObject availability = agentProviderAvailability(provider);
    if (!availability.value(QStringLiteral("binaryFound")).toBool() ||
        availability.value(QStringLiteral("loginState")).toString() ==
            QLatin1String("missing")) {
        const QString message =
            availability.value(QStringLiteral("message")).toString().trimmed();
        flashMessage(
            QStringLiteral("The cloud run was not stopped: %1")
                .arg(message.isEmpty()
                         ? QStringLiteral("this desktop cannot run %1 agents.")
                               .arg(agentProviderName(provider))
                         : message),
            true);
        return;
    }
    QUrl url = cloudAgentSessionUrl(cloud);
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(url, request,
                                       forkmesh::agents::cloudAgentStopProof())) {
        flashMessage(QStringLiteral("This device cannot sign for %1, so it "
                                    "cannot stop the cloud run.")
                         .arg(accountOwner()),
                     true);
        return;
    }
    const QString runner = forkmesh::agents::cloudAgentRunnerName(cloud);
    m_cloudAgentWritesInFlight.insert(sessionId);
    updateAgentActionState();
    QNetworkReply *reply = m_networkAccess->post(
        request, QByteArrayLiteral("{\"action\":\"stop\"}"));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, cloud, runner] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_cloudAgentWritesInFlight.remove(sessionId);
        if (!networkOk || !payload.value(QStringLiteral("ok")).toBool()) {
            const QString message =
                payload.value(QStringLiteral("message")).toString().trimmed();
            flashMessage(
                QStringLiteral("Could not stop the run on %1, so it was not "
                               "moved: %2")
                    .arg(runner, message.isEmpty()
                                     ? QStringLiteral("the relay refused the "
                                                      "stop")
                                     : message),
                true);
            refreshCloudAgentSessions(/*force=*/true);
            updateAgentActionState();
            return;
        }
        m_cloudAgentWritesInFlight.insert(sessionId);
        readCloudAgentSessionThen(
            cloud, [this, sessionId, runner](const CloudAgentSession &fresh) {
                m_cloudAgentWritesInFlight.remove(sessionId);
                if (localSessionForCloudSession(fresh.sessionId) > 0) {
                    updateAgentActionState();
                    return;
                }
                continueCloudRunOnItsBranch(sessionId, fresh, runner);
            });
    });
}

void MainWindow::readCloudAgentSessionThen(
    const CloudAgentSession &cloud,
    std::function<void(const CloudAgentSession &)> done)
{
    QUrl url = cloudAgentSessionUrl(cloud);
    QNetworkRequest request(url);
    if (!m_networkAccess ||
        !authenticateCloudAgentRequest(url, request,
                                       forkmesh::agents::cloudAgentReadProof())) {
        done(cloud);
        return;
    }
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cloud, done = std::move(done)] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (!networkOk || !payload.value(QStringLiteral("ok")).toBool()) {
            done(cloud);
            return;
        }
        const QList<CloudAgentSession> read =
            forkmesh::agents::parseCloudAgentSessions(
                QJsonObject{{QStringLiteral("sessions"),
                             QJsonArray{payload.value(
                                 QStringLiteral("session"))}}},
                cloud.owner, cloud.name, machineNodeName());
        if (read.isEmpty() || read.first().sessionId != cloud.sessionId) {
            done(cloud);
            return;
        }
        CloudAgentSession fresh = read.first();
        fresh.relayOwner = cloud.relayOwner;
        fresh.organization = cloud.organization;
        done(fresh);
    });
}

QString MainWindow::cloudAgentRetryBlocker(int sessionId) const
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return QStringLiteral("Select a cloud agent session first.");
    const CloudAgentSession &cloud = it.value();
    if (m_cloudAgentWritesInFlight.contains(sessionId))
        return QStringLiteral("This run is already being queued again.");
    if (!forkmesh::agents::cloudAgentActions(cloud).contains(
            forkmesh::agents::CloudAgentAction::RetryInCloud))
        return cloud.claimable()
                   ? QStringLiteral("This run is still waiting for a node.")
                   : QStringLiteral("This run has not ended, so there is "
                                    "nothing to retry yet.");
    if (cloud.organization)
        return QStringLiteral("This run belongs to the %1 organization's "
                              "queue; retry it from the website.")
            .arg(cloud.owner);
    if (forkmesh::agents::cloudAgentQueueRuntime(cloud.provider).isEmpty())
        return QStringLiteral("Only Claude Code and Codex runs can be queued "
                              "in the cloud.");
    if (!hasOwnerSigningCapability(accountOwner()))
        return QStringLiteral("This device cannot sign for %1, so it cannot "
                              "queue work on its agent nodes.")
            .arg(accountOwner());
    return QString();
}

void MainWindow::retryCloudAgentInCloud(int sessionId)
{
    const QString blocker = cloudAgentRetryBlocker(sessionId);
    if (!blocker.isEmpty()) {
        flashMessage(blocker, true);
        return;
    }
    const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
    if (!forkmesh::agents::cloudAgentNeedsRunnerSignIn(cloud)) {
        queueCloudAgentRetry(cloud);
        return;
    }
    auto *box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("agentCloudRetrySignInQuestion"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(QStringLiteral("Retry in cloud"));
    box->setText(QStringLiteral("This run failed because the runner's %1 CLI "
                                "is not signed in.")
                     .arg(agentProviderName(cloud.provider)));
    box->setInformativeText(QStringLiteral(
        "Sign the runner in first, and the run is queued again as soon as the "
        "sign-in succeeds. Retrying now is likely to fail the same way."));
    QPushButton *signIn =
        box->addButton(QStringLiteral("Sign in runner, then retry"),
                       QMessageBox::AcceptRole);
    QPushButton *retry = box->addButton(QStringLiteral("Retry now"),
                                        QMessageBox::DestructiveRole);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(signIn);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, cloud, signIn, retry](QAbstractButton *clicked) {
                if (clicked == retry) {
                    queueCloudAgentRetry(cloud);
                    return;
                }
                if (clicked != signIn)
                    return;
                withRunnerForCloudAgent(
                    cloud, [this, cloud](const QString &runner) {
                        openRunnerAgentLoginTerminal(
                            runner,
                            forkmesh::agents::cloudAgentQueueRuntime(
                                cloud.provider),
                            [this, cloud] { queueCloudAgentRetry(cloud); });
                    });
            });
    box->show();
}

void MainWindow::signInRunnerForCloudAgent(int sessionId)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return;
    const CloudAgentSession cloud = it.value();
    const QString provider =
        forkmesh::agents::cloudAgentQueueRuntime(cloud.provider);
    if (provider.isEmpty())
        return;
    withRunnerForCloudAgent(cloud, [this, provider](const QString &runner) {
        openRunnerAgentLoginTerminal(runner, provider);
    });
}

void MainWindow::queueCloudAgentRetry(const CloudAgentSession &cloud)
{
    const QString prompt = forkmesh::agents::cloudAgentOriginalPrompt(cloud);
    const QString runtime =
        forkmesh::agents::cloudAgentQueueRuntime(cloud.provider);
    if (prompt.isEmpty() || runtime.isEmpty()) {
        flashMessage(QStringLiteral("This run has no prompt to queue again."),
                     true);
        return;
    }
    postCloudAgentQueue(
        cloud.relayRepoOwner(), cloud.name, runtime, cloud.model, cloud.createPr,
        prompt,
        [this](bool ok, const QString &message) {
            if (!ok) {
                flashMessage(
                    QStringLiteral("The cloud did not take the retry: %1")
                        .arg(message),
                    true);
                return;
            }
            flashMessage(message.isEmpty()
                             ? QStringLiteral("Queued the run again in the "
                                              "cloud.")
                             : message);
            refreshCloudAgentSessions(/*force=*/true);
        });
}

bool MainWindow::postCloudAgentQueue(
    const QString &owner, const QString &name, const QString &runtime,
    const QString &model, bool createPr, const QString &prompt,
    std::function<void(bool, const QString &)> done)
{
    if (!m_networkAccess) {
        done(false, QStringLiteral("ForkMesh is offline"));
        return false;
    }
    const QString fromDevice = machineNodeName();
    QUrl url = cloudAgentSessionsUrl(owner, name);
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(
            url, request, forkmesh::agents::cloudAgentQueueProof(),
            forkmesh::agents::cloudAgentQueueText(runtime, model, createPr,
                                                  fromDevice, prompt))) {
        done(false, QStringLiteral("this device cannot sign for %1, so it "
                                   "cannot queue work on its agent nodes")
                        .arg(accountOwner()));
        return false;
    }
    const QJsonObject body{
        {QStringLiteral("prompt"), prompt},
        {QStringLiteral("provider"), runtime},
        {QStringLiteral("model"), model},
        {QStringLiteral("createPr"), createPr},
        {QStringLiteral("fromDevice"), fromDevice},
    };
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const QString message =
            payload.value(QStringLiteral("message")).toString().trimmed();
        if (!networkOk || !payload.value(QStringLiteral("ok")).toBool()) {
            done(false, message.isEmpty()
                            ? QStringLiteral("the relay rejected it")
                            : message);
            return;
        }
        done(true, message);
    });
    return true;
}

// A cloud conversation is short — the relay keeps 80 turns of at most 16 KB —
// so it is drawn in one go, and only when it changed.
namespace {

constexpr int kCloudBranchActionPolls = 90;
constexpr int kCloudBranchActionPollMs = 2000;

QString cloudBranchRequestId()
{
    return QStringLiteral("dk-") +
           QString::number(QRandomGenerator::global()->generate64(), 16) +
           QString::number(QRandomGenerator::global()->generate64(), 16);
}

QString cloudBranchActionText(int httpStatus, const QJsonObject &payload,
                               const QString &fallback)
{
    const QString message =
        payload.value(QStringLiteral("message")).toString().trimmed();
    if (!message.isEmpty())
        return message;
    if (httpStatus == 0)
        return QStringLiteral("ForkMesh could not reach the relay.");
    return fallback;
}

} // namespace

QString MainWindow::cloudAgentBranchActionBlocker(int sessionId) const
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return QStringLiteral("Select a cloud agent session first.");
    if (m_cloudAgentWritesInFlight.contains(sessionId))
        return QStringLiteral("Waiting for the relay to answer the last "
                              "request for this run.");
    if (it->organization)
        return QStringLiteral("An organization's runs are merged from the "
                              "website.");
    if (!m_networkAccess)
        return QStringLiteral("ForkMesh is offline.");
    const QString signer = accountOwner().trimmed().toLower();
    if (signer.isEmpty() || !hasOwnerSigningCapability(signer))
        return QStringLiteral("This device cannot sign for %1, so it cannot "
                              "change the cloud run's branch.")
            .arg(accountOwner());
    return QString();
}

void MainWindow::postCloudAgentBranchAction(
    int sessionId, const QJsonObject &body, int attempt,
    std::function<void(int httpStatus, const QJsonObject &payload)> done)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    const QString text = forkmesh::agents::cloudAgentBranchActionText(body);
    if (it == m_cloudAgentSessions.constEnd() || !m_networkAccess ||
        text.isEmpty() || it->organization) {
        m_cloudAgentWritesInFlight.remove(sessionId);
        updateAgentActionState();
        done(0, QJsonObject());
        return;
    }
    QUrl url = cloudAgentSessionUrl(it.value());
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(
            url, request, forkmesh::agents::cloudAgentBranchProof(), text)) {
        m_cloudAgentWritesInFlight.remove(sessionId);
        updateAgentActionState();
        done(-1, QJsonObject());
        return;
    }
    m_cloudAgentWritesInFlight.insert(sessionId);
    updateAgentActionState();
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, body, attempt, done = std::move(done)] {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const bool processing =
            status == 202 ||
            payload.value(QStringLiteral("status")).toString() ==
                QLatin1String("processing");
        if (processing && attempt + 1 < kCloudBranchActionPolls) {
            QTimer::singleShot(kCloudBranchActionPollMs, this,
                               [this, sessionId, body, attempt, done] {
                postCloudAgentBranchAction(sessionId, body, attempt + 1, done);
            });
            return;
        }
        m_cloudAgentWritesInFlight.remove(sessionId);
        updateAgentActionState();
        done(processing ? 202 : status, payload);
    });
}

void MainWindow::mergeCloudAgentBranch(int sessionId)
{
    const QString blocker = cloudAgentBranchActionBlocker(sessionId);
    if (!blocker.isEmpty()) {
        flashMessage(blocker, true);
        return;
    }
    const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
    const QString branch = cloud.branchName;
    const QString base = cloud.baseBranch.isEmpty() ? QStringLiteral("its base")
                                                    : cloud.baseBranch;
    flashMessage(QStringLiteral("Merging %1 into %2…").arg(branch, base));
    postCloudAgentBranchAction(
        sessionId, forkmesh::agents::cloudAgentMergeBody(cloudBranchRequestId()),
        0, [this, cloud, branch, base](int status, const QJsonObject &payload) {
        if (status == 202) {
            flashMessage(QStringLiteral("A core is still publishing the merge "
                                        "of %1. Press Finish merge to keep "
                                        "waiting for it.")
                             .arg(branch),
                         true);
        } else if (status == 200 &&
                   payload.value(QStringLiteral("ok")).toBool()) {
            const QString text =
                forkmesh::agents::cloudAgentMergeOutcome(payload, branch, base);
            logSystem(QStringLiteral("Agents: %1").arg(text));
            flashMessage(text + QStringLiteral(" Its Actions start on %1; "
                                               "press Actions to follow them.")
                                    .arg(base));
            completeCloudAgentTask(
                cloud, forkmesh::agents::cloudAgentMergedTaskNote(cloud, payload));
        } else {
            flashMessage(
                payload.value(QStringLiteral("error")).toString() ==
                        QLatin1String("merge_conflict")
                    ? forkmesh::agents::cloudAgentMergeOutcome(payload, branch,
                                                               base)
                    : cloudBranchActionText(
                          status, payload,
                          QStringLiteral("%1 was not merged.").arg(branch)),
                true);
        }
        refreshCloudAgentSessions(/*force=*/true);
    });
}

void MainWindow::updateCloudAgentBranch(int sessionId)
{
    const QString blocker = cloudAgentBranchActionBlocker(sessionId);
    if (!blocker.isEmpty()) {
        flashMessage(blocker, true);
        return;
    }
    const CloudAgentSession cloud = m_cloudAgentSessions.value(sessionId);
    const QString branch = cloud.branchName;
    const QString base = cloud.baseBranch.isEmpty() ? QStringLiteral("its base")
                                                    : cloud.baseBranch;
    flashMessage(QStringLiteral("Updating %1 from %2…").arg(branch, base));
    postCloudAgentBranchAction(
        sessionId,
        forkmesh::agents::cloudAgentUpdateBody(cloudBranchRequestId()), 0,
        [this, branch, base](int status, const QJsonObject &payload) {
        const bool ok =
            status == 200 && payload.value(QStringLiteral("ok")).toBool();
        if (status == 202)
            flashMessage(QStringLiteral("A core is still updating %1. Press "
                                        "Pull again to keep waiting for it.")
                             .arg(branch),
                         true);
        else if (ok)
            flashMessage(cloudBranchActionText(
                status, payload,
                QStringLiteral("Updated %1 from %2.").arg(branch, base)));
        else
            flashMessage(
                payload.value(QStringLiteral("error")).toString() ==
                        QLatin1String("merge_conflict")
                    ? forkmesh::agents::cloudAgentMergeOutcome(payload, branch,
                                                               base)
                    : cloudBranchActionText(
                          status, payload,
                          QStringLiteral("%1 was not updated.").arg(branch)),
                true);
        refreshCloudAgentSessions(/*force=*/true);
    });
}

void MainWindow::openCloudAgentActions(int sessionId)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return;
    QUrl url = catalogApiUrl();
    url.setQuery(QString());
    url.setFragment(QString());
    url.setPath(QStringLiteral("/%1/%2/actions")
                    .arg(it->relayRepoOwner(), it->name));
    QDesktopServices::openUrl(url);
}

void MainWindow::completeCloudAgentTask(const CloudAgentSession &cloud,
                                        const QString &note)
{
    if (cloud.taskId.isEmpty() || !m_networkAccess)
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/tasks/") + cloud.taskId +
                QStringLiteral("/complete"));
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(url, request, kOrgTaskCompleteProof,
                                    cloud.taskId))
        return;
    const QJsonObject body{{QStringLiteral("completionNote"), note}};
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    const QString taskId = cloud.taskId;
    connect(reply, &QNetworkReply::finished, this, [this, reply, taskId] {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status < 200 || status >= 300)
            logSystem(QStringLiteral("Task %1 was not marked merged (relay "
                                     "replied %2).")
                          .arg(taskId)
                          .arg(status));
    });
}

void MainWindow::cleanUpCloudAgentBranch(int sessionId)
{
    const QString blocker = cloudAgentBranchActionBlocker(sessionId);
    if (!blocker.isEmpty()) {
        flashMessage(blocker, true);
        return;
    }
    const QString branch = m_cloudAgentSessions.value(sessionId).branchName;
    postCloudAgentBranchAction(
        sessionId,
        forkmesh::agents::cloudAgentCleanupBody(cloudBranchRequestId()), 0,
        [this, branch](int status, const QJsonObject &payload) {
        const bool ok =
            status == 200 && payload.value(QStringLiteral("ok")).toBool();
        flashMessage(ok ? cloudBranchActionText(
                              status, payload,
                              QStringLiteral("Cleaned up %1.").arg(branch))
                        : cloudBranchActionText(
                              status, payload,
                              QStringLiteral("%1 was not cleaned up.")
                                  .arg(branch)),
                     !ok);
        refreshCloudAgentSessions(/*force=*/true);
    });
}

void MainWindow::stopCloudAgentRun(int sessionId)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd() ||
        !forkmesh::agents::cloudAgentStoppable(it.value()) || !m_networkAccess)
        return;
    const CloudAgentSession cloud = it.value();
    QUrl url = cloudAgentSessionUrl(cloud);
    QNetworkRequest request(url);
    if (cloud.organization ||
        !authenticateCloudAgentRequest(url, request,
                                       forkmesh::agents::cloudAgentStopProof())) {
        flashMessage(QStringLiteral("This device cannot sign for %1, so it "
                                    "cannot stop the cloud run.")
                         .arg(accountOwner()),
                     true);
        return;
    }
    const QString runner = forkmesh::agents::cloudAgentRunnerName(cloud);
    m_cloudAgentWritesInFlight.insert(sessionId);
    updateAgentActionState();
    QNetworkReply *reply = m_networkAccess->post(
        request, QByteArrayLiteral("{\"action\":\"stop\"}"));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionId, runner] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_cloudAgentWritesInFlight.remove(sessionId);
        const bool ok = networkOk && payload.value(QStringLiteral("ok")).toBool();
        const QString message =
            payload.value(QStringLiteral("message")).toString().trimmed();
        flashMessage(ok ? (message.isEmpty()
                               ? QStringLiteral("Asked %1 to stop the run.")
                                     .arg(runner)
                               : message)
                        : QStringLiteral("Could not stop the run on %1: %2")
                              .arg(runner,
                                   message.isEmpty()
                                       ? QStringLiteral("the relay refused "
                                                        "the stop")
                                       : message),
                     !ok);
        refreshCloudAgentSessions(/*force=*/true);
        updateAgentActionState();
    });
}

void MainWindow::createCloudAgentPull(int sessionId)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return;
    const CloudAgentSession cloud = it.value();
    if (cloud.prNumber > 0) {
        flashMessage(QStringLiteral("This run already has PR #%1.")
                         .arg(cloud.prNumber));
        return;
    }
    const QString blocker = cloudAgentBranchActionBlocker(sessionId);
    if (!blocker.isEmpty()) {
        flashMessage(blocker, true);
        return;
    }
    const QString branch = cloud.branchName.trimmed();
    const int repoIndex = cloudAgentLocalRepoIndex(cloud);
    if (branch.isEmpty() ||
        !forkmesh::localcontrol::handoffBranchValid(branch) || repoIndex < 0) {
        QUrl url = catalogApiUrl();
        url.setQuery(QString());
        url.setFragment(QString());
        url.setPath(QStringLiteral("/%1/%2/agents")
                        .arg(cloud.relayRepoOwner(), cloud.name));
        url.setQuery(QStringLiteral("session=%1").arg(cloud.sessionId));
        QDesktopServices::openUrl(url);
        flashMessage(QStringLiteral("This desktop has no checkout of %1/%2, so "
                                    "the run opened on the website: press "
                                    "Create PR there.")
                         .arg(cloud.owner, cloud.name));
        return;
    }
    const QString key = cloud.key();
    flashMessage(QStringLiteral("Fetching %1 from the mirror…").arg(branch));
    syncCloudAgentBranchesIn(
        repoIndex, cloud.relayRepoOwner(), {branch},
        [this, cloud, branch, key](const QStringList &present, bool) {
            const int index = cloudAgentLocalRepoIndex(cloud);
            if (index < 0 || !present.contains(branch)) {
                flashMessage(QStringLiteral("%1 is not on the mirror yet, so "
                                            "there is nothing to open a pull "
                                            "request from.")
                                 .arg(branch),
                             true);
                return;
            }
            if (!bindRepoDetailToRepo(index))
                return;
            createPullFromBranch(branch, [this, key](int number) {
                if (number > 0)
                    linkCloudAgentPull(key, number);
            });
        });
}

void MainWindow::linkCloudAgentPull(const QString &cloudKey, int number)
{
    const int sessionId = m_cloudAgentIds.value(cloudKey, 0);
    if (!m_cloudAgentSessions.contains(sessionId))
        return;
    postCloudAgentBranchAction(
        sessionId, forkmesh::agents::cloudAgentLinkPullBody(number), 0,
        [this, number](int status, const QJsonObject &payload) {
        if (status != 200 || !payload.value(QStringLiteral("ok")).toBool())
            flashMessage(
                QStringLiteral("Opened pull request #%1, but the cloud run "
                               "could not be linked to it: %2")
                    .arg(number)
                    .arg(cloudBranchActionText(
                        status, payload,
                        QStringLiteral("the relay refused the link"))),
                true);
        refreshCloudAgentSessions(/*force=*/true);
    });
}

void MainWindow::renderCloudAgentTranscript(int sessionId)
{
    if (!m_agentTranscript)
        return;
    fetchCloudAgentDetail(sessionId);
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return;
    const CloudAgentSession &cloud = it.value();
    const qsizetype tailLength =
        cloud.history.isEmpty() ? 0 : cloud.history.last().text.size();
    const int streamed = cloudAgentStreamShownEvents(sessionId);
    if (m_renderedCloudSession == sessionId &&
        m_renderedCloudTurns == cloud.history.size() &&
        m_renderedCloudTailLength == tailLength &&
        m_renderedCloudStreamEvents == streamed &&
        m_renderedExternalSession == sessionId)
        return;
    const bool redraw = m_renderedCloudSession == sessionId &&
                        m_renderedExternalSession == sessionId;
    ++m_agentTranscriptRenderGeneration;
    m_agentTranscriptRenderSession = -1;
    m_agentTranscriptRenderEvents.clear();
    ++m_externalTranscriptRenderGeneration;
    m_externalTranscriptRenderSession = -1;
    m_externalTranscriptRenderEvents.clear();
    if (redraw) {
        m_agentTranscript->setBatchHoldsFrame(true, false);
        m_agentTranscript->setBulkPopulate(true);
    }
    m_agentTranscript->clear();
    if (!redraw)
        m_agentTranscript->setBulkPopulate(false);
    m_renderedTranscriptSession = -1;
    // The shared view now holds a row from the external band; the same marker
    // the external renderer sets tells every other path the view is not theirs.
    m_renderedExternalSession = sessionId;
    m_renderedCloudSession = sessionId;
    m_renderedCloudTurns = cloud.history.size();
    m_renderedCloudTailLength = tailLength;
    const QList<QJsonObject> events =
        forkmesh::agents::cloudAgentTranscriptEvents(cloud, cloudAgentImageDirectory());
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() ==
            QLatin1String("user"))
            m_agentTranscript->addUserTurn(
                event.value(QStringLiteral("message")).toObject()
                    .value(QStringLiteral("content")).toString(),
                ClaudeTranscriptView::eventTime(event));
        else
            m_agentTranscript->handleEvent(event, /*countStats=*/false);
    }
    for (int i = 0; i < streamed; ++i)
        m_agentTranscript->handleEvent(m_cloudAgentStreamEvents.at(i),
                                       /*countStats=*/false);
    m_renderedCloudStreamEvents = streamed;
    if (redraw)
        m_agentTranscript->setBulkPopulate(false);
    else
        m_agentTranscript->jumpToBottom();
}

int MainWindow::cloudAgentStreamShownEvents(int sessionId) const
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd() ||
        m_cloudAgentStream.sessionId() != sessionId ||
        m_cloudAgentStream.key() != it->key() ||
        it->status != QLatin1String("running"))
        return 0;
    return m_cloudAgentStreamEvents.size();
}

QUrl MainWindow::cloudAgentStreamUrl(const CloudAgentSession &cloud) const
{
    QUrl url = cloudAgentSessionUrl(cloud);
    url.setPath(url.path() + QStringLiteral("/stream"));
    return url;
}

void MainWindow::pumpCloudAgentStream(bool rang)
{
    const int id = m_selectedAgentSessionId;
    const auto it = m_cloudAgentSessions.constFind(id);
    const bool cloud =
        isCloudSession(id) && it != m_cloudAgentSessions.constEnd();
    const bool visible = m_agentsPage && m_agentsPage->isVisible() &&
                         m_agentTranscript && !isMinimized();
    const bool changed = m_cloudAgentStream.follow(
        cloud ? id : 0, cloud ? it->key() : QString(),
        cloud && it->status == QLatin1String("running"), visible);
    if (changed) {
        m_cloudAgentStreamEvents.clear();
        if (cloud && m_renderedCloudStreamEvents > 0 &&
            m_renderedCloudSession == id && m_renderedExternalSession == id)
            renderCloudAgentTranscript(id);
    }
    if (rang)
        m_cloudAgentStream.ring();
    if (m_cloudAgentStream.due(QDateTime::currentMSecsSinceEpoch()))
        readCloudAgentStream();
}

void MainWindow::readCloudAgentStream()
{
    const int id = m_cloudAgentStream.sessionId();
    const auto it = m_cloudAgentSessions.constFind(id);
    if (!m_networkAccess || it == m_cloudAgentSessions.constEnd())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QUrl url = cloudAgentStreamUrl(it.value());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("after"),
                       QString::number(m_cloudAgentStream.after()));
    url.setQuery(query);
    QNetworkRequest request(url);
    m_cloudAgentStream.started(now);
    if (!authenticateCloudAgentRequest(url, request,
                                       forkmesh::agents::cloudAgentReadProof())) {
        m_cloudAgentStream.finished(false, QJsonObject(), now);
        return;
    }
    const quint64 generation = m_cloudAgentStream.generation();
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, id, generation] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (generation != m_cloudAgentStream.generation())
            return;
        const qint64 done = QDateTime::currentMSecsSinceEpoch();
        const forkmesh::agents::CloudAgentStreamBatch batch =
            m_cloudAgentStream.finished(
                networkOk && payload.value(QStringLiteral("ok")).toBool(),
                payload, done);
        applyCloudAgentStreamBatch(id, batch);
        if (m_cloudAgentStream.due(done))
            readCloudAgentStream();
    });
}

void MainWindow::applyCloudAgentStreamBatch(
    int sessionId, const forkmesh::agents::CloudAgentStreamBatch &batch)
{
    constexpr int kCloudStreamEventLimit = 2000;
    bool redraw = false;
    if (batch.restarted) {
        m_cloudAgentStreamEvents.clear();
        redraw = true;
    }
    const int before = m_cloudAgentStreamEvents.size();
    const double stamp =
        static_cast<double>(QDateTime::currentMSecsSinceEpoch());
    for (QJsonObject event : batch.events) {
        if (!event.contains(QStringLiteral("_ts")))
            event.insert(QStringLiteral("_ts"), stamp);
        m_cloudAgentStreamEvents.append(event);
    }
    if (m_cloudAgentStreamEvents.size() > kCloudStreamEventLimit) {
        m_cloudAgentStreamEvents.remove(
            0, m_cloudAgentStreamEvents.size() - kCloudStreamEventLimit);
        redraw = true;
    }
    if (m_selectedAgentSessionId != sessionId || !m_agentTranscript ||
        (batch.events.isEmpty() && !redraw))
        return;
    const bool drawn = m_renderedCloudSession == sessionId &&
                       m_renderedExternalSession == sessionId &&
                       m_renderedCloudStreamEvents == before;
    if (redraw || !drawn) {
        renderCloudAgentTranscript(sessionId);
        return;
    }
    if (cloudAgentStreamShownEvents(sessionId) == 0)
        return;
    for (int i = before; i < m_cloudAgentStreamEvents.size(); ++i)
        m_agentTranscript->handleEvent(m_cloudAgentStreamEvents.at(i),
                                       /*countStats=*/false);
    m_renderedCloudStreamEvents = m_cloudAgentStreamEvents.size();
}

void MainWindow::sendCloudAgentFollowUp(int sessionId, const QString &prompt)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return;
    const CloudAgentSession cloud = it.value();
    if (!cloud.promptable()) {
        flashMessage(
            cloud.localAgentId <= 0
                ? QStringLiteral("No node has started this cloud agent yet, so "
                                 "there is nothing to send a follow-up to.")
                : QStringLiteral("This cloud agent was turned away before it "
                                 "ran — start a new one instead."),
            true);
        return;
    }
    if (!m_networkAccess || m_cloudAgentWritesInFlight.contains(sessionId))
        return;
    // Nothing typed means "carry on", the same nudge a local session gets.
    const QString text = prompt.trimmed().isEmpty()
                             ? continueWhereLeftOffPrompt()
                             : prompt.trimmed();
    QUrl url = cloudAgentSessionUrl(cloud);
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(
            url, request, forkmesh::agents::cloudAgentPromptProof(), text)) {
        flashMessage(QStringLiteral("This device cannot sign for %1, so it "
                                    "cannot message its cloud agents.")
                         .arg(accountOwner()),
                     true);
        return;
    }
    m_cloudAgentWritesInFlight.insert(sessionId);
    updateAgentActionState();
    // The signed body is the prompt and nothing else.
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("prompt"), text}})
            .toJson(QJsonDocument::Compact));
    if (m_agentTranscript && sessionId == m_selectedAgentSessionId &&
        m_renderedCloudSession == sessionId &&
        m_renderedExternalSession == sessionId) {
        m_agentTranscript->addUserTurn(text, QDateTime::currentDateTime());
        m_agentTranscript->jumpToBottom();
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, sessionId] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_cloudAgentWritesInFlight.remove(sessionId);
        updateAgentActionState();
        if (!networkOk || !payload.value(QStringLiteral("ok")).toBool()) {
            if (m_renderedCloudSession == sessionId) {
                m_renderedCloudSession = 0;
                if (sessionId == m_selectedAgentSessionId)
                    renderCloudAgentTranscript(sessionId);
            }
            const QString message =
                payload.value(QStringLiteral("message")).toString().trimmed();
            flashMessage(QStringLiteral("The cloud agent did not get that: %1")
                             .arg(message.isEmpty()
                                      ? QStringLiteral("the relay rejected the "
                                                       "follow-up")
                                      : message),
                         true);
            return;
        }
        logSystem(QStringLiteral("Follow-up queued for the cloud agent."));
        refreshCloudAgentSessions(/*force=*/true);
    });
}

bool MainWindow::startQuickAddCloudAgent(int repoIndex, const QString &prompt,
                                         const QString &provider,
                                         const QString &model, bool createPr)
{
    const QString runtime =
        agentIsCodexProvider(provider)
            ? QStringLiteral("codex")
            : (provider == QLatin1String("claude-code") ? QStringLiteral("claude-code")
                                                        : QString());
    if (runtime.isEmpty()) {
        flashMessage(QStringLiteral("Only Claude Code and Codex can run on a "
                                    "ForkMesh agent node. Untick the cloud to "
                                    "run %1 on this desktop.")
                         .arg(agentProviderName(provider)),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    const int resolvedIndex = resolveAgentRepositoryIndex(repoIndex);
    if (resolvedIndex >= 0)
        repoIndex = resolvedIndex;
    if (repoIndex < 0 || repoIndex >= m_repositories.size()) {
        flashMessage(QStringLiteral("Pick a repository before sending a prompt "
                                    "to the cloud."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    if (repo.previewOnly || !repo.publishToNetwork) {
        flashMessage(QStringLiteral("%1/%2 is not published to the network, so "
                                    "no ForkMesh agent node can work on it. "
                                    "Publish it, or untick the cloud to run "
                                    "here.")
                         .arg(repo.owner, repo.name),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    if (!m_networkAccess) {
        flashMessage(QStringLiteral("ForkMesh is offline, so the prompt cannot "
                                    "be sent to the cloud."),
                     true);
        return false;
    }
    return postCloudAgentQueue(
        repo.owner, repo.name, runtime, model, createPr, prompt,
        [this, prompt](bool ok, const QString &message) {
            if (!ok) {
                flashMessage(
                    QStringLiteral("The cloud did not take the prompt: %1")
                        .arg(message),
                    true);
                if (m_issueQuickAdd &&
                    m_issueQuickAdd->toPlainText().isEmpty())
                    m_issueQuickAdd->setPlainText(prompt);
                return;
            }
            const QString started =
                message.isEmpty()
                    ? QStringLiteral("Sent the prompt to the cloud.")
                    : message;
            logSystem(started);
            flyPromptSparkleToAgents();
            refreshCloudAgentSessions(/*force=*/true);
        });
}

void MainWindow::deleteCloudAgentSession(int sessionId)
{
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd())
        return;
    const CloudAgentSession cloud = it.value();
    if (!m_networkAccess || m_cloudAgentWritesInFlight.contains(sessionId))
        return;
    // Not modal, like the local row's question: this is reached from inside the
    // row's own click, and a modal there pumps the loop under the table.
    if (auto *existing =
            findChild<QMessageBox *>(QStringLiteral("agentCloudDeleteConfirm"))) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    const bool live = cloud.status == QLatin1String("running") ||
                      cloud.status == QLatin1String("queued");
    auto *box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("agentCloudDeleteConfirm"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QStringLiteral("Delete cloud agent session"));
    box->setText(QStringLiteral("Delete the cloud agent session \"%1\" on %2?")
                     .arg(cloud.title.isEmpty() ? QStringLiteral("Cloud agent")
                                                : cloud.title,
                          cloud.targetNode.isEmpty() ? QStringLiteral("the cloud")
                                                     : cloud.targetNode));
    box->setInformativeText(
        live ? QStringLiteral("It is removed from the relay for every device. "
                              "A run already in flight on the node is not "
                              "interrupted, and its branch stays where it is.")
             : QStringLiteral("It is removed from the relay for every device. "
                              "Its branch and pull request stay where they "
                              "are."));
    QPushButton *confirm = box->addButton(QStringLiteral("Delete session"),
                                          QMessageBox::DestructiveRole);
    confirm->setObjectName(QStringLiteral("agentCloudDeleteConfirmButton"));
    QPushButton *cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, confirm, sessionId](QAbstractButton *clicked) {
                if (clicked == confirm)
                    requestCloudAgentSessionDelete(sessionId);
            });
    box->show();
}

void MainWindow::requestCloudAgentSessionDelete(int sessionId)
{
    // The list may have been refreshed while the question stood.
    const auto it = m_cloudAgentSessions.constFind(sessionId);
    if (it == m_cloudAgentSessions.constEnd() || !m_networkAccess ||
        m_cloudAgentWritesInFlight.contains(sessionId))
        return;
    const CloudAgentSession cloud = it.value();
    m_cloudAgentWritesInFlight.insert(sessionId);
    const bool sent = sendCloudAgentSessionDelete(
        cloud, [this, sessionId, key = cloud.key()](bool alreadyGone,
                                                   const QString &error) {
            m_cloudAgentWritesInFlight.remove(sessionId);
            if (!error.isEmpty()) {
                flashMessage(
                    QStringLiteral("The cloud agent session was not deleted: %1")
                        .arg(error),
                    true);
                return;
            }
            m_cloudAgentSessions.remove(sessionId);
            m_cloudAgentIds.remove(key);
            if (m_selectedAgentSessionId == sessionId)
                m_selectedAgentSessionId = -1;
            flashMessage(alreadyGone
                             ? QStringLiteral("The relay no longer had this cloud "
                                              "agent session.")
                             : QStringLiteral("Cloud agent session deleted."));
            reloadAgents();
        });
    if (!sent) {
        m_cloudAgentWritesInFlight.remove(sessionId);
        flashMessage(QStringLiteral("This device cannot sign for %1, so it "
                                    "cannot delete its cloud agents.")
                         .arg(accountOwner()),
                     true);
    }
}

bool MainWindow::sendCloudAgentSessionDelete(
    const CloudAgentSession &cloud,
    std::function<void(bool alreadyGone, const QString &error)> done)
{
    if (!m_networkAccess)
        return false;
    QUrl url = cloudAgentSessionUrl(cloud);
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(
            url, request, forkmesh::agents::cloudAgentDeleteProof()))
        return false;
    QNetworkReply *reply = m_networkAccess->deleteResource(request);
    connect(reply, &QNetworkReply::finished, this,
            [reply, done = std::move(done)] {
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const bool alreadyGone =
            forkmesh::agents::cloudAgentDeleteFoundNothing(httpStatus, payload);
        if (!alreadyGone &&
            (!networkOk || !payload.value(QStringLiteral("ok")).toBool())) {
            const QString message =
                payload.value(QStringLiteral("message")).toString().trimmed();
            done(false, message.isEmpty()
                            ? QStringLiteral("the relay rejected the request")
                            : message);
            return;
        }
        done(alreadyGone, QString());
    });
    return true;
}

void MainWindow::loadRetiredCloudAgentSessions()
{
    if (m_cloudRetiredLoaded)
        return;
    m_cloudRetiredLoaded = true;
    const QStringList retired = QSettings()
                                    .value(QStringLiteral("agents/cloudRunsRetired"))
                                    .toStringList();
    for (const QString &sessionId : retired)
        m_cloudRetiredSessionIds.insert(sessionId);
}

int MainWindow::retireMergedCloudAgentSessions(const QString &owner,
                                               const QString &name,
                                               const QString &branch)
{
    if (branch.trimmed().isEmpty())
        return 0;
    loadRetiredCloudAgentSessions();
    const QString repoKey = cloudRepoKey(owner, name);
    QStringList retiring;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.cloud || session.id <= 0 || session.webSessionId.isEmpty() ||
            session.branchName != branch ||
            cloudRepoKey(session.owner, session.name) != repoKey)
            continue;
        retiring.append(session.webSessionId);
    }
    QList<CloudAgentSession> relayDeletes;
    for (auto it = m_cloudAgentSessions.begin();
         it != m_cloudAgentSessions.end();) {
        const bool onBranch = it->branchName == branch &&
                              cloudRepoKey(it->owner, it->name) == repoKey;
        if (!onBranch && !retiring.contains(it->sessionId)) {
            ++it;
            continue;
        }
        if (!retiring.contains(it->sessionId))
            retiring.append(it->sessionId);
        if (!m_cloudAgentWritesInFlight.contains(it.key()))
            relayDeletes.append(it.value());
        m_cloudAgentIds.remove(it->key());
        m_cloudAgentDetailFetched.remove(it.key());
        if (m_selectedAgentSessionId == it.key())
            m_selectedAgentSessionId = -1;
        it = m_cloudAgentSessions.erase(it);
    }
    if (retiring.isEmpty())
        return 0;
    QStringList stored = QSettings()
                             .value(QStringLiteral("agents/cloudRunsRetired"))
                             .toStringList();
    for (const QString &sessionId : std::as_const(retiring)) {
        m_cloudRetiredSessionIds.insert(sessionId);
        if (!stored.contains(sessionId))
            stored.append(sessionId);
    }
    while (stored.size() > 500)
        stored.removeFirst();
    QSettings().setValue(QStringLiteral("agents/cloudRunsRetired"), stored);
    for (const CloudAgentSession &cloud : std::as_const(relayDeletes)) {
        const QString label = cloud.title.isEmpty() ? cloud.sessionId : cloud.title;
        const bool sent = sendCloudAgentSessionDelete(
            cloud, [this, label, branch](bool, const QString &error) {
                if (!error.isEmpty())
                    logSystem(QStringLiteral("Agents: merged cloud run \"%1\" on "
                                             "%2 left the list, but the relay "
                                             "kept it: %3")
                                  .arg(label, branch, error));
            });
        if (!sent)
            logSystem(QStringLiteral("Agents: merged cloud run \"%1\" on %2 left "
                                     "the list; this device cannot sign its "
                                     "delete on the relay.")
                          .arg(label, branch));
    }
    logSystem(QStringLiteral("Agents: merge cleanup took %1 cloud run%2 on %3 off "
                             "the list.")
                  .arg(retiring.size())
                  .arg(retiring.size() == 1 ? QString() : QStringLiteral("s"))
                  .arg(branch));
    injectExternalSessions();
    refreshAgentListAfterRemoval();
    refreshAgentDotMatrix();
    return retiring.size();
}

// The cloud run open in the detail pane, for the composer: a follow-up typed
// under it goes to the relay. 0 when what is on screen is not a cloud run.
int MainWindow::quickAddCloudFollowUpSessionId() const
{
    if (!isCloudSession(m_selectedAgentSessionId) ||
        !m_cloudAgentSessions.contains(m_selectedAgentSessionId))
        return 0;
    const QWidget *agentOutput = m_agentOutputStack;
    return agentOutput && agentOutput->isVisible() ? m_selectedAgentSessionId : 0;
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testApplyCloudAgentPayload(const QString &owner,
                                            const QString &name,
                                            const QJsonObject &payload)
{
    applyCloudAgentSessions(
        owner, name,
        forkmesh::agents::parseCloudAgentSessions(payload, owner, name,
                                                  machineNodeName()));
}

void MainWindow::testApplyCloudAgentAccountPayload(const QJsonObject &payload)
{
    applyCloudAgentAccountSessions(
        forkmesh::agents::parseCloudAgentAccountSessions(payload,
                                                         machineNodeName()),
        forkmesh::agents::cloudAgentAccountFinishedBefore(payload));
}

QList<int> MainWindow::testCloudAgentRowIds() const
{
    QList<int> ids;
    if (!m_agentTable)
        return ids;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *cell = m_agentTable->item(row, 0);
        if (cell && isCloudSession(cell->data(Qt::UserRole).toInt()))
            ids.append(cell->data(Qt::UserRole).toInt());
    }
    return ids;
}

bool MainWindow::testAgentRowIsCloud(int sessionId) const
{
    for (const AgentSession &session : m_agentSessions)
        if (session.id == sessionId)
            return session.cloud;
    return false;
}

QByteArray MainWindow::testCloudAgentAccountSignedUrl() const
{
    QUrl url = cloudAgentAccountSessionsUrl();
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(
            url, request, forkmesh::agents::cloudAgentAccountListProof()))
        return QByteArray();
    return url.toEncoded();
}

QByteArray MainWindow::testCloudAgentSignedUrl(const QString &owner,
                                               const QString &name,
                                               const QString &sessionId,
                                               const QString &proof,
                                               const QString &prompt) const
{
    QUrl url = cloudAgentSessionsUrl(owner, name, sessionId);
    QNetworkRequest request(url);
    if (!authenticateCloudAgentRequest(url, request, proof, prompt))
        return QByteArray();
    return url.toEncoded();
}
#endif

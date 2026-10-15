// Repository sync: the node event socket, the private-mirror control plane,
// the periodic mirror sweep, source convergence, peer mirror updates,
// mentions, network serving and the private/public encrypted replica sync.
// Split out of MainWindowRepos.cpp.

#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowReposInternal.h"
#include "ControlNode.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PrivateMirrorRuntime.h"
#include "PublicMirrorRuntime.h"
#include "UpstreamCheckoutSync.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace forkmesh::ui;
using namespace mainwindowrepos;

namespace {

QString publicIdentityVaultPath()
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/identity/public-age-vault.json");
}

QByteArray publicIdentityVaultSecret(const ForkMeshIdentity &identity)
{
    if (!identity.isValid())
        return {};
    const QByteArray canonical =
        QByteArrayLiteral("forkmesh-public-age-vault-unlock-v1\n") +
        identity.publicKey().toUtf8();
    return QByteArray::fromBase64(
        identity.signData(canonical).toLatin1(),
        QByteArray::Base64UrlEncoding);
}

QString privateOpaqueRepositoryId(const QString &opaqueReplicaId)
{
    if (!PrivateMirrorStore::isOpaqueId(opaqueReplicaId))
        return {};
    return QString::fromLatin1(
               QCryptographicHash::hash(
                   QByteArrayLiteral("forkmesh-private-repository-id-v1\n") +
                       opaqueReplicaId.toUtf8(),
                   QCryptographicHash::Sha256)
                   .toHex())
        .left(32);
}

QString configuredPrivateMirrorNode()
{
    return QSettings()
        .value(QStringLiteral("control/cloudflareNodeName"))
        .toString()
        .trimmed()
        .toLower();
}

struct PrivateSyncWorkerResult {
    PrivateMirrorRuntime::SyncResult sync;
    std::shared_ptr<PrivateMirrorMaterialization> materialization;
    QString error;
    QString notice;
    QString refsSource;
    QString refsFingerprint;
    bool legacyRemoved = true;
};

struct PrivateDownloadWorkerResult {
    QString opaqueId;
    PrivateMirrorStore::Metadata metadata;
    std::shared_ptr<PrivateMirrorMaterialization> materialization;
    QString error;
};

struct PublicSyncWorkerResult {
    PublicMirrorRuntime::Metadata metadata;
    std::shared_ptr<PublicMirrorMaterialization> materialization;
    QString error;
    QString notice;
    QString upstreamSummary;
    bool created = false;
    bool legacyRemoved = true;
    bool reusedOpenMaterialization = false;
};

QString privateControlPlaneKey(const QUrl &catalogUrl,
                               const QString &repositoryKey,
                               const QString &ownerKeyId)
{
    QUrl origin = catalogUrl;
    origin.setPath(QString());
    origin.setQuery(QString());
    origin.setFragment(QString());
    return origin.toString(QUrl::RemoveUserInfo |
                           QUrl::StripTrailingSlash) +
           QLatin1Char('|') + repositoryKey + QLatin1Char('|') + ownerKeyId;
}

} // namespace

// One WebSocket per signed-in owner account to the relay's ForkMeshNodes
// Durable Object. The relay pushes a payload-free event frame the instant a
// source changes. Commit topics fetch the named Git repository directly;
// collaboration topics use the consolidated signed GET /api/sync. The socket
// is the fast path, while reconnect catch-up and a sparse periodic pass cover
// offline nodes and dropped best-effort frames.
void MainWindow::startNodeEventSocket()
{
    if (m_nodeOffline || m_offlineMode) {
        stopNodeEventSocket();
        return;
    }
    if (!hasOwnerSigningCapability()) {
        stopNodeEventSocket();
        return;
    }
    if (m_nodeEventSocket)
        return; // already running; the socket reconnects on its own
    m_nodeEventSocket = new NodeEventSocket(this);
    m_nodeEventSocket->setCoreClient(m_coreClient);
    m_nodeEventSocket->setClientIdentity(
        m_accountName.isEmpty()
            ? QSettings().value(kAccountNameSetting).toString().trimmed()
            : m_accountName,
        machineNodeName());
    m_nodeEventSocket->setConnectionAuthorizer([this](const QUrl &endpoint) {
        return authorizeFirewallConnection(QStringLiteral("WebSocket"),
                                           endpoint);
    });
    // Re-evaluated on every (re)connect attempt so each upgrade carries a
    // freshly-signed drain token and follows account/relay changes.
    m_nodeEventSocket->setUrlFactory([this]() -> QUrl {
        const QString account = m_accountName.isEmpty()
            ? QSettings().value(kAccountNameSetting).toString().trimmed()
            : m_accountName;
        if (!hasOwnerSigningCapability(account) || !m_profileIdentity.isValid())
            return QUrl();
        const QString owner =
            repoSegment(account, QStringLiteral("owner"));
        if (owner.isEmpty())
            return QUrl();
        QUrl url = catalogApiUrl();
        url.setScheme(url.scheme() == QLatin1String("http")
                          ? QStringLiteral("ws")
                          : QStringLiteral("wss"));
        url.setPath(QStringLiteral("/api/nodes/events"));
        url.setQuery(signedInboxQuery(owner));
        return url;
    });
    connect(m_nodeEventSocket, &NodeEventSocket::eventReceived, this,
            [this](const QString &topic, const QString &eventRepo,
                   const QString &targetState) {
                // This DO is per ACCOUNT, so it now carries account-scoped
                // topics alongside the repo ones — and the browser tabs of the
                // same person share the channel. Those topics answer a
                // different question than the repo sync does, so handle them
                // here and return rather than letting a direct message drag a
                // signed /api/sync along behind it.
                if (topic == QLatin1String("pings")) {
                    refreshWebAlerts(true);
                    return;
                }
                if (topic == QLatin1String("direct-messages")) {
                    if (m_officeChannelMirror)
                        m_officeChannelMirror->refreshDirectMessages();
                    return;
                }
                if (topic == QLatin1String("chat-read")) {
                    fetchReadMarks();
                    return;
                }
                // This desktop's own agent-session push rang it, for the
                // phone's Agents tab. Nothing here to fetch.
                if (topic == QLatin1String("agents")) {
                    return;
                }
                if (topic == QLatin1String("desktop-commands")) {
                    onDesktopCommandsEvent(eventRepo);
                    return;
                }
                // A mirror claimed, started or finished one of the account's
                // cloud runs. This is the only notice this desktop gets that a
                // run it is not doing exists — nothing here polls for it — so
                // it is read at once, whichever repository is open.
                if (topic == QLatin1String("cloud-agents")) {
                    refreshCloudAgentSessions(/*force=*/true);
                    return;
                }
                if (topic == QLatin1String("cloud-agent-stream")) {
                    pumpCloudAgentStream(/*rang=*/true);
                    return;
                }
                if (topic.startsWith(QLatin1String("package:"))) {
                    const QString packageId = topic.mid(8);
                    for (const NativePackagePage &page :
                         std::as_const(m_nativePackagePages)) {
                        if (page.id == packageId && page.widget)
                            page.widget->packageChanged();
                    }
                    return;
                }
                // A member of this organization changed the private task
                // board. That catalog is its own signed read, not part of the
                // /api/sync drain, so answer it directly and stop here.
                if (topic == QLatin1String("tasks")) {
                    onOrganizationTaskEvent();
                    return;
                }
                // A catalog record changed for this account (another device
                // published, or the relay accepted an edit). That is a UI
                // freshness signal, not inbox work — refresh the open repo
                // surfaces and skip the signed /api/sync drain entirely.
                if (topic == QLatin1String("catalog")) {
                    if (m_repoDetailIndex >= 0)
                        refreshRepoPinBanner();
                    return;
                }
                // A source catalog publication carries the exact stable-refs
                // generation its mirrors should fetch.  This is repository
                // synchronization, not collaboration inbox work: target only
                // the matching local mirror immediately and do not spend an
                // otherwise-useless GET /api/sync on the relay.  The relay now
                // keeps eventRepo as the source identity even when this socket
                // is authenticated under a linked user account.
                if (topic == QLatin1String("commits")) {
                    const QString wanted = eventRepo.trimmed().toLower();
                    if (!wanted.isEmpty()) {
                        // One queued state per repository coalesces a burst of
                        // pushes. A newer event replaces the older target, and
                        // drainNodeCommitEvent retains it while another fetch
                        // is active instead of losing the wake-up.
                        m_pendingMirrorEventStates.insert(
                            wanted, targetState.trimmed().toLower());
                        drainNodeCommitEvent(wanted);
                    }
                    return;
                }
                // New inbox work also outdates the cached /pending tallies
                // behind the toolbar badges; zeroing clientFetchedAt lets the
                // badge refresh refetch them (fetchMirrorPendingCounts is
                // otherwise fetch-once — the badges are push-driven too).
                for (auto it = m_mirrorPendingCache.begin();
                     it != m_mirrorPendingCache.end(); ++it)
                    it.value().insert(QStringLiteral("clientFetchedAt"), 0);
                scheduleRelaySync();
                refreshPendingInboxBadges();
            });
    connect(m_nodeEventSocket, &NodeEventSocket::agentStatusResult, this,
            &MainWindow::onOrgTaskAgentStatusFrame);
    connect(m_nodeEventSocket, &NodeEventSocket::terminalFrameReceived, this,
            &MainWindow::onNodeTerminalFrame);
    connect(m_nodeEventSocket, &NodeEventSocket::controlFrameReceived, this,
            &MainWindow::onNodeControlFrame);
    connect(m_nodeEventSocket, &NodeEventSocket::heartbeatResult, this,
            &MainWindow::onNodeHeartbeatFrame);
    connect(m_nodeEventSocket, &NodeEventSocket::connectedChanged, this,
            [this](bool connected) {
                // A live socket makes commit events the normal sync path, so
                // stretch the periodic timer into a sparse lost-frame safety
                // pass.  If the socket drops, restore the operator's shorter
                // configured cadence until automatic reconnect succeeds.
                restartMirrorSyncTimer();
                if (connected)
                    m_nodeHeartbeatSocketFailed = false;
                if (connected) {
                    onNodeControlRelayRestored();
                    scheduleRelaySync();
                    refreshWebAlerts(true);
                    fetchReadMarks();
                    // Task pushes sent while the link was down are gone, so
                    // re-read the board on reconnect. Not on the first
                    // connect of a session: the launch pull already covers it.
                    if (m_nodeEventSocketDropped)
                        onOrganizationTaskEvent();
                    m_nodeEventSocketDropped = false;
                    drainAllDesktopCommands();
                } else {
                    m_nodeEventSocketDropped = true;
                    retryNodeHeartbeatOverHttp();
                    requeueOrgTaskAgentStatusInFlight();
                }
            });
    connect(m_nodeEventSocket, &NodeEventSocket::systemMessage, this,
            [this](const QString &text) { logSystem(text); });
    m_nodeEventSocket->start();
}

void MainWindow::drainNodeCommitEvent(const QString &repositoryKey)
{
    const QString wanted = repositoryKey.trimmed().toLower();
    if (wanted.isEmpty() || !m_pendingMirrorEventStates.contains(wanted))
        return;

    const QString wantedName = wanted.section(QLatin1Char('/'), 1, 1);
    QList<int> nameMatches;
    int exactMatch = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &candidate = m_repositories.at(i);
        if (candidate.previewOnly)
            continue;
        const QString name =
            repoSegment(candidate.name, QStringLiteral("repository"));
        if (name.compare(wantedName, Qt::CaseInsensitive) == 0)
            nameMatches.append(i);
        const QString publicKey =
            (repoSegment(candidate.owner, QStringLiteral("owner")) +
             QLatin1Char('/') + name).toLower();
        const QString canonicalKey =
            (catalogOwner(candidate) + QLatin1Char('/') + name).toLower();
        if (wanted == publicKey || wanted == canonicalKey)
            exactMatch = i;
    }
    // Legacy relays addressed a linked user's event as user/repo. Preserve a
    // safe rolling-upgrade path only when that name is unambiguous locally.
    if (exactMatch < 0 && nameMatches.size() == 1)
        exactMatch = nameMatches.first();
    if (exactMatch < 0) {
        m_pendingMirrorEventStates.remove(wanted);
        m_mirrorEventRetryScheduled.remove(wanted);
        return;
    }
    if (m_syncingRepos.contains(exactMatch)) {
        if (!m_mirrorEventRetryScheduled.contains(wanted)) {
            m_mirrorEventRetryScheduled.insert(wanted);
            QTimer::singleShot(1000, this, [this, wanted] {
                m_mirrorEventRetryScheduled.remove(wanted);
                drainNodeCommitEvent(wanted);
            });
        }
        return;
    }

    if (m_mirrorEventHashChecks.contains(wanted))
        return;
    const RepositoryRecord candidate = m_repositories.at(exactMatch);
    const QString state = m_pendingMirrorEventStates.take(wanted);
    m_mirrorEventRetryScheduled.remove(wanted);
    if (state.size() == 64) {
        m_mirrorEventHashChecks.insert(wanted);
        runOffThread<QString>(
            [mirrorPath = candidate.mirrorPath] {
                return mirrorStateHash(mirrorPath);
            },
            [this, wanted, state](QString hash) {
                m_mirrorEventHashChecks.remove(wanted);
                if (hash != state && !m_pendingMirrorEventStates.contains(wanted))
                    m_pendingMirrorEventStates.insert(wanted, QString());
                drainNodeCommitEvent(wanted);
            });
        return;
    }
    if (isSourceOfTruthRepo(candidate))
        convergeSourceRepoFromMesh(exactMatch, true);
    else
        syncRepository(exactMatch, /*quiet=*/true);
}

void MainWindow::stopNodeEventSocket()
{
    requeueOrgTaskAgentStatusInFlight();
    // A deliberate teardown (sign-out, offline mode) is not a reconnect: end
    // the streamed sign-in rather than leaving a prompt open with nobody able
    // to reach it.
    stopNodeTerminalSession();
    if (!m_nodeEventSocket)
        return;
    m_nodeEventSocket->stop();
    m_nodeEventSocket->deleteLater();
    m_nodeEventSocket = nullptr;
}

void MainWindow::refreshRepositoryMirror(int index, bool quietSync)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(index);
    if (repo.previewOnly)
        return;

    if (!repositorySource(repo).isEmpty()) {
        if (m_syncingRepos.contains(index)) {
            if (!quietSync)
                logSystem("Mirror refresh is already running for " + repo.owner +
                          "/" + repo.name + ".");
            return;
        }
        syncRepository(index, quietSync);
    }
}

void MainWindow::ensurePrivateRepositoryControlPlane(
    int index, bool showDialogOnError)
{
    if (index < 0 || index >= m_repositories.size() || !m_networkAccess)
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    if (!repo.isPrivate ||
        !PrivateMirrorStore::isOpaqueId(repo.privateReplicaId))
        return;
    PrivateMirrorStore::Metadata metadata;
    QString localError;
    if (!PrivateMirrorStore::inspectReplica(
            privateReplicaRoot(), repo.privateReplicaId, &metadata,
            &localError)) {
        if (showDialogOnError)
            flashMessage(QStringLiteral(
                             "Private publication is blocked: the encrypted "
                             "replica could not be authenticated."),
                         true);
        return;
    }
    const QString node = configuredPrivateMirrorNode();
    if (node.isEmpty()) {
        const QString message = QStringLiteral(
            "Private publication is blocked until the Control Node page has "
            "the exact Cloudflare mirror node name used by the registered "
            "direct-HTTPS endpoint.");
        logSystem(QStringLiteral("Private mirror: ") + message);
        if (showDialogOnError)
            flashMessage(message, true);
        return;
    }
    const QUrl controlApiBase = catalogApiUrl();
    const QString sessionToken = accountSessionTokenForUrl(controlApiBase);
    if (sessionToken.isEmpty()) {
        const QString message = QStringLiteral(
            "Private publication needs an authenticated owner account session "
            "to register the public encryption key and repository policy. Sign "
            "in from Account settings; private keys remain on this device.");
        logSystem(QStringLiteral("Private mirror: ") + message);
        if (showDialogOnError)
            flashMessage(message, true);
        return;
    }

    QByteArray vaultSecret =
        privateIdentityVaultSecret(m_profileIdentity);
    MirrorCrypto::Identity mirrorIdentity;
    if (!PrivateMirrorRuntime::loadOrCreateIdentity(
            privateIdentityVaultPath(), vaultSecret, &mirrorIdentity,
            &localError)) {
        vaultSecret.fill('\0');
        const QString message = QStringLiteral(
            "Private publication is blocked because the owner-only encryption "
            "identity vault could not be unlocked.");
        logSystem(QStringLiteral("Private mirror: ") + message);
        if (showDialogOnError)
            flashMessage(message, true);
        return;
    }
    vaultSecret.fill('\0');
    vaultSecret.clear();
    const QString ownerKeyId = mirrorIdentity.keyId();
    const QJsonObject publicBundle = mirrorIdentity.publicBundle();
    mirrorIdentity.x25519Priv.fill('\0');
    mirrorIdentity.mlkemPriv.fill('\0');
    mirrorIdentity = {};
    const QString repoKey = catalogOwner(repo) + QLatin1Char('/') +
                            repoSegment(repo.name, QStringLiteral("repository"));
    const QString controlKey =
        privateControlPlaneKey(controlApiBase, repoKey, ownerKeyId);
    if (m_privateControlReady.contains(controlKey)) {
        registerPrivateReplicaRoute(index);
        return;
    }
    if (m_privateControlInFlight.contains(controlKey))
        return;
    m_privateControlInFlight.insert(controlKey);

    auto fail = [this, controlKey, showDialogOnError](
                    const QString &message) {
        m_privateControlInFlight.remove(controlKey);
        logSystem(QStringLiteral("Private mirror: ") + message);
        if (showDialogOnError)
            flashMessage(message, true);
    };

    QUrl ownerKeysUrl = controlApiBase;
    ownerKeysUrl.setPath(QStringLiteral("/api/security/owner-keys"));
    ownerKeysUrl.setQuery(QString());
    ownerKeysUrl.setFragment(QString());
    QNetworkRequest keyRequest(ownerKeysUrl);
    keyRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                         QStringLiteral("application/json"));
    keyRequest.setRawHeader(
        "Authorization",
        QByteArrayLiteral("Bearer ") +
            sessionToken.toUtf8());
    QNetworkReply *keyReply = m_networkAccess->post(
        keyRequest,
        QJsonDocument(QJsonObject{
                          {QStringLiteral("publicBundle"), publicBundle},
                      })
            .toJson(QJsonDocument::Compact));
    connect(
        keyReply, &QNetworkReply::finished, this,
        [this, keyReply, index, repo, repoKey, controlKey, ownerKeyId,
         controlApiBase, sessionToken, showDialogOnError, fail] {
            const QByteArray body = keyReply->readAll();
            const int status =
                keyReply->attribute(
                            QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const QJsonObject response =
                QJsonDocument::fromJson(body).object();
            keyReply->deleteLater();
            if (status < 200 || status >= 300 ||
                !response.value(QStringLiteral("ok")).toBool() ||
                response.value(QStringLiteral("privateKeysStored"))
                    .toBool(true) ||
                response.value(QStringLiteral("keyId")).toString() !=
                    ownerKeyId) {
                fail(QStringLiteral(
                    "Private publication is blocked: the relay did not accept "
                    "the public-only owner encryption key."));
                return;
            }

            QUrl policyUrl = controlApiBase;
            policyUrl.setPath(
                QStringLiteral("/api/repo/") +
                repoSegment(catalogOwner(repo), QStringLiteral("owner")) +
                QLatin1Char('/') +
                repoSegment(repo.name, QStringLiteral("repository")) +
                QStringLiteral("/privacy"));
            policyUrl.setQuery(QString());
            policyUrl.setFragment(QString());
            QNetworkRequest policyRequest(policyUrl);
            policyRequest.setHeader(
                QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
            policyRequest.setRawHeader(
                "Authorization",
                QByteArrayLiteral("Bearer ") +
                    sessionToken.toUtf8());
            QNetworkReply *policyReply = m_networkAccess->post(
                policyRequest,
                QJsonDocument(QJsonObject{
                                  {QStringLiteral("ownerKeyId"),
                                   ownerKeyId},
                              })
                    .toJson(QJsonDocument::Compact));
            connect(
                policyReply, &QNetworkReply::finished, this,
                [this, policyReply, index, repoKey, controlKey, ownerKeyId,
                 showDialogOnError, fail] {
                    const QByteArray policyBody = policyReply->readAll();
                    const int policyStatus =
                        policyReply
                            ->attribute(
                                QNetworkRequest::
                                    HttpStatusCodeAttribute)
                            .toInt();
                    const QJsonObject policy =
                        QJsonDocument::fromJson(policyBody).object();
                    policyReply->deleteLater();
                    if (policyStatus < 200 || policyStatus >= 300 ||
                        !policy.value(QStringLiteral("ok")).toBool() ||
                        policy.value(QStringLiteral("ownerKeyId"))
                                .toString() != ownerKeyId ||
                        !policy
                             .value(QStringLiteral(
                                 "requireMirrorEncryption"))
                             .toBool() ||
                        !policy
                             .value(QStringLiteral("requireAgentE2EE"))
                             .toBool()) {
                        fail(QStringLiteral(
                            "Private publication is blocked: the relay did not "
                            "confirm the mandatory owner-only encryption policy."));
                        return;
                    }
                    m_privateControlInFlight.remove(controlKey);
                    m_privateControlReady.insert(controlKey);
                    if (index >= 0 && index < m_repositories.size())
                        registerPrivateReplicaRoute(index);
                });
        });
}

void MainWindow::registerPrivateReplicaRoute(int index)
{
    if (index < 0 || index >= m_repositories.size() || !m_networkAccess ||
        !m_profileIdentity.isValid())
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    if (!repo.isPrivate ||
        !PrivateMirrorStore::isOpaqueId(repo.privateReplicaId))
        return;
    // The route is registered with the active relay; a private repository that
    // belongs to another relay must not have its owner/name disclosed here.
    if (!repositoryBelongsToActiveRelay(repo))
        return;
    const QString node = configuredPrivateMirrorNode();
    if (node.isEmpty()) {
        logSystem(QStringLiteral(
            "Private mirror: route registration remains blocked until the "
            "configured Cloudflare node name matches a direct-HTTPS endpoint."));
        return;
    }
    PrivateMirrorStore::Metadata metadata;
    QString error;
    if (!PrivateMirrorStore::inspectReplica(
            privateReplicaRoot(), repo.privateReplicaId, &metadata, &error) ||
        metadata.replicaFileSha256.size() != 64) {
        logSystem(QStringLiteral(
            "Private mirror: route registration refused an unauthenticated "
            "local replica."));
        return;
    }
    const qint64 issuedAt =
        QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    const QString owner = catalogOwner(repo);
    const QString name =
        repoSegment(repo.name, QStringLiteral("repository"));
    const QByteArray canonical =
        forkmesh::control::privateReplicaRouteSigningPayload(
            owner, name, node, repo.privateReplicaId,
            metadata.replicaFileSha256, metadata.keyEpoch, true, issuedAt,
            &error);
    const QString signature =
        canonical.isEmpty() ? QString()
                            : m_profileIdentity.signData(canonical);
    if (signature.isEmpty()) {
        logSystem(QStringLiteral(
            "Private mirror: route registration could not create the local "
            "owner signature."));
        return;
    }
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/mirrors/private"));
    url.setQuery(QString());
    url.setFragment(QString());
    const QJsonObject payload{
        {QStringLiteral("owner"), owner},
        {QStringLiteral("repository"), name},
        {QStringLiteral("node"), node},
        {QStringLiteral("opaqueId"), repo.privateReplicaId},
        {QStringLiteral("replicaSha256"),
         metadata.replicaFileSha256},
        {QStringLiteral("keyEpoch"), double(metadata.keyEpoch)},
        {QStringLiteral("active"), true},
        {QStringLiteral("issuedAt"), double(issuedAt)},
        {QStringLiteral("signature"), signature},
    };
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, metadata, expectedOpaqueId = repo.privateReplicaId] {
        const QByteArray body = reply->readAll();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                .toInt();
        const QJsonObject response =
            QJsonDocument::fromJson(body).object();
        reply->deleteLater();
        const QString expectedAccessPath =
            QStringLiteral("/api/private-replicas/") + expectedOpaqueId;
        if (status >= 200 && status < 300 &&
            response.value(QStringLiteral("ok")).toBool() &&
            response.value(QStringLiteral("active")).toBool() &&
            quint64(response.value(QStringLiteral("keyEpoch")).toDouble()) ==
                metadata.keyEpoch &&
            response.value(QStringLiteral("accessPath")).toString() ==
                expectedAccessPath) {
            logSystem(QStringLiteral(
                          "Private mirror: owner-signed opaque HTTPS route is "
                          "active for encrypted epoch %1.")
                          .arg(metadata.keyEpoch));
            return;
        }
        logSystem(QStringLiteral(
            "Private mirror: ciphertext remains local and undiscoverable "
            "because route registration was not accepted. Confirm the exact "
            "Cloudflare node endpoint is registered and healthy."));
    });
}

namespace mainwindowrepos {

// code-health: allow(blocking-wait) local `git for-each-ref`, capped at 5 s; every caller runs it off the GUI thread: convergeSourceRepoFromMesh, startSyncFetch and the private seal worker on QThread::create workers, syncPrivateRepository's quiet-pass gate through runOffThread
QString mirrorRefsDigest(const QString &mirrorPath)
{
    if (!QDir(mirrorPath).exists())
        return QString();
    QProcess p;
    p.start("git", {"-C", mirrorPath, "for-each-ref",
                    "--format=%(objectname) %(refname)"});
    if (!p.waitForFinished(5000))
        return QString();
    return QString::fromUtf8(p.readAllStandardOutput());
}

} // namespace mainwindowrepos

namespace {

// How long an unchanged private repository may stay quiet before the periodic
// pass re-checks the relay's recipient set. A revocation published from another
// device still lands within this window; until then a private repository that
// nobody touched costs no request at all.
constexpr qint64 kPrivateIdleResyncMs = 6 * 60 * 60 * 1000;

// The local Git directory a private replica is sealed from, or empty when the
// only source is a remote URL. Mirrors the source order in
// syncPrivateRepositoryWithRecipients so the idle check and the recorded
// fingerprint always describe the same thing.
QString privateSyncLocalSource(const RepositoryRecord &repo)
{
    const QString local = repo.localPath.trimmed();
    if (!local.isEmpty() &&
        (QDir(local).exists(QStringLiteral(".git")) ||
         QFileInfo(QDir(local).filePath(QStringLiteral("HEAD"))).isFile()))
        return local;
    const QString mirror = repo.mirrorPath.trimmed();
    if (!mirror.isEmpty() && QDir(mirror).exists())
        return mirror;
    return QString();
}

QString privateSyncRefsFingerprint(const QString &localSource)
{
    if (localSource.isEmpty())
        return QString();
    const QString digest = mirrorRefsDigest(localSource);
    if (digest.isEmpty())
        return QString();
    return QString::fromLatin1(
        QCryptographicHash::hash(digest.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

} // namespace

// One repository's half of the sync pass. Split out so the periodic sweep can
// walk the fleet one repository per event-loop turn.
void MainWindow::syncMirrorRepositoryAt(int index)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(index);
    // Local-only private repositories are an explicit no-cloud boundary.
    // Do not even enter private sync from the periodic pass: a stale clone
    // URL or opaque replica id must not turn background maintenance into a
    // relay request that discloses the repository owner/name.
    if (repo.isPrivate && !repo.publishToNetwork)
        return;
    if (repo.previewOnly || repositorySource(repo).isEmpty())
        return;
    // Background sync talks to the active relay and its mirrors. A repository
    // that belongs to another relay waits until that relay is active again.
    if (!repositoryBelongsToActiveRelay(repo))
        return;

    // A source node has two directions to reconcile. Read safe mirror-ahead
    // refs first (issue/pull intake, new branches and fast-forwards), then
    // the completion path fans the resulting canonical generation back to
    // every managed host. Previously this reverse pass ran only when a new
    // roster frame happened to expose a different fingerprint; one failed
    // attempt against that fingerprint was remembered forever, so the
    // periodic "safety" pass could keep publishing the same stale fleet.
    if (isSourceOfTruthRepo(repo) && !repo.mirrorPath.trimmed().isEmpty() &&
        QDir(repo.mirrorPath).exists()) {
        convergeSourceRepoFromMesh(index);
    } else if (!m_syncingRepos.contains(index)) {
        syncRepository(index, /*quiet=*/true);
    }
}

// The pull-ledger sweep (adopt mirror-drained pulls, close branches that
// already landed) otherwise runs only on relay event frames and Pulls-tab
// activity, so a merge landing while the app idles stayed "open" until
// the next frame arrived. This periodic pass bounds that wait; the sweep
// throttles itself per repo. By value: reconcilePullLedgerFor pumps the
// event loop and m_repositories can be reallocated while it does (the
// git-pump UAF family).
void MainWindow::sweepPullLedgerAt(int index, QSet<QString> *swept)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.previewOnly || !repositoryBelongsToActiveRelay(repo))
        return;
    const QString key = repo.owner + "/" + repo.name;
    if (swept) {
        if (swept->contains(key))
            return;
        swept->insert(key);
    }
    reconcilePullLedgerFor(repo);
}

void MainWindow::autoSyncMirrors()
{
    for (int i = 0; i < m_repositories.size(); ++i)
        syncMirrorRepositoryAt(i);

    QSet<QString> swept;
    for (int i = 0; i < m_repositories.size(); ++i)
        sweepPullLedgerAt(i, &swept);
}

// adhoc #2104: the same work, one repository per event-loop turn. Reconciling a
// whole fleet inside one timer tick is seconds of git — convergeSourceRepoFromMesh
// and reconcilePullLedgerFor both read repositories synchronously — and the
// sampled stalls caught it holding the GUI thread from the mirror safety timer.
// Yielding between repositories lets the window paint and stay clickable while
// the sweep walks. The explicit "sync every mirror" control still calls
// autoSyncMirrors() in place, because its caller reports on the result as soon
// as it returns.
void MainWindow::startMirrorSyncSweep()
{
    if (m_mirrorSweepActive)
        return; // a sweep is already walking the fleet; it covers this tick
    m_mirrorSweepActive = true;
    m_mirrorSweepLedgerPhase = false;
    m_mirrorSweepIndex = 0;
    m_mirrorSweepSeen.clear();
    // The routine the timer named is what the action log should attribute every
    // turn of this sweep to, so carry it across the turns.
    m_mirrorSweepRoutine = forkmesh::BackgroundRoutine::current();
    QTimer::singleShot(0, this, &MainWindow::stepMirrorSyncSweep);
}

void MainWindow::stepMirrorSyncSweep()
{
    if (!m_mirrorSweepActive)
        return;
    const forkmesh::BackgroundRoutine routine(m_mirrorSweepRoutine);
    if (!m_mirrorSweepLedgerPhase) {
        if (m_mirrorSweepIndex < m_repositories.size()) {
            syncMirrorRepositoryAt(m_mirrorSweepIndex++);
        } else {
            m_mirrorSweepLedgerPhase = true;
            m_mirrorSweepIndex = 0;
        }
    } else if (m_mirrorSweepIndex < m_repositories.size()) {
        sweepPullLedgerAt(m_mirrorSweepIndex++, &m_mirrorSweepSeen);
    } else {
        m_mirrorSweepActive = false;
        m_mirrorSweepSeen.clear();
        return;
    }
    QTimer::singleShot(0, this, &MainWindow::stepMirrorSyncSweep);
}

bool MainWindow::mirrorHasCommit(const QString &mirrorPath, const QString &commit)
{
    static QSet<QString> present;
    static QMutex presentMutex;
    const QString key = mirrorPath + QLatin1Char('\x1f') + commit;
    {
        QMutexLocker lock(&presentMutex);
        if (present.contains(key))
            return true;
    }
    if (!runGitCapture(mirrorPath,
                       {QStringLiteral("cat-file"), QStringLiteral("-e"),
                        commit + QStringLiteral("^{commit}")},
                       nullptr, nullptr))
        return false;
    QMutexLocker lock(&presentMutex);
    present.insert(key);
    return true;
}

bool MainWindow::isSourceOfTruthRepo(const RepositoryRecord &repo) const
{
    return !repo.isPrivate && repo.publishToNetwork &&
           !repo.localPath.trimmed().isEmpty() &&
           catalogOwner(repo).trimmed().toLower() ==
               accountOwner().trimmed().toLower();
}

// code-health: allow(deep-nested-loop) per repository, walks online roster nodes' adverts and keeps each node's first match; no git or disk reads, those run in readMirrorsBehindRoster on a worker
void MainWindow::syncMirrorsBehindRoster()
{
    if (m_rosterBehindReadInFlight) {
        m_rosterBehindReadPending = true;
        return;
    }
    QList<RosterBehindInput> inputs;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (repo.previewOnly || m_syncingRepos.contains(i))
            continue;
        const bool sourceOfTruth = isSourceOfTruthRepo(repo);
        if (repositorySource(repo).isEmpty() && !sourceOfTruth)
            continue;
        if (repo.mirrorPath.trimmed().isEmpty() || !QDir(repo.mirrorPath).exists())
            continue;
        const QString canonical =
            catalogOwner(repo) + "/" +
            repoSegment(repo.name, QStringLiteral("repository"));
        const QString source = repoSegment(repo.owner, QStringLiteral("owner")) +
                               "/" + repoSegment(repo.name, QStringLiteral("repository"));
        const QString legacy = repo.owner + "/" + repo.name;
        RosterBehindInput input;
        input.index = i;
        input.mirrorPath = repo.mirrorPath;
        input.sourceOfTruth = sourceOfTruth;
        for (const MemberInfo &node : std::as_const(m_homeRoster)) {
            if (node.self || !node.online)
                continue;
            for (const MirrorAdvert &m : node.mirrorDetails) {
                if (m.source != source && m.ownerName != canonical &&
                    m.ownerName != legacy)
                    continue;
                input.adverts.append(m);
                break;
            }
        }
        if (!input.adverts.isEmpty())
            inputs.append(input);
    }
    if (inputs.isEmpty())
        return;
    m_rosterBehindReadInFlight = true;
    m_rosterBehindReadPending = false;
    runOffThread<QList<RosterBehindResult>>(
        [inputs] { return readMirrorsBehindRoster(inputs); },
        [this](QList<RosterBehindResult> results) {
            m_rosterBehindReadInFlight = false;
            for (const RosterBehindResult &result : std::as_const(results)) {
                const int i = result.index;
                if (i < 0 || i >= m_repositories.size() ||
                    m_repositories.at(i).mirrorPath != result.mirrorPath ||
                    m_repositories.at(i).previewOnly || m_syncingRepos.contains(i))
                    continue;
                if (result.behind) {
                    if (isSourceOfTruthRepo(m_repositories.at(i)))
                        convergeSourceRepoFromMesh(i);
                    else
                        syncRepository(i, /*quiet=*/true);
                } else if (result.artifactsBehind)
                    replicateReleaseArtifacts(i);
            }
            if (m_rosterBehindReadPending) {
                m_rosterBehindReadPending = false;
                syncMirrorsBehindRoster();
            }
        });
}

QList<MainWindow::RosterBehindResult>
MainWindow::readMirrorsBehindRoster(const QList<RosterBehindInput> &inputs)
{
    QList<RosterBehindResult> results;
    for (const RosterBehindInput &input : inputs) {
        const RosterBehindInput &repo = input;
        int localArtifactCount = 0;
        bool localArtifactCountDone = false;
        QString localRefsFingerprint;
        bool localRefsFingerprintDone = false;
        bool behind = false;
        bool artifactsBehind = false;
        for (const MirrorAdvert &m : input.adverts) {
            if (!m.refsFingerprint.isEmpty()) {
                if (!localRefsFingerprintDone) {
                    localRefsFingerprint = mirrorStateHash(repo.mirrorPath);
                    localRefsFingerprintDone = true;
                }
                if (m.refsFingerprint != localRefsFingerprint)
                    behind = true;
            } else if (!m.commit.isEmpty() &&
                       !mirrorHasCommit(repo.mirrorPath, m.commit)) {
                behind = true;
            }
            if (!localArtifactCountDone) {
                localArtifactCount = mirrorArtifactCount(repo.mirrorPath);
                localArtifactCountDone = true;
            }
            if (m.artifactCount > localArtifactCount)
                artifactsBehind = true;
            if (behind || artifactsBehind)
                break;
        }
        if (behind || artifactsBehind)
            results.append(RosterBehindResult{input.index, input.mirrorPath,
                                              behind, artifactsBehind});
    }
    return results;
}

static void appendMirrorRoute(QList<forkmesh::upstream::MirrorRoute> &routes,
                              const QString &rawUrl,
                              const QProcessEnvironment &environment,
                              const QString &fallbackGroup)
{
    const QString url = rawUrl.trimmed();
    if (url.isEmpty())
        return;
    for (forkmesh::upstream::MirrorRoute &route : routes) {
        if (route.url != url)
            continue;
        if (route.environment.isEmpty() && !environment.isEmpty())
            route.environment = environment;
        if (route.fallbackGroup.isEmpty() && !fallbackGroup.isEmpty())
            route.fallbackGroup = fallbackGroup;
        return;
    }
    routes.append({url, environment, fallbackGroup});
}

struct ServedBranchTrashRequest {
    QUrl url;
    QByteArray body;
    QByteArray publicKey;
    QByteArray timestamp;
    QByteArray signature;
    QString servedHost;
    QString owner;
    QString name;
};

static bool fetchServedBranchTrash(const ServedBranchTrashRequest &request,
                                   QHash<QString, QStringList> *deleted)
{
    constexpr qint64 kMaxListingBytes = 4 * 1024 * 1024;
    QNetworkAccessManager network;
    QNetworkRequest http(request.url);
    http.setHeader(QNetworkRequest::ContentTypeHeader,
                   QStringLiteral("application/json"));
    http.setRawHeader("X-ForkMesh-Pubkey", request.publicKey);
    http.setRawHeader("X-ForkMesh-Ts", request.timestamp);
    http.setRawHeader("X-ForkMesh-Sig", request.signature);
    http.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                      QNetworkRequest::ManualRedirectPolicy);
    http.setTransferTimeout(20 * 1000);
    QNetworkReply *reply = network.post(http, request.body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const bool answered =
        reply->error() == QNetworkReply::NoError &&
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    const QByteArray body = reply->read(kMaxListingBytes + 1);
    delete reply;
    return answered && body.size() <= kMaxListingBytes &&
           forkmesh::upstream::parseServedBranchTrash(body, request.owner,
                                                      request.name, deleted);
}

void MainWindow::convergeSourceRepoFromMesh(int index, bool force)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    const QString key = repo.owner.trimmed().toLower() + QLatin1Char('/') +
                        repo.name.trimmed().toLower();
    if (m_syncingRepos.contains(index)) {
        if (!m_sourceConvergePending.contains(key)) {
            // A source fetch, inbox commit, or fan-out can own the Git writer
            // when either an explicit refresh or the safety pass arrives.
            // Preserve every reverse refresh across that writer; dropping the
            // periodic one could leave a mirror-ahead branch stale indefinitely.
            m_sourceConvergePending.insert(key);
            QTimer::singleShot(1000, this, [this, key] {
                if (!m_sourceConvergePending.remove(key))
                    return;
                for (int i = 0; i < m_repositories.size(); ++i) {
                    const RepositoryRecord &candidate = m_repositories.at(i);
                    const QString candidateKey =
                        candidate.owner.trimmed().toLower() +
                        QLatin1Char('/') + candidate.name.trimmed().toLower();
                    if (candidateKey == key) {
                        convergeSourceRepoFromMesh(i, /*force=*/true);
                        return;
                    }
                }
            });
        }
        return;
    }
    const QString mirrorPath = repo.mirrorPath.trimmed();
    if (mirrorPath.isEmpty() || !QDir(mirrorPath).exists())
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // The sweep's interval keeps a background pass off the network. An explicit
    // "I opened this page" converges now instead of reporting stale counts for
    // another whole cycle, but keeps a short floor of its own so flipping
    // between tabs cannot turn into a fetch storm.
    constexpr qint64 kForcedConvergeFloorMs = 5000;
    if (nowMs - m_sourceConvergeAttemptMs.value(key, 0) <
        (force ? kForcedConvergeFloorMs : kMirrorSyncIntervalMs))
        return;
    m_sourceConvergeAttemptMs.insert(key, nowMs);
    const QString meshUrl = repositoryNetworkCloneUrl(repo.owner, repo.name);
    const QString workTree = repo.localPath.trimmed();
    QList<forkmesh::upstream::MirrorRoute> mirrorRoutes;
    auto addMirrorRoute = [&mirrorRoutes](
                              const QString &rawUrl,
                              const QProcessEnvironment &environment = {},
                              const QString &fallbackGroup = QString()) {
        appendMirrorRoute(mirrorRoutes, rawUrl, environment, fallbackGroup);
    };
    // The canonical managed product also fans out directly from the saved
    // Hosts inventory, without requiring one Git remote per node. Build the
    // matching authenticated read routes here. A pruning fan-out is permitted
    // only after every one of these exact routes was fetched successfully.
    struct ManagedRouteIdentity {
        QString node;
        QString fallbackGroup;
        QSet<QString> addresses;
    };
    QList<ManagedRouteIdentity> managedRouteIdentities;
    if (repo.owner.compare(QLatin1String("forkmesh"), Qt::CaseInsensitive) == 0 &&
        repo.name.compare(QLatin1String("forkmesh"), Qt::CaseInsensitive) == 0) {
        QSettings settings;
        const QJsonArray hosts = forkmesh::control::loadSavedHosts(
            settings, kHostsSetting, &m_hostSessionPasswords);
        for (const QJsonValue &value : hosts) {
            const QJsonObject saved = value.toObject();
            if (!forkmesh::control::savedHostMayBeActive(saved))
                continue;
            const QString node =
                saved.value(QStringLiteral("name")).toString().trimmed();
            const QString host =
                saved.value(QStringLiteral("ip")).toString().trimmed();
            const QString user =
                saved.value(QStringLiteral("user")).toString().trimmed();
            if (node.isEmpty())
                continue;
            const QString fallbackGroup =
                QStringLiteral("managed-host:") + node.toLower();
            ManagedRouteIdentity identity{node, fallbackGroup, {}};
            for (const QString &address :
                 {host,
                  saved.value(QStringLiteral("dnsHostname"))
                      .toString(),
                  QUrl(saved.value(QStringLiteral("publicOrigin"))
                           .toString())
                      .host()}) {
                const QString normalized = address.trimmed().toLower();
                if (!normalized.isEmpty())
                    identity.addresses.insert(normalized);
            }
            managedRouteIdentities.append(identity);

            // Prefer the already-authenticated host route. A syntactically
            // valid public hostname can still be waiting for DNS, and choosing
            // it unconditionally made every reverse pass wait through resolver
            // timeouts even though the saved host was directly reachable. The
            // public endpoint stays in the same fallback group, so a transient
            // SSH failure does not make a healthy mirror look unreadable.
            if (!host.isEmpty() && user == QLatin1String("root")) {
                const QString credentialKey =
                    forkmesh::control::savedHostCredentialKey(node, host, user);
                QString sshError;
                forkmesh::control::HostSshCommand transport =
                    forkmesh::control::buildHostSshCommand(
                        host, user,
                        m_hostSessionPasswords.value(credentialKey),
                        QStringLiteral("true"), &sshError,
                        savedHostIdentityFile(node, host, user));
                if (!transport.program.isEmpty() &&
                    transport.arguments.size() >= 2) {
                    // GIT_SSH_COMMAND is only the transport prefix. Git appends
                    // the destination and git-upload-pack invocation itself.
                    transport.arguments.removeLast(); // fixed "true" command
                    transport.arguments.removeLast(); // root@host destination
                    transport.arguments << QStringLiteral("-o")
                                        << QStringLiteral(
                                               "ServerAliveInterval=15")
                                        << QStringLiteral("-o")
                                        << QStringLiteral(
                                               "ServerAliveCountMax=2");
                    QProcessEnvironment environment = transport.environment;
                    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"),
                                       QStringLiteral("0"));
                    environment.insert(
                        QStringLiteral("GIT_SSH_COMMAND"),
                        forkmesh::control::hostSshCommandLine(transport));

                    QUrl remote;
                    remote.setScheme(QStringLiteral("ssh"));
                    remote.setUserName(user);
                    remote.setHost(host);
                    remote.setPath(QStringLiteral(
                        "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git"));
                    addMirrorRoute(remote.toString(QUrl::FullyEncoded),
                                   environment, fallbackGroup);
                }
            }

            QUrl publicOrigin(
                saved.value(QStringLiteral("publicOrigin")).toString().trimmed());
            if ((!publicOrigin.isValid() ||
                 publicOrigin.scheme() != QLatin1String("https") ||
                 publicOrigin.host().isEmpty() ||
                 !publicOrigin.userInfo().isEmpty())) {
                publicOrigin = QUrl(QStringLiteral("https://") +
                                    saved.value(QStringLiteral("dnsHostname"))
                                        .toString().trimmed());
            }
            if (publicOrigin.isValid() &&
                publicOrigin.scheme() == QLatin1String("https") &&
                !publicOrigin.host().isEmpty() &&
                publicOrigin.userInfo().isEmpty()) {
                publicOrigin.setPath(QStringLiteral("/%1/%2.git")
                                         .arg(repo.owner, repo.name));
                publicOrigin.setQuery(QString());
                publicOrigin.setFragment(QString());
                addMirrorRoute(publicOrigin.toString(QUrl::FullyEncoded), {},
                               fallbackGroup);
            }
        }
    }
    m_syncingRepos.insert(index, /*quiet=*/true);
    refreshRepositoryList();
    auto mirrorMoved = std::make_shared<bool>(false);
    auto checkoutSummary = std::make_shared<QString>();
    auto reconciliationFetched = std::make_shared<bool>(false);
    auto reconciliationComplete = std::make_shared<bool>(false);
    ServedBranchTrashRequest trashRequest;
    const QString trashSigner = accountOwner().trimmed().toLower();
    QUrl trashEndpoint = catalogApiUrl();
    if (!trashSigner.isEmpty() && hasOwnerSigningCapability(trashSigner) &&
        trashEndpoint.scheme() == QLatin1String("https") &&
        !trashEndpoint.host().isEmpty()) {
        const QString owner = repo.owner.trimmed().toLower();
        const QString name = repo.name.trimmed();
        const QString stamp =
            QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString signature = m_profileIdentity.signData(
            forkmesh::upstream::branchTrashRequestCanonical(owner, name, stamp));
        if (!signature.isEmpty()) {
            trashEndpoint.setPath(QStringLiteral("/api/git/branch-trash"));
            trashEndpoint.setQuery(QString());
            trashEndpoint.setFragment(QString());
            trashRequest = {
                trashEndpoint,
                QJsonDocument(QJsonObject{{QStringLiteral("owner"), owner},
                                          {QStringLiteral("repo"), name}})
                    .toJson(QJsonDocument::Compact),
                m_profileIdentity.publicKey().toUtf8(),
                stamp.toUtf8(),
                signature.toUtf8(),
                QUrl(meshUrl).host().toLower(),
                owner,
                name};
        }
    }
    QThread *worker = QThread::create(
        [mirrorPath, meshUrl, baseRoutes = mirrorRoutes, managedRouteIdentities,
         workTree, mirrorMoved, checkoutSummary, reconciliationFetched,
         reconciliationComplete, trashRequest] {
            QList<forkmesh::upstream::MirrorRoute> mirrorRoutes = baseRoutes;
            auto addMirrorRoute = [&mirrorRoutes](
                                      const QString &rawUrl,
                                      const QProcessEnvironment &environment = {},
                                      const QString &fallbackGroup = QString()) {
                appendMirrorRoute(mirrorRoutes, rawUrl, environment,
                                  fallbackGroup);
            };
            // The public clone is intentionally round-robin and can show only one
            // peer generation per fetch. Read every exact configured SSH remote back
            // too so work committed by different intake winners cannot remain
            // marooned. A remote whose address belongs to a saved host is another
            // access path for that host, not an independently required mirror.
            for (const RepoRemoteRow &remote : readRepoRemotes(workTree)) {
                const QString candidate = remote.pushUrl.trimmed();
                const QUrl candidateUrl(candidate);
                if (candidateUrl.scheme().compare(QStringLiteral("ssh"),
                                                  Qt::CaseInsensitive) != 0) {
                    continue;
                }
                const QString candidateHost = candidateUrl.host().trimmed().toLower();
                QString fallbackGroup;
                for (const ManagedRouteIdentity &identity :
                     std::as_const(managedRouteIdentities)) {
                    if (identity.addresses.contains(candidateHost) ||
                        mirrorGatewayHostBelongsToNode(candidateHost,
                                                       identity.node)) {
                        fallbackGroup = identity.fallbackGroup;
                        break;
                    }
                }
                addMirrorRoute(candidate, {}, fallbackGroup);
            }
            // Exact managed routes cover every fan-out destination and are suitable
            // for the complete-read/prune decision. The public round-robin route is a
            // fallback for repositories without an exact route; adding it after the
            // full managed inventory only introduces a duplicate, potentially stalled
            // fetch and incorrectly makes an otherwise complete pass look partial.
            if (mirrorRoutes.isEmpty())
                addMirrorRoute(meshUrl);
            const auto servedRoute =
                [&trashRequest](const forkmesh::upstream::MirrorRoute &route) {
                    return !trashRequest.servedHost.isEmpty() &&
                           QUrl(route.url).host().toLower() ==
                               trashRequest.servedHost;
                };
            if (trashRequest.url.isValid() &&
                std::any_of(mirrorRoutes.cbegin(), mirrorRoutes.cend(),
                            servedRoute)) {
                QHash<QString, QStringList> servedTrash;
                if (fetchServedBranchTrash(trashRequest, &servedTrash))
                    for (forkmesh::upstream::MirrorRoute &route : mirrorRoutes)
                        if (servedRoute(route))
                            route.deletedBranches = servedTrash;
            }
            const QString before = mirrorRefsDigest(mirrorPath);
            const auto publishCheckoutToSource = [&] {
                return runGitCapture(
                    mirrorPath,
                    {QStringLiteral("fetch"), QStringLiteral("--quiet"),
                     workTree, QStringLiteral("refs/heads/*:refs/heads/*"),
                     QStringLiteral("refs/tags/*:refs/tags/*")},
                    nullptr, nullptr);
            };
            if (!workTree.isEmpty()) {
                const forkmesh::upstream::RefreshOutcome outcome =
                    forkmesh::upstream::convergeSourceCheckoutFromRoutes(
                        workTree, mirrorRoutes, publishCheckoutToSource);
                *checkoutSummary = outcome.summary();
                *reconciliationFetched = outcome.fetchOk;
                *reconciliationComplete =
                    outcome.fetchOk && outcome.error.isEmpty();
            } else {
                *reconciliationComplete = runGitCapture(
                    mirrorPath,
                    {QStringLiteral("fetch"), QStringLiteral("--quiet"),
                     meshUrl, QStringLiteral("refs/heads/*:refs/heads/*"),
                     QStringLiteral("refs/tags/*:refs/tags/*")},
                    nullptr, nullptr);
                *reconciliationFetched = *reconciliationComplete;
            }
            *mirrorMoved = mirrorRefsDigest(mirrorPath) != before;
        });
    connect(worker, &QThread::finished, this,
            [this, worker, index, key, mirrorMoved, checkoutSummary,
             reconciliationFetched, reconciliationComplete] {
                worker->deleteLater();
                m_syncingRepos.remove(index);
                refreshRepositoryList();
                if (!checkoutSummary->isEmpty())
                    logSystem(QStringLiteral("Mesh converge %1: %2")
                                  .arg(key, *checkoutSummary));
                if (*mirrorMoved || !checkoutSummary->isEmpty()) {
                    m_mirrorAdvertSig.clear();
                    refreshMirrorAdverts();
                }
                if (index == m_repoDetailIndex) {
                    // An explicit Issues refresh completes here even when all
                    // fetched refs were already current. Never leave the list
                    // showing the snapshot loaded before this worker started.
                    reloadIssues();
                    if (*mirrorMoved || !checkoutSummary->isEmpty()) {
                        reloadPulls();
                        reloadDiscussions();
                    }
                }
                if (*reconciliationFetched && index >= 0 &&
                    index < m_repositories.size()) {
                    // Publish after every reverse-reconciliation attempt that
                    // reached at least one peer. A partial pass may safely send
                    // non-forcing updates, but only a complete read may prune:
                    // an unreachable mirror can still hold the last branch.
                    const RepositoryRecord &live = m_repositories.at(index);
                    const QString liveKey =
                        live.owner.trimmed().toLower() + QLatin1Char('/') +
                        live.name.trimmed().toLower();
                    if (liveKey == key)
                        propagateRepoUpdate(index, /*includeManagedHosts=*/true,
                                            /*pruneRefs=*/
                                                *reconciliationComplete);
                }
            });
    worker->start();
}

void MainWindow::propagateRepoUpdate(int index)
{
    propagateRepoUpdate(index, /*includeManagedHosts=*/true);
}

void MainWindow::propagateRepoUpdate(int index, bool includeManagedHosts)
{
    propagateRepoUpdate(index, includeManagedHosts, /*pruneRefs=*/false);
}

void MainWindow::propagateRepoUpdate(int index, bool includeManagedHosts,
                                     bool pruneRefs)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    // By value: pushToSshMirrorRemotes and runGitCapture below pump the event
    // loop, and m_repositories can be reallocated while they do — a reference
    // here would dangle (the git-pump UAF family).
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.previewOnly || repo.localPath.trimmed().isEmpty())
        return;
    if (m_syncingRepos.contains(index)) {
        // A mirror sync already running is no reason to withhold the new refs
        // from the fleet. The SSH fan-out reads the working copy, serializes on
        // its own per gateway, and is the path that makes a mirror current
        // within seconds; only the duplicate syncRepository call below has to be
        // dropped, and the sync already in flight covers that. Returning early
        // from the whole function meant a commit landing during a sync reached
        // the mirrors on the next periodic pass at the earliest.
        if (includeManagedHosts)
            pushToSshMirrorRemotes(index, /*wakeManagedIfUnchanged=*/true,
                                   pruneRefs);
        else
            (void)pushToSshMirrorRemotes(
                index, /*userInitiated=*/false, QString(), nullptr, pruneRefs);
        return;
    }
    if (index == m_repoDetailIndex)
        refreshRepoSyncIndicators();
    if (includeManagedHosts)
        pushToSshMirrorRemotes(index, /*wakeManagedIfUnchanged=*/true,
                               pruneRefs);
    else
        (void)pushToSshMirrorRemotes(
            index, /*userInitiated=*/false, QString(), nullptr, pruneRefs);

    syncRepository(index, /*quiet=*/true);
    if (index == mirrorNodesRepoIndex() && mirrorNodesOnScreen())
        loadMirrorNodesPanel();
}

void MainWindow::propagateBranchDeletion(const QString &owner,
                                         const QString &name)
{
    // A ref deletion changes no commit, so the checkout's push hook cannot
    // discover it.  Resolve the live record by identity because asynchronous
    // worktree cleanup may finish after the user switches repositories.
    const int index = repoIndexFor(owner, name);
    if (index < 0)
        return;
    logSystem(QStringLiteral("Mirror: propagating deleted branch for %1/%2.")
                  .arg(owner, name));
    if (isSourceOfTruthRepo(m_repositories.at(index)))
        convergeSourceRepoFromMesh(index, /*force=*/true);
    else
        propagateRepoUpdate(index);
}

void MainWindow::onPeerMirrorUpdated(const QString &ownerName,
                                     const QString &peerName,
                                     const QString &commit)
{
    int matchIndex = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (repo.previewOnly)
            continue;
        const QString canonical =
            catalogOwner(repo) + "/" +
            repoSegment(repo.name, QStringLiteral("repository"));
        const QString source =
            repoSegment(repo.owner, QStringLiteral("owner")) + "/" +
            repoSegment(repo.name, QStringLiteral("repository"));
        if (canonical == ownerName || source == ownerName ||
            (repo.owner + "/" + repo.name) == ownerName) {
            matchIndex = i;
            break;
        }
    }
    if (matchIndex < 0)
        return;

    const QString who =
        peerName.trimmed().isEmpty() ? QStringLiteral("A peer") : peerName.trimmed();
    const QString msg = who + " updated the mirror of " + ownerName +
                        " from its source.";
    logSystem(msg);
    flashPingMessage(msg);

    applyPeerMirrorCommit(ownerName, peerName, commit);

    const RepositoryRecord &matched = m_repositories.at(matchIndex);

    if (!m_syncingRepos.contains(matchIndex)) {
        if (isSourceOfTruthRepo(matched))
            convergeSourceRepoFromMesh(matchIndex);
        else
            syncRepository(matchIndex, /*quiet=*/true);
    }
    if (notifyEnabled(kMirrorUpdateAlertSetting))
        showPingTrayMessage(QStringLiteral("ForkMesh — mirror updated"), msg);
}

void MainWindow::onPeerMirrorSynced(const QString &ownerName,
                                    const QString &peerName,
                                    const QString &commit)
{
    bool relevant = false;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        const QString canonical =
            catalogOwner(repo) + "/" +
            repoSegment(repo.name, QStringLiteral("repository"));
        const QString source =
            repoSegment(repo.owner, QStringLiteral("owner")) + "/" +
            repoSegment(repo.name, QStringLiteral("repository"));
        if (canonical == ownerName || source == ownerName ||
            (repo.owner + "/" + repo.name) == ownerName) {
            relevant = true;
            break;
        }
    }
    if (!relevant)
        return;

    const QString who =
        peerName.trimmed().isEmpty() ? QStringLiteral("A peer") : peerName.trimmed();
    QString msg = who + " is up to date on the mirror of " + ownerName;
    if (!commit.trimmed().isEmpty())
        msg += " at " + commit.trimmed().left(10);
    msg += ".";
    logSystem(msg);

    applyPeerMirrorCommit(ownerName, peerName, commit);
}

bool MainWindow::applyPeerMirrorCommit(const QString &ownerName,
                                       const QString &peerName,
                                       const QString &commit)
{
    const QString group = ownerName.trimmed();
    const QString target = commit.trimmed().left(64);
    const QString who = peerName.trimmed();
    if (group.isEmpty() || target.isEmpty() || who.isEmpty())
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (MemberInfo &node : m_homeRoster) {
        if (node.self)
            continue; // our own row reads the live local HEAD, never a peer ack
        if (node.name.compare(who, Qt::CaseInsensitive) != 0 &&
            node.nodeName.compare(who, Qt::CaseInsensitive) != 0)
            continue;
        for (MirrorAdvert &m : node.mirrorDetails) {
            if (m.source.compare(group, Qt::CaseInsensitive) != 0 &&
                m.ownerName.compare(group, Qt::CaseInsensitive) != 0)
                continue;
            if (m.commit != target) {
                m.commit = target;
                if (m.updatedMs < now)
                    m.updatedMs = now;
                changed = true;
            }
        }
    }
    if (changed && mirrorNodesOnScreen())
        loadMirrorNodesPanel();
    return changed;
}

void MainWindow::scanRepoMentionsFor(const RepositoryRecord &repo)
{
    if (repo.owner.isEmpty() || repo.name.isEmpty())
        return;
    // Two readers share this one load of the threads: @you notifications, which
    // need a valid account name to match against, and @model mentions, which
    // answer on the host whatever this node calls itself. Skip the load only
    // when neither wants it.
    if (!isValidNodeName(accountNameFromInput(m_userName, QString())) &&
        !QSettings().value(kIssueModelMentionsSetting, true).toBool())
        return;

    const QString repoKey = repo.owner + "/" + repo.name;

    // Loading every issue and pull request thread off disk (each a
    // parse of many small files) is the heavy part: on a large repo — the flagship
    // forkmesh project runs to hundreds of issues/PRs — it froze the UI every time
    // a sync or inbox drain finished, which is what fired this scan. Do that I/O on
    // a worker thread, then match @mentions and raise notifications back on the main
    // thread. The stores are copied by value and only read on the worker (no event
    // signing), the same off-thread pattern deleteIssue uses for its git work.
    if (m_mentionScanInFlight.contains(repoKey))
        return; // a scan for this repo is already loading; don't double-notify
    m_mentionScanInFlight.insert(repoKey);

    auto loadedIssues = std::make_shared<QList<Issue>>();
    auto loadedPulls = std::make_shared<QList<PullRequest>>();
    IssueStore issueStore(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                          m_userName);
    PullStore pullStore(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                        m_userName);
    QThread *worker = QThread::create(
        [issueStore, pullStore, loadedIssues, loadedPulls]() mutable {
            *loadedIssues = issueStore.loadAll();
            *loadedPulls = pullStore.loadAll();
        });
    connect(worker, &QThread::finished, this,
            [this, worker, repo, repoKey, loadedIssues, loadedPulls]() {
                worker->deleteLater();
                m_mentionScanInFlight.remove(repoKey);
                applyRepoMentions(repo, *loadedIssues, *loadedPulls);
            });
    worker->start();
}

void MainWindow::applyRepoMentions(const RepositoryRecord &repo,
                                   const QList<Issue> &allIssues,
                                   const QList<PullRequest> &allPulls)
{
    const QString repoKey = repo.owner + "/" + repo.name;
    QSettings settings;
    const QStringList seenList =
        settings.value(QStringLiteral("mentions/seen")).toStringList();
    QSet<QString> seen(seenList.cbegin(), seenList.cend());
    QStringList seededRepos =
        settings.value(QStringLiteral("mentions/seededRepos")).toStringList();
    const bool seeding = !seededRepos.contains(repoKey);
    const QString myKey = m_profileIdentity.publicKey();
    const int mentionRepoIndex = repoIndexFor(repo.owner, repo.name);

    std::optional<forkmesh::teammates::LoadResult> teammates;
    auto mentionsAnyTeammate = [&](const QString &text) {
        if (mentionRepoIndex < 0 || !text.contains(QLatin1Char('@')))
            return false;
        if (!teammates)
            teammates = forkmesh::teammates::Store(
                            teammatesDirPath(mentionRepoIndex))
                            .load();
        return std::any_of(
            teammates->teammates.cbegin(), teammates->teammates.cend(),
            [&text](const forkmesh::teammates::Config &teammate) {
                return teammate.enabled &&
                       forkmesh::teammates::textMentionsTeammate(text,
                                                                 teammate);
            });
    };

    bool dirty = false;
    auto notifyMention = [&](const QString &stableKey, const QString &authorKey,
                             const QString &authorName, const QString &text,
                             const QString &humanLocator,
                             const NotificationLink &link) {
        if (seen.contains(stableKey))
            return;
        const bool mentionsUser =
            !text.isEmpty() && textMentionsNodeName(text, m_userName);
        const bool mentionsTeammate = mentionsAnyTeammate(text);
        if (!mentionsUser && !mentionsTeammate)
            return;
        const bool ownWriting = !authorKey.isEmpty() && authorKey == myKey;
        seen.insert(stableKey);
        dirty = true;
        if (seeding)
            return; // recorded, but no alert for pre-existing history
        const QString who = authorName.trimmed().isEmpty()
                                ? QStringLiteral("Someone")
                                : authorName.trimmed();
        if (mentionsTeammate) {
            triggerTeammatesForEvent(
                QStringLiteral("mentions"),
                QStringLiteral("%1 in %2 %3\n%4")
                    .arg(who, repoKey, humanLocator, text),
                mentionRepoIndex);
        }
        // A person's own @username is not a notification, but their @Teammate
        // remains a real assignment and has already been delivered above.
        if (!mentionsUser || ownWriting)
            return;
        QString snippet = text.simplified();
        if (snippet.size() > 160)
            snippet = snippet.left(157) + QString::fromUtf8("\xE2\x80\xA6");
        const QString body =
            QString::fromUtf8("%1 mentioned you in %2 %3: \xE2\x80\x9C%4\xE2\x80\x9D")
                .arg(who, repoKey, humanLocator, snippet);
        addNotification(QStringLiteral("Mention"), body, false, link);
        if (notifyEnabled(kMentionAlertSetting)) {
            if (!pingsPaused() && nativeSystemNotificationsEnabled())
                QApplication::alert(this, 0);
            postNotification(who + QStringLiteral(" mentioned you"), body);
        }
    };
    auto consider = [&](const QString &kind, int number, const QString &eventId,
                        const QString &authorKey, const QString &authorName,
                        const QString &text, const QString &context) {
        const QString key = QStringLiteral("%1#%2%3:%4")
                                .arg(repoKey, kind)
                                .arg(number)
                                .arg(eventId);
        NotificationLink link;
        link.kind = kind; // "issue" | "pull" — matches openNotificationLink
        link.owner = repo.owner;
        link.name = repo.name;
        link.number = number;
        notifyMention(key, authorKey, authorName, text,
                      context + QStringLiteral("#") + QString::number(number),
                      link);
    };

    for (const Issue &issue : allIssues) {
        for (const IssueEvent &ev : issue.events) {
            if (ev.type != QLatin1String("open") &&
                ev.type != QLatin1String("comment") &&
                ev.type != QLatin1String("edit"))
                continue;
            const QString text = (ev.title + QStringLiteral("\n") + ev.body).trimmed();
            consider(QStringLiteral("issue"), issue.number, ev.id, ev.author,
                     ev.authorName, text, QStringLiteral("issue "));
        }
    }

    for (const PullRequest &pr : allPulls) {
        const QString openText =
            (pr.title + QStringLiteral("\n") + pr.description).trimmed();
        consider(QStringLiteral("pull"), pr.number, QStringLiteral("open"), pr.author,
                 pr.authorName, openText, QStringLiteral("PR "));
        for (const PullEvent &ev : pr.events)
            consider(QStringLiteral("pull"), pr.number, ev.id, ev.author,
                     ev.authorName, ev.body, QStringLiteral("PR "));
    }

    if (dirty) {
        const QStringList keys(seen.cbegin(), seen.cend());
        settings.setValue(QStringLiteral("mentions/seen"), keys);
    }
    if (seeding) {
        seededRepos.append(repoKey);
        settings.setValue(QStringLiteral("mentions/seededRepos"), seededRepos);
    }

    answerIssueModelMentions(repo, allIssues, allPulls);
}

bool MainWindow::setRepositoryNetworkServing(int index, bool enabled)
{
    if (index < 0 || index >= m_repositories.size())
        return false;
    RepositoryRecord &repo = m_repositories[index];
    if (repo.publishToNetwork == enabled)
        return false;
    if (!hasOwnerSigningCapability(catalogOwner(repo))) {
        flashMessage(QStringLiteral(
                         "Only the repository owner's signing identity can "
                         "change network serving."),
                     true);
        return false;
    }
    // Read everything needed before the sync below: it pumps the event loop and
    // m_repositories can be reallocated while it runs.
    const QString owner = repo.owner;
    const QString name = repo.name;
    const bool isPrivate = repo.isPrivate;
    repo.publishToNetwork = enabled;
    saveRepositories();
    if (enabled) {
        if (isPrivate)
            syncPrivateRepository(index, /*quiet=*/false);
        else
            syncRepository(index, /*quiet=*/false);
    } else if (directMirrorGatewayConfigured()) {
        // Drop the mirror from the gateway so turning serving off actually
        // stops the relay from routing to it.
        QString gatewayError;
        if (!rebuildDirectMirrorGatewayConfiguration(&gatewayError, true))
            logSystem(QStringLiteral(
                          "Could not refresh the direct gateway after "
                          "disabling a mirror: %1")
                          .arg(gatewayError));
    }
    startNodeEventSocket();
    logSystem(QStringLiteral("%1 network serving for %2/%3.")
                  .arg(enabled ? QStringLiteral("Enabled")
                               : QStringLiteral("Disabled"),
                       owner, name));
    refreshRepositoryList();
    return true;
}

bool MainWindow::privateSyncIdle(const RepositoryRecord &repo,
                                 const QString &refsFingerprint) const
{
    if (repo.privateSyncRefsHash.isEmpty())
        return false;
    return PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
        repo.privateSyncRefsHash, refsFingerprint, repo.lastSyncMs,
        QDateTime::currentMSecsSinceEpoch(), kPrivateIdleResyncMs);
}

void MainWindow::syncPrivateRepository(int index, bool quiet, bool idleChecked)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_syncingRepos.contains(index))
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    if (!PrivateMirrorRuntime::privateRepositoryCloudAccessAllowed(
            repo.isPrivate, repo.publishToNetwork)) {
        // Sealing, downloading, and recipient-key refresh can all lead to a
        // network request. A local-only private repository does none of them;
        // enabling Serve is the explicit transition into the cloud-backed
        // private-mirror flow.
        if (!quiet)
            flashMessage(QStringLiteral(
                "This private repository is local only. Enable network "
                "serving before syncing its encrypted replica; no network "
                "request was made."));
        return;
    }
    // Everything below reaches the relay with the owner and repository name in
    // the URL, so the periodic safety pass must have a reason to go. When the
    // sealed source's refs have not moved since the last successful seal and
    // that seal is still recent, a private repository nobody touched stays
    // entirely off the network. An explicit sync, a local commit, or a long
    // idle stretch (which re-checks the recipient set) all still go through.
    if (quiet && !idleChecked && !repo.privateSyncRefsHash.isEmpty()) {
        const QString localSource = privateSyncLocalSource(repo);
        if (!localSource.isEmpty()) {
            if (m_privateSyncIdleChecks.contains(localSource))
                return;
            m_privateSyncIdleChecks.insert(localSource);
            runOffThread<QString>(
                [localSource] { return privateSyncRefsFingerprint(localSource); },
                [this, index, quiet, localSource, owner = repo.owner,
                 name = repo.name](QString fingerprint) {
                    m_privateSyncIdleChecks.remove(localSource);
                    if (index < 0 || index >= m_repositories.size())
                        return;
                    const RepositoryRecord &current = m_repositories.at(index);
                    if (current.owner != owner || current.name != name)
                        return;
                    if (privateSyncLocalSource(current) == localSource &&
                        privateSyncIdle(current, fingerprint))
                        return;
                    syncPrivateRepository(index, quiet, /*idleChecked=*/true);
                });
            return;
        }
    }
    const bool ownerCapable =
        hasOwnerSigningCapability(repo.owner);
    const QUrl sourceUrl(repo.cloneUrl.trimmed());
    const bool sourceIsForkMeshRoute =
        repo.localPath.trimmed().isEmpty() &&
        sourceUrl.isValid() && !sourceUrl.host().isEmpty() &&
        sourceUrl.host().compare(catalogApiUrl().host(),
                                Qt::CaseInsensitive) == 0;
    const bool hasPublicTransitionSource =
        PublicMirrorRuntime::isArchiveId(repo.publicArchiveId) &&
        QDir(repo.mirrorPath).exists();
    if (!ownerCapable ||
        (sourceIsForkMeshRoute && !hasPublicTransitionSource)) {
        downloadPrivateReplica(index, quiet);
        return;
    }

    if (PrivateMirrorStore::isOpaqueId(repo.privateReplicaId) &&
        repo.publishToNetwork) {
        m_syncingRepos.insert(index, quiet);
        setRepoSyncActivity(
            index, QStringLiteral("Verifying the collaborators' keys…"));
        refreshRepositoryList();
        requestPrivateRecipientBundles(
            repo, /*updateCollaboratorList=*/false,
            [this, index, quiet](bool ready,
                                QList<QJsonObject> recipientBundles,
                                QStringList, QString error) {
                m_syncingRepos.remove(index);
                if (!ready) {
                    clearRepoSyncActivity(index);
                    logSystem(QStringLiteral(
                        "Private mirror: recipient-key verification blocked "
                        "the refresh; the previous encrypted epoch remains "
                        "unchanged."));
                    if (!quiet)
                        flashMessage(
                            QStringLiteral(
                                "Private mirror refresh blocked: %1")
                                .arg(error),
                            true);
                    refreshRepositoryList();
                    return;
                }
                syncPrivateRepositoryWithRecipients(
                    index, quiet, recipientBundles);
            });
        return;
    }
    syncPrivateRepositoryWithRecipients(index, quiet, {});
}

void MainWindow::syncPrivateRepositoryWithRecipients(
    int index, bool quiet,
    const QList<QJsonObject> &recipientBundles)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_syncingRepos.contains(index))
        return;
    if (!m_profileIdentity.isValid() && !m_profileIdentity.load()) {
        clearRepoSyncActivity(index);
        if (!quiet)
            flashMessage(
                QStringLiteral("The local owner identity is required before a "
                               "private mirror can be sealed."),
                true);
        return;
    }
    const QByteArray vaultSecret =
        privateIdentityVaultSecret(m_profileIdentity);
    if (vaultSecret.size() < 32) {
        clearRepoSyncActivity(index);
        if (!quiet)
            flashMessage(QStringLiteral(
                             "Could not unlock the owner-only private-mirror vault."),
                         true);
        return;
    }

    RepositoryRecord &repo = m_repositories[index];
    const QString existingOpaqueId = repo.privateReplicaId;
    QString runtimePath;
    const auto currentMaterialization =
        m_privateMirrorMaterializations.value(existingOpaqueId);
    if (currentMaterialization && currentMaterialization->isValid())
        runtimePath = currentMaterialization->repositoryPath();

    // A pre-encryption install may still have a persistent bare mirror under
    // the managed mirror root. It is a one-time migration source and is removed
    // only after both the encrypted replica and a fresh authenticated temporary
    // materialization have succeeded.
    QString legacyMirrorPath;
    if (!repo.mirrorPath.trimmed().isEmpty() &&
        repo.mirrorPath != runtimePath && QDir(repo.mirrorPath).exists()) {
        legacyMirrorPath = repo.mirrorPath;
    }

    QString source;
    if (!repo.localPath.trimmed().isEmpty() &&
        (QDir(repo.localPath).exists(QStringLiteral(".git")) ||
         QFileInfo(QDir(repo.localPath).filePath(QStringLiteral("HEAD")))
             .isFile())) {
        source = repo.localPath;
    } else if (!legacyMirrorPath.isEmpty()) {
        source = legacyMirrorPath;
    } else {
        source = repo.cloneUrl.trimmed();
    }
    QStringList authArgs = viewAuthGitArgs(repo, source);
    if (authArgs.isEmpty() &&
        QUrl(source).scheme().compare(QStringLiteral("https"),
                                     Qt::CaseInsensitive) == 0) {
        authArgs = importAuthGitArgs(source);
    }
    const QString replicaRoot = privateReplicaRoot();
    const QString vaultPath = privateIdentityVaultPath();
    const QString managedMirrorRoot = repositoryMirrorRoot();

    auto result = std::make_shared<PrivateSyncWorkerResult>();
    m_syncingRepos.insert(index, quiet);
    setRepoSyncActivity(
        index, QStringLiteral("Sealing the owner-only private replica…"));
    // Keep the outbound relay event bridge current while the managed mirror
    // service owns publication and serving.
    startNodeEventSocket();
    refreshRepositoryList();
    if (!quiet)
        logSystem(QStringLiteral(
            "Private mirror: sealing an owner-only opaque replica."));

    QThread *worker = QThread::create(
        [result, source, authArgs, recipientBundles, replicaRoot, vaultPath,
         mutableVaultSecret = vaultSecret, existingOpaqueId,
         legacyMirrorPath, managedMirrorRoot,
         localPath = repo.localPath]() mutable {
            if (!source.isEmpty()) {
                result->sync = PrivateMirrorRuntime::syncSource(
                    source, authArgs, replicaRoot, vaultPath,
                    mutableVaultSecret, existingOpaqueId,
                    recipientBundles, &result->error);
            }
            // Offline fallback: preserve the last authenticated Git archive but
            // still rotate it to the relay-verified exact recipient set. This
            // ensures a revocation is not postponed by an unavailable upstream.
            if (!result->sync.isValid() &&
                PrivateMirrorStore::isOpaqueId(existingOpaqueId)) {
                PrivateMirrorStore::Metadata metadata;
                QString resealError;
                if (PrivateMirrorRuntime::resealRecipients(
                        replicaRoot, vaultPath, mutableVaultSecret,
                        existingOpaqueId, recipientBundles, &metadata,
                        &resealError)) {
                    result->sync.opaqueId = existingOpaqueId;
                    result->sync.metadata = metadata;
                    result->error.clear();
                    result->notice = QStringLiteral(
                        "Source refresh was deferred; the last authenticated "
                        "archive was re-keyed to the verified recipient set.");
                }
            }
            if (result->sync.isValid()) {
                auto opened = PrivateMirrorRuntime::materialize(
                    replicaRoot, vaultPath, mutableVaultSecret,
                    result->sync.opaqueId, &result->error);
                if (opened)
                    result->materialization =
                        std::shared_ptr<PrivateMirrorMaterialization>(
                            std::move(opened));
            }
            if (result->sync.isValid() && result->materialization &&
                !legacyMirrorPath.isEmpty()) {
                const QString root =
                    QFileInfo(managedMirrorRoot).canonicalFilePath();
                const QString target =
                    QFileInfo(legacyMirrorPath).canonicalFilePath();
                if (!root.isEmpty() && !target.isEmpty() &&
                    target.startsWith(
                        QDir::cleanPath(root) + QLatin1Char('/'))) {
                    result->legacyRemoved =
                        PrivateMirrorRuntime::removeManagedPlaintextMirror(
                            legacyMirrorPath, managedMirrorRoot,
                            &result->error);
                }
            }
            mutableVaultSecret.fill('\0');
            mutableVaultSecret.clear();
            if (result->sync.isValid() && result->materialization &&
                result->materialization->isValid() && result->legacyRemoved) {
                RepositoryRecord sealed;
                sealed.localPath = localPath;
                sealed.mirrorPath =
                    result->materialization->repositoryPath();
                result->refsSource = privateSyncLocalSource(sealed);
                result->refsFingerprint =
                    privateSyncRefsFingerprint(result->refsSource);
            }
        });
    connect(worker, &QThread::finished, this,
            [this, worker, result, index, quiet, legacyMirrorPath] {
                worker->deleteLater();
                m_syncingRepos.remove(index);
                clearRepoSyncActivity(index);
                if (index < 0 || index >= m_repositories.size()) {
                    refreshRepositoryList();
                    return;
                }
                if (!result->sync.isValid() || !result->materialization ||
                    !result->materialization->isValid() ||
                    !result->legacyRemoved) {
                    logSystem(QStringLiteral(
                                  "Private mirror: encryption/runtime setup failed: %1")
                                  .arg(result->error.isEmpty()
                                           ? QStringLiteral("unknown local error")
                                           : result->error.left(240)));
                    if (!quiet) {
                        flashMessage(
                            QStringLiteral(
                                "Private mirror was not activated: %1")
                                .arg(result->error.isEmpty()
                                         ? QStringLiteral(
                                               "could not authenticate the "
                                               "encrypted replica")
                                         : result->error.left(160)),
                            true);
                    }
                    refreshRepositoryList();
                    return;
                }

                RepositoryRecord &current = m_repositories[index];
                const QString previousPublicArchive =
                    current.publicArchiveId;
                current.privateReplicaId = result->sync.opaqueId;
                current.publicArchiveId.clear();
                adoptMaterializedMirror(
                    current, result->materialization->repositoryPath());
                current.lastSyncMs =
                    QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
                // Recorded against the post-sync record so the next background
                // pass measures exactly what was sealed here and can stay off
                // the network while it is unchanged.
                current.privateSyncRefsHash =
                    privateSyncLocalSource(current) == result->refsSource
                        ? result->refsFingerprint
                        : QString();
                m_privateMirrorMaterializations.insert(
                    result->sync.opaqueId, result->materialization);
                if (PublicMirrorRuntime::isArchiveId(
                        previousPublicArchive)) {
                    m_publicMirrorMaterializations.remove(
                        previousPublicArchive);
                }
                saveRepositories();
                refreshRepositoryList();
                ensurePushHook(current);
                if (index == m_repoDetailIndex)
                    refreshOpenRepoDetail();
                startNodeEventSocket();
                logSystem(QStringLiteral(
                              "Private mirror: authenticated encrypted epoch %1 "
                              "is ready in owner-only temporary storage.")
                              .arg(result->sync.metadata.keyEpoch));
                if (!legacyMirrorPath.isEmpty()) {
                    logSystem(QStringLiteral(
                        "Private mirror: removed the migrated managed plaintext "
                        "bare replica."));
                }
                if (!result->notice.isEmpty())
                    logSystem(QStringLiteral("Private mirror: ") + result->notice);
                if (!quiet) {
                    flashMessage(
                        QStringLiteral(
                            "Private mirror sealed (encrypted epoch %1).")
                            .arg(result->sync.metadata.keyEpoch));
                }
                if (current.publishToNetwork) {
                    QString gatewayError;
                    if (rebuildDirectMirrorGatewayConfiguration(
                            &gatewayError, true)) {
                        registerPrivateReplicaRoute(index);
                    } else {
                        logSystem(
                            QStringLiteral(
                                "Private publication remains blocked until "
                                "the direct HTTPS gateway is configured: %1")
                                .arg(gatewayError));
                    }
                }
            });
    worker->start();
}

void MainWindow::resealPrivateRepositoryRecipients(
    const RepositoryRecord &repo,
    const QList<QJsonObject> &recipientBundles,
    std::function<void(bool, QString)> onDone)
{
    auto finish =
        [onDone = std::move(onDone)](
            bool ok, const QString &error = QString()) mutable {
            if (onDone)
                onDone(ok, error);
        };
    int index = -1;
    for (int candidate = 0; candidate < m_repositories.size();
         ++candidate) {
        const RepositoryRecord &current =
            m_repositories.at(candidate);
        if (current.isPrivate && current.owner == repo.owner &&
            current.name == repo.name &&
            current.privateReplicaId == repo.privateReplicaId) {
            index = candidate;
            break;
        }
    }
    if (index < 0 ||
        !PrivateMirrorStore::isOpaqueId(repo.privateReplicaId) ||
        m_syncingRepos.contains(index) ||
        !hasOwnerSigningCapability(repo.owner)) {
        finish(false,
               QStringLiteral(
                   "the local owner-sealed replica is unavailable"));
        return;
    }
    if (!m_profileIdentity.isValid() &&
        !m_profileIdentity.load()) {
        finish(false,
               QStringLiteral(
                   "the local owner identity is unavailable"));
        return;
    }
    QByteArray vaultSecret =
        privateIdentityVaultSecret(m_profileIdentity);
    if (vaultSecret.size() < 32) {
        finish(false,
               QStringLiteral(
                   "the local encryption identity vault is unavailable"));
        return;
    }

    const QString replicaRoot = privateReplicaRoot();
    const QString vaultPath = privateIdentityVaultPath();
    const QString opaqueId = repo.privateReplicaId;
    auto result = std::make_shared<PrivateDownloadWorkerResult>();
    m_syncingRepos.insert(index, false);
    refreshRepositoryList();
    QThread *worker = QThread::create(
        [result, replicaRoot, vaultPath, opaqueId, recipientBundles,
         mutableVaultSecret = std::move(vaultSecret)]() mutable {
            if (PrivateMirrorRuntime::resealRecipients(
                    replicaRoot, vaultPath, mutableVaultSecret,
                    opaqueId, recipientBundles, &result->metadata,
                    &result->error)) {
                result->opaqueId = opaqueId;
                auto opened = PrivateMirrorRuntime::materialize(
                    replicaRoot, vaultPath, mutableVaultSecret,
                    opaqueId, &result->error);
                if (opened) {
                    result->materialization =
                        std::shared_ptr<PrivateMirrorMaterialization>(
                            std::move(opened));
                }
            }
            mutableVaultSecret.fill('\0');
            mutableVaultSecret.clear();
        });
    connect(
        worker, &QThread::finished, this,
        [this, worker, result, index, opaqueId,
         finish = std::move(finish)]() mutable {
            worker->deleteLater();
            m_syncingRepos.remove(index);
            if (index < 0 || index >= m_repositories.size() ||
                m_repositories.at(index).privateReplicaId !=
                    opaqueId ||
                result->opaqueId != opaqueId ||
                !result->materialization ||
                !result->materialization->isValid()) {
                refreshRepositoryList();
                finish(
                    false,
                    QStringLiteral(
                        "the encrypted replica could not be authenticated "
                        "after rotation"));
                return;
            }
            RepositoryRecord &current = m_repositories[index];
            adoptMaterializedMirror(
                current, result->materialization->repositoryPath());
            current.lastSyncMs =
                QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
            m_privateMirrorMaterializations.insert(
                opaqueId, result->materialization);
            saveRepositories();
            refreshRepositoryList();
            if (index == m_repoDetailIndex)
                refreshOpenRepoDetail();
            startNodeEventSocket();
            if (current.publishToNetwork) {
                QString gatewayError;
                if (rebuildDirectMirrorGatewayConfiguration(
                        &gatewayError, true)) {
                    registerPrivateReplicaRoute(index);
                } else {
                    logSystem(
                        QStringLiteral(
                            "Private publication remains blocked until the "
                            "direct HTTPS gateway is configured: %1")
                            .arg(gatewayError));
                }
            }
            finish(true, QString());
        });
    worker->start();
}

void MainWindow::downloadPrivateReplica(int index, bool quiet)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_syncingRepos.contains(index))
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    auto unavailable = [this, index, quiet](const QString &logDetail) {
        m_syncingRepos.remove(index);
        if (!logDetail.isEmpty())
            logSystem(QStringLiteral(
                "Private mirror: authorized encrypted replica download "
                "was unavailable."));
        if (!quiet)
            flashMessage(
                QStringLiteral(
                    "Private repository is unavailable or this account is "
                    "not authorized."),
                true);
        refreshRepositoryList();
    };
    if (!repo.isPrivate || !m_networkAccess ||
        !m_profileIdentity.isValid() ||
        !hasOwnerSigningCapability(accountOwner())) {
        unavailable(QStringLiteral("local authorization unavailable"));
        return;
    }

    m_syncingRepos.insert(index, quiet);
    refreshRepositoryList();
    ensurePrivateMirrorRecipientIdentityRegistered(
        [this, index, repo, quiet, unavailable](
            bool registered, QString) {
            if (!registered || index < 0 ||
                index >= m_repositories.size() ||
                m_repositories.at(index).owner != repo.owner ||
                m_repositories.at(index).name != repo.name) {
                unavailable(
                    QStringLiteral("recipient identity unavailable"));
                return;
            }
            const QByteArray authorization =
                privateReplicaAuthorization(repo);
            QUrl url = catalogApiUrl();
            // The edge-visible route contains only a random opaque archive id.
            // Owner/repository names remain inside the existing signed
            // authorization proof and are resolved after ACL lookup; there is
            // deliberately no named-route or query-string fallback.
            url.setPath(
                QStringLiteral("/api/private-replicas/") +
                repo.privateReplicaId);
            url.setQuery(QString());
            url.setFragment(QString());
            if (authorization.isEmpty() ||
                url.scheme().compare(QStringLiteral("https"),
                                     Qt::CaseInsensitive) != 0) {
                unavailable(
                    QStringLiteral("secure route unavailable"));
                return;
            }

            QNetworkRequest request(url);
            request.setRawHeader("Authorization", authorization);
            request.setAttribute(
                QNetworkRequest::RedirectPolicyAttribute,
                QNetworkRequest::ManualRedirectPolicy);
            request.setAttribute(
                QNetworkRequest::CacheLoadControlAttribute,
                QNetworkRequest::AlwaysNetwork);
            request.setAttribute(
                QNetworkRequest::CacheSaveControlAttribute, false);
            QNetworkReply *reply = m_networkAccess->get(request);
            auto bytes = std::make_shared<QByteArray>();
            auto overflowed = std::make_shared<bool>(false);
            auto appendChunk =
                [reply, bytes, overflowed]() {
                    QByteArray chunk = reply->readAll();
                    const qint64 maximum =
                        PrivateMirrorRuntime::
                            maximumSerializedReplicaBytes();
                    if (*overflowed ||
                        qint64(bytes->size()) + chunk.size() >
                            maximum) {
                        chunk.fill('\0');
                        chunk.clear();
                        bytes->fill('\0');
                        bytes->clear();
                        *overflowed = true;
                        reply->abort();
                        return;
                    }
                    bytes->append(chunk);
                };
            connect(reply, &QIODevice::readyRead, this,
                    appendChunk);
            connect(
                reply, &QNetworkReply::finished, this,
                [this, reply, index, repo, quiet, unavailable,
                 bytes, overflowed, appendChunk]() mutable {
                    appendChunk();
                    const int status =
                        reply->attribute(
                                 QNetworkRequest::
                                     HttpStatusCodeAttribute)
                            .toInt();
                    const QString contentType =
                        reply->header(
                                 QNetworkRequest::ContentTypeHeader)
                            .toString()
                            .section(QLatin1Char(';'), 0, 0)
                            .trimmed()
                            .toLower();
                    const QByteArray etag =
                        reply->rawHeader("ETag").trimmed();
                    bool lengthOk = false;
                    const qint64 announcedLength =
                        reply->rawHeader("Content-Length")
                            .toLongLong(&lengthOk);
                    const bool networkOk =
                        reply->error() ==
                        QNetworkReply::NoError;
                    reply->deleteLater();

                    static const QRegularExpression etagPattern(
                        QStringLiteral(
                            "^\"sha256-([0-9a-f]{64})\"$"));
                    const QRegularExpressionMatch etagMatch =
                        etagPattern.match(
                            QString::fromLatin1(etag));
                    const bool responseValid =
                        networkOk && !*overflowed &&
                        status == 200 &&
                        contentType ==
                            QLatin1String(
                                "application/vnd.forkmesh."
                                "private-replica+json") &&
                        etagMatch.hasMatch() && lengthOk &&
                        announcedLength > 0 &&
                        announcedLength ==
                            qint64(bytes->size()) &&
                        announcedLength <=
                            PrivateMirrorRuntime::
                                maximumSerializedReplicaBytes();
                    if (!responseValid) {
                        bytes->fill('\0');
                        bytes->clear();
                        unavailable(
                            QStringLiteral(
                                "ciphertext response invalid"));
                        return;
                    }

                    const QString expectedDigest =
                        etagMatch.captured(1);
                    const QString replicaRoot =
                        privateReplicaRoot();
                    const QString vaultPath =
                        privateIdentityVaultPath();
                    QByteArray vaultSecret =
                        privateIdentityVaultSecret(
                            m_profileIdentity);
                    auto result =
                        std::make_shared<
                            PrivateDownloadWorkerResult>();
                    QThread *worker = QThread::create(
                        [result, replicaRoot, vaultPath,
                         serialized = std::move(*bytes),
                         expectedDigest,
                         mutableVaultSecret =
                             std::move(vaultSecret)]() mutable {
                            result->opaqueId =
                                PrivateMirrorStore::
                                    importReplica(
                                        replicaRoot, serialized,
                                        expectedDigest,
                                        &result->metadata,
                                        &result->error);
                            serialized.fill('\0');
                            serialized.clear();
                            if (!result->opaqueId.isEmpty()) {
                                auto opened =
                                    PrivateMirrorRuntime::
                                        materialize(
                                            replicaRoot,
                                            vaultPath,
                                            mutableVaultSecret,
                                            result->opaqueId,
                                            &result->error);
                                if (opened) {
                                    result->materialization =
                                        std::shared_ptr<
                                            PrivateMirrorMaterialization>(
                                            std::move(opened));
                                }
                            }
                            mutableVaultSecret.fill('\0');
                            mutableVaultSecret.clear();
                        });
                    connect(
                        worker, &QThread::finished, this,
                        [this, worker, result, index, repo,
                         quiet, unavailable] {
                            worker->deleteLater();
                            if (index < 0 ||
                                index >=
                                    m_repositories.size() ||
                                m_repositories.at(index).owner !=
                                    repo.owner ||
                                m_repositories.at(index).name !=
                                    repo.name ||
                                !PrivateMirrorStore::isOpaqueId(
                                    result->opaqueId) ||
                                !result->materialization ||
                                !result->materialization
                                     ->isValid()) {
                                unavailable(QStringLiteral(
                                    "ciphertext authentication "
                                    "failed"));
                                return;
                            }
                            RepositoryRecord &current =
                                m_repositories[index];
                            const QString previousId =
                                current.privateReplicaId;
                            current.privateReplicaId =
                                result->opaqueId;
                            adoptMaterializedMirror(
                                current,
                                result->materialization
                                    ->repositoryPath());
                            current.lastSyncMs =
                                QDateTime::
                                    currentDateTimeUtc()
                                        .toMSecsSinceEpoch();
                            if (!previousId.isEmpty() &&
                                previousId !=
                                    result->opaqueId) {
                                m_privateMirrorMaterializations
                                    .remove(previousId);
                            }
                            m_privateMirrorMaterializations
                                .insert(
                                    result->opaqueId,
                                    result->materialization);
                            m_syncingRepos.remove(index);
                            saveRepositories();
                            refreshRepositoryList();
                            if (index ==
                                m_repoDetailIndex) {
                                refreshOpenRepoDetail();
                            }
                            startNodeEventSocket();
                            logSystem(QStringLiteral(
                                          "Private mirror: "
                                          "authenticated "
                                          "encrypted epoch %1 "
                                          "opened in temporary "
                                          "recipient-only "
                                          "storage.")
                                          .arg(result->metadata
                                                   .keyEpoch));
                            if (!quiet)
                                flashMessage(
                                    QStringLiteral(
                                        "Private repository "
                                        "opened from encrypted "
                                        "epoch %1.")
                                        .arg(
                                            result->metadata
                                                .keyEpoch));
                        });
                    worker->start();
                });
        });
}

void MainWindow::syncPublicEncryptedRepository(int index, bool quiet)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_syncingRepos.contains(index))
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.previewOnly || repo.isPrivate)
        return;

    QByteArray vaultSecret =
        publicIdentityVaultSecret(m_profileIdentity);
    if (vaultSecret.size() < 32) {
        if (!quiet)
            flashMessage(
                QStringLiteral(
                    "Encrypted public mirror unavailable: the local device "
                    "identity could not unlock its age-key vault."),
                true);
        return;
    }

    const QString existingArchiveId = repo.publicArchiveId;
    const QString legacyMirrorPath = repo.mirrorPath;
    const QString managedMirrorRoot = repositoryMirrorRoot();
    QString source = repositorySource(repo);
    // A sealed mirror deliberately does not persist its plaintext
    // materialization. On process startup mirrorPath is therefore empty while
    // publicArchiveId remains available. Reopen that authenticated archive
    // first instead of cloning and re-encrypting the entire upstream before
    // this node can serve or publish anything. A later normal sync sees the
    // live temporary mirrorPath and refreshes from the upstream as usual.
    const bool reopeningSealedArchive =
        PublicMirrorRuntime::isArchiveId(existingArchiveId) &&
        !m_publicMirrorMaterializations.contains(existingArchiveId) &&
        (legacyMirrorPath.trimmed().isEmpty() ||
         !QDir(legacyMirrorPath).exists());
    // During a private→public transition, the authenticated private
    // materialization is the only name-free source. Prefer it over the legacy
    // named relay clone URL, which is intentionally inert for private bytes.
    if (PrivateMirrorStore::isOpaqueId(repo.privateReplicaId) &&
        QDir(legacyMirrorPath).exists()) {
        source = legacyMirrorPath;
    }
    if (source.isEmpty() && QDir(legacyMirrorPath).exists())
        source = legacyMirrorPath;
    // Keep the real source available as a recovery path. The old startup path
    // discarded it before attempting archive reopen, so an interrupted
    // ciphertext/metadata rotation produced an authentication error forever:
    // every retry only reopened the same damaged pair.
    const QString recoverySource = source;
    if (reopeningSealedArchive)
        source.clear();
    if (source.isEmpty() &&
        !PublicMirrorRuntime::isArchiveId(existingArchiveId)) {
        vaultSecret.fill('\0');
        vaultSecret.clear();
        if (!quiet)
            flashMessage(
                QStringLiteral(
                    "Encrypted public mirror unavailable: no authenticated "
                    "source or sealed archive exists."),
                true);
        return;
    }

    m_syncingRepos.insert(index, quiet);
    setRepoSyncActivity(index,
                        QStringLiteral("Preparing the encrypted mirror…"));
    refreshRepositoryList();
    logSystem(QStringLiteral(
        "Public mirror: sealing %1/%2 with official age encryption; "
        "plaintext is limited to owner-only temporary storage.")
                  .arg(repo.owner, repo.name));

    const bool managedCheckoutSource = false;
    qInfo().noquote()
        << "Public mirror sync mode:"
        << (managedCheckoutSource ? "managed-origin" : "ordinary")
        << (source.isEmpty() ? "archive-reopen" : "source-clone");
    QString upstreamUrl;
    if (managedCheckoutSource && !recoverySource.isEmpty()) {
        const QUrl upstream(repo.cloneUrl.trimmed());
        if (upstream.isValid() && !upstream.host().isEmpty() &&
            upstream.host().compare(catalogApiUrl().host(),
                                    Qt::CaseInsensitive) == 0)
            upstreamUrl = repo.cloneUrl.trimmed();
    }

    auto result = std::make_shared<PublicSyncWorkerResult>();
    const QString archiveRoot = publicArchiveRoot();
    const QString vaultPath = publicIdentityVaultPath();
    const QString owner = repo.owner;
    const QString name = repo.name;
    const std::shared_ptr<PublicMirrorMaterialization> openMaterialization =
        m_publicMirrorMaterializations.value(existingArchiveId);
    PublicMirrorRuntime::Progress progress = [this, index](const QString &line) {
        QMetaObject::invokeMethod(
            this, [this, index, line] { setRepoSyncActivity(index, line); },
            Qt::QueuedConnection);
    };
    QThread *worker = QThread::create(
        [result, source, recoverySource, upstreamUrl, managedCheckoutSource,
         archiveRoot, vaultPath, progress, openMaterialization,
         mutableVaultSecret = std::move(vaultSecret), existingArchiveId,
         legacyMirrorPath, managedMirrorRoot]() mutable {
            if (!upstreamUrl.isEmpty()) {
                progress(QStringLiteral("Fetching the managed checkout from "
                                        "its upstream…"));
                result->upstreamSummary =
                    forkmesh::upstream::refreshManagedCheckoutFromUpstream(
                        recoverySource, upstreamUrl)
                        .summary();
            }
            // If the source refs still match the seal and its authenticated
            // materialization is already alive, retaining that exact checkout
            // is the complete sync. Reopening the same large archive used to
            // emit a new advert and feed back into another sync request.
            if (!source.isEmpty() && openMaterialization &&
                openMaterialization->isValid() &&
                PublicMirrorRuntime::isArchiveId(existingArchiveId)) {
                progress(QStringLiteral(
                    "Checking whether the open encrypted mirror is current…"));
                const PublicMirrorRuntime::Metadata metadata =
                    PublicMirrorRuntime::readMetadata(
                        archiveRoot, existingArchiveId, nullptr);
                const QString refs =
                    PublicMirrorRuntime::repositoryRefsSha256(
                        source, PublicMirrorRuntime::Tools(), nullptr);
                if (metadata.isValid() && !refs.isEmpty() &&
                    refs == metadata.expectedRefsSha256) {
                    result->metadata = metadata;
                    result->materialization = openMaterialization;
                    result->reusedOpenMaterialization = true;
                    progress(QStringLiteral(
                        "Repository unchanged — encrypted mirror is already open."));
                }
            }
            if (!result->materialization && source.isEmpty()) {
                progress(QStringLiteral("Opening the sealed archive…"));
                result->metadata = PublicMirrorRuntime::readMetadata(
                    archiveRoot, existingArchiveId, &result->error);
                if (result->metadata.isValid()) {
                    std::unique_ptr<PublicMirrorMaterialization>
                        materialization =
                            PublicMirrorRuntime::materialize(
                                archiveRoot, vaultPath,
                                mutableVaultSecret, existingArchiveId,
                                PublicMirrorRuntime::Tools(),
                                &result->error);
                    if (materialization) {
                        result->materialization =
                            std::shared_ptr<PublicMirrorMaterialization>(
                                std::move(materialization));
                    }
                }
            }

            // A missing/corrupt archive is recoverable when the repository's
            // authenticated local/upstream source still exists. Re-seal the
            // same opaque archive id and replace both durable files, instead
            // of retrying the broken archive-reopen forever.
            const QString sealSource =
                source.isEmpty() ? recoverySource : source;
            if ((!result->metadata.isValid() || !result->materialization) &&
                !sealSource.isEmpty()) {
                const QString reopenError = result->error;
                result->error.clear();
                PublicMirrorRuntime::SyncResult sync =
                    managedCheckoutSource
                        ? PublicMirrorRuntime::syncManagedCheckout(
                              sealSource, archiveRoot, vaultPath,
                              mutableVaultSecret, existingArchiveId,
                              PublicMirrorRuntime::Tools(), &result->error,
                              progress)
                        : PublicMirrorRuntime::syncSource(
                              sealSource, {}, archiveRoot, vaultPath,
                              mutableVaultSecret, existingArchiveId,
                              PublicMirrorRuntime::Tools(), &result->error,
                              progress);
                qInfo().noquote()
                    << "Public mirror sync result:"
                    << (managedCheckoutSource ? "managed-origin" : "ordinary")
                    << (sync.metadata.isValid()
                            ? sync.metadata.expectedRefsSha256.left(12)
                            : QStringLiteral("failed"));
                result->metadata = sync.metadata;
                result->created = sync.created;
                if (sync.materialization) {
                    result->materialization =
                        std::shared_ptr<PublicMirrorMaterialization>(
                            std::move(sync.materialization));
                }
                if (result->metadata.isValid() && result->materialization &&
                    !reopenError.isEmpty()) {
                    result->notice = QStringLiteral(
                        "The damaged encrypted archive was rebuilt from its "
                        "authenticated repository source.");
                }
            }

            if (result->metadata.isValid() &&
                result->materialization &&
                result->materialization->isValid() &&
                !legacyMirrorPath.isEmpty() &&
                QDir(legacyMirrorPath).exists() &&
                legacyMirrorPath !=
                    result->materialization->repositoryPath()) {
                const QString root =
                    QFileInfo(managedMirrorRoot).canonicalFilePath();
                const QString target =
                    QFileInfo(legacyMirrorPath).canonicalFilePath();
                if (!root.isEmpty() && !target.isEmpty() &&
                    target.startsWith(
                        QDir::cleanPath(root) + QLatin1Char('/'))) {
                    QString cleanupError;
                    result->legacyRemoved =
                        PublicMirrorRuntime::removeManagedPlaintextMirror(
                            legacyMirrorPath, managedMirrorRoot,
                            &cleanupError);
                    if (!result->legacyRemoved) {
                        result->notice = QStringLiteral(
                            "The encrypted replacement is ready, but the legacy "
                            "plaintext mirror could not be removed. Publication "
                            "remains blocked until it is cleaned up.");
                    }
                }
            }
            mutableVaultSecret.fill('\0');
            mutableVaultSecret.clear();
        });
    connect(
        worker, &QThread::finished, this,
        [this, worker, result, index, owner, name, existingArchiveId,
         quiet] {
            worker->deleteLater();
            m_syncingRepos.remove(index);
            clearRepoSyncActivity(index);
            if (!result->upstreamSummary.isEmpty())
                logSystem(QStringLiteral("Mirror: %1/%2 %3")
                              .arg(owner, name, result->upstreamSummary));
            if (index < 0 || index >= m_repositories.size() ||
                m_repositories.at(index).owner != owner ||
                m_repositories.at(index).name != name ||
                m_repositories.at(index).isPrivate ||
                m_repositories.at(index).publicArchiveId !=
                    existingArchiveId) {
                refreshRepositoryList();
                return;
            }
            RepositoryRecord &current = m_repositories[index];
            if (!result->metadata.isValid() ||
                !result->materialization ||
                !result->materialization->isValid()) {
                refreshRepositoryList();
                qWarning().noquote()
                    << "Public mirror encrypted sync failed:"
                    << (result->error.isEmpty()
                            ? QStringLiteral("unknown safe failure")
                            : result->error);
                logSystem(QStringLiteral(
                              "Public mirror: encrypted sync failed for %1/%2. "
                              "No new durable plaintext mirror was created.")
                              .arg(owner, name));
                if (!quiet) {
                    flashMessage(
                        QStringLiteral(
                            "Encrypted public mirror failed for %1/%2: %3")
                            .arg(owner, name,
                                 result->error.isEmpty()
                                     ? QStringLiteral(
                                           "age, age-keygen, tar, Git, and a "
                                           "valid source are required")
                                     : result->error),
                        true);
                }
                return;
            }

            if (result->reusedOpenMaterialization) {
                refreshRepositoryList();
                if (index == m_repoDetailIndex)
                    refreshRepoSyncIndicators();
                if (!quiet) {
                    flashMessage(QStringLiteral("%1/%2 is already synced.")
                                     .arg(owner, name));
                }
                return;
            }

            if (PublicMirrorRuntime::isArchiveId(existingArchiveId) &&
                existingArchiveId != result->metadata.archiveId) {
                m_publicMirrorMaterializations.remove(
                    existingArchiveId);
            }
            current.publicArchiveId = result->metadata.archiveId;
            const QString previousPrivateReplica =
                current.privateReplicaId;
            current.privateReplicaId.clear();
            adoptMaterializedMirror(
                current, result->materialization->repositoryPath());
            current.lastSyncMs =
                QDateTime::currentMSecsSinceEpoch();
            m_publicMirrorMaterializations.insert(
                result->metadata.archiveId, result->materialization);
            if (PrivateMirrorStore::isOpaqueId(
                    previousPrivateReplica)) {
                m_privateMirrorMaterializations.remove(
                    previousPrivateReplica);
            }
            saveRepositories(); // never persists the temporary mirrorPath
            refreshRepositoryList();
            ensurePushHook(current);

            if (!result->legacyRemoved) {
                logSystem(QStringLiteral("Public mirror: ") +
                          result->notice);
                if (!quiet)
                    flashMessage(result->notice, true);
                return;
            }

            if (!result->notice.isEmpty())
                logSystem(QStringLiteral("Public mirror: ") + result->notice);

            logSystem(
                QStringLiteral(
                    "Public mirror: %1/%2 is stored as age ciphertext %3; "
                    "the authenticated repository is temporary.")
                    .arg(owner, name,
                         result->metadata.archiveId.left(12)));
            if (!quiet)
                flashMessage(
                    QStringLiteral(
                        "Encrypted public mirror ready for %1/%2.")
                        .arg(owner, name));
            if (index == m_repoDetailIndex)
                refreshOpenRepoDetail();
            scanRepoMentionsFor(current);
            if (current.publishToNetwork) {
                QString gatewayError;
                if (!rebuildDirectMirrorGatewayConfiguration(
                        &gatewayError, true)) {
                    logSystem(
                        QStringLiteral(
                            "Public mirror has no direct HTTPS gateway yet; "
                            "the managed mirror service will keep retrying "
                            "endpoint setup: %1")
                            .arg(gatewayError));
                    if (!quiet)
                        flashMessage(
                            QStringLiteral(
                                "Mirror is synced and visible; direct HTTPS "
                                "serving is still being configured."),
                            false);
                }
            }
            startNodeEventSocket();
            replicateReleaseArtifacts(index);
            pushToSshMirrorRemotes(index);
            completePendingRepoAutoOpen(index);
        });
    worker->start();
}

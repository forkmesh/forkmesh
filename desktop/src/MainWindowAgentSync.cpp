
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
#include "OrgAgentJobAttachments.h"
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

QJsonObject localCliAvailability(const QString &provider)
{
    const bool codex = agentIsCodexProvider(provider);
    const AgentAccountProfile account = activeAgentAccount(provider);
    const QString program =
        codex ? QStringLiteral("codex") : QStringLiteral("claude");
    if (forkmesh::vm::active()) {
        const QString unavailable = forkmesh::vm::availabilityError();
        const bool runtimeReady = unavailable.isEmpty();
        return {
            {QStringLiteral("provider"),
             codex ? QStringLiteral("codex") : QStringLiteral("claude-code")},
            {QStringLiteral("binaryFound"), runtimeReady},
            {QStringLiteral("loginState"),
             runtimeReady ? QStringLiteral("unchecked")
                          : QStringLiteral("missing")},
            {QStringLiteral("message"),
             runtimeReady
                 ? QStringLiteral(
                       "%1 installation and login are checked inside KVM when "
                       "the agent starts.")
                       .arg(codex ? QStringLiteral("Codex")
                                  : QStringLiteral("Claude Code"))
                 : unavailable},
            {QStringLiteral("credentialSource"), QStringLiteral("KVM guest")},
        };
    }
    const bool binaryFound = forkmesh::agentcli::installed(program);
    bool loggedIn = false;
    if (codex) {
        QFile auth(agentAccountCredentialPath(
            provider, activeAgentAccount(provider).configDir));
        if (auth.open(QIODevice::ReadOnly)) {
            const QJsonObject record =
                QJsonDocument::fromJson(auth.readAll()).object();
            loggedIn =
                !record.value(QStringLiteral("tokens")).toObject().isEmpty() ||
                !record.value(QStringLiteral("access_token")).toString().isEmpty() ||
                !record.value(QStringLiteral("OPENAI_API_KEY")).toString().isEmpty();
        }
        loggedIn = loggedIn ||
            (account.builtIn &&
             !qEnvironmentVariable("OPENAI_API_KEY").trimmed().isEmpty());
    } else {
        // Everything this gates runs the CLI, and the CLI redeems its own
        // refresh token. An idle node's access
        // token lapses within hours, so requiring a live one rejected website
        // jobs as "login missing" on a node that was signed in all along. The
        // mirror daemon (server/agents.go) already counts a refresh token.
        loggedIn = !claudeCodeOAuthToken(account.configDir).isEmpty() ||
                   claudeCodeHasRefreshableLogin(account.configDir);
        loggedIn = loggedIn ||
            (account.builtIn &&
             (!qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed().isEmpty() ||
              !qEnvironmentVariable("ANTHROPIC_AUTH_TOKEN").trimmed().isEmpty() ||
              !qEnvironmentVariable("CLAUDE_CODE_OAUTH_TOKEN").trimmed().isEmpty()));
    }
    QString credentialSource;
    if (codex) {
        credentialSource =
            account.builtIn &&
                    !qEnvironmentVariable("OPENAI_API_KEY").trimmed().isEmpty()
                ? QStringLiteral("OPENAI_API_KEY")
                : loggedIn ? QStringLiteral("device login") : QString();
    } else if (account.builtIn &&
        !qEnvironmentVariable("CLAUDE_CODE_OAUTH_TOKEN").trimmed().isEmpty()) {
        credentialSource = QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN");
    } else if (account.builtIn &&
        !qEnvironmentVariable("ANTHROPIC_AUTH_TOKEN").trimmed().isEmpty()) {
        credentialSource = QStringLiteral("ANTHROPIC_AUTH_TOKEN");
    } else if (account.builtIn &&
        !qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed().isEmpty()) {
        credentialSource = QStringLiteral("ANTHROPIC_API_KEY");
    } else if (loggedIn) {
        credentialSource = QStringLiteral("device login");
    }
    QString message;
    if (!binaryFound) {
        message = QStringLiteral(
            "%1 binary is missing on this mirror.")
                      .arg(codex ? QStringLiteral("Codex")
                                 : QStringLiteral("Claude Code"));
    } else if (!loggedIn) {
        message = QStringLiteral(
            "%1 login was not found on this mirror.")
                      .arg(codex ? QStringLiteral("Codex")
                                 : QStringLiteral("Claude Code"));
    } else {
        message = QStringLiteral("%1 is installed and authorized via %2.")
                      .arg(codex ? QStringLiteral("Codex")
                                 : QStringLiteral("Claude Code"),
                           credentialSource);
    }
    return {
        {QStringLiteral("provider"),
         codex ? QStringLiteral("codex") : QStringLiteral("claude-code")},
        {QStringLiteral("binaryFound"), binaryFound},
        {QStringLiteral("loginState"),
         loggedIn ? QStringLiteral("available") : QStringLiteral("missing")},
        {QStringLiteral("message"), message},
        {QStringLiteral("credentialSource"), credentialSource},
    };
}

QString agentE2EEControlKey(const QUrl &catalogUrl,
                            const RepositoryRecord &repo,
                            const QString &ownerKeyId)
{
    QUrl origin = catalogUrl;
    origin.setPath(QString());
    origin.setQuery(QString());
    origin.setFragment(QString());
    return origin.toString(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash) +
           QLatin1Char('|') + repo.owner.trimmed().toLower() +
           QLatin1Char('/') + repo.name.trimmed().toLower() +
           QLatin1Char('|') + ownerKeyId;
}

void clearOwnerIdentity(MirrorCrypto::Identity *identity)
{
    if (!identity)
        return;
    identity->x25519Priv.fill('\0');
    identity->mlkemPriv.fill('\0');
    identity->x25519Pub.clear();
    identity->x25519Priv.clear();
    identity->mlkemPub.clear();
    identity->mlkemPriv.clear();
}

QString acceptedAgentPromptToken(const QUrl &catalogUrl,
                                 const RepositoryRecord &repo,
                                 qint64 queueId,
                                 const QJsonObject &envelope)
{
    QUrl origin = catalogUrl;
    origin.setPath(QString());
    origin.setQuery(QString());
    origin.setFragment(QString());
    const QByteArray canonical =
        origin.toString(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash)
            .toUtf8() +
        '\n' + repo.owner.trimmed().toLower().toUtf8() + '/' +
        repo.name.trimmed().toLower().toUtf8() + '\n' +
        QByteArray::number(queueId) + '\n' +
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

bool agentPromptWasAccepted(const QString &token)
{
    return QSettings()
        .value(QStringLiteral("agents/acceptedEncryptedPromptTokens"))
        .toStringList()
        .contains(token);
}

void rememberAcceptedAgentPrompt(const QString &token)
{
    QSettings settings;
    QStringList tokens =
        settings
            .value(QStringLiteral("agents/acceptedEncryptedPromptTokens"))
            .toStringList();
    if (tokens.contains(token))
        return;
    tokens.append(token);
    constexpr int kAcceptedPromptJournalLimit = 1024;
    if (tokens.size() > kAcceptedPromptJournalLimit)
        tokens = tokens.mid(tokens.size() - kAcceptedPromptJournalLimit);
    settings.setValue(QStringLiteral("agents/acceptedEncryptedPromptTokens"),
                      tokens);
}

} // namespace

// ---- Owner-encrypted agent relay sync -------------------------
// Session snapshots are sealed to the owner's local hybrid identity before
// upload. The relay can route numeric ids but cannot read transcripts/results;
// encrypted prompts are opened and authenticated only in this desktop.

QUrl MainWindow::agentsApiUrl(const RepositoryRecord &repo) const
{
    QUrl url = catalogApiUrl();
    url.setPath("/api/repo/" + repoSegment(repo.owner, QStringLiteral("owner")) +
                "/" + repoSegment(repo.name, QStringLiteral("repository")) +
                "/agents");
    return url;
}

void MainWindow::ensureAgentE2EEControlPlane(
    RepositoryRecord repo, std::function<void(bool)> onDone)
{
    auto finish = [onDone = std::move(onDone)](bool ok) mutable {
        if (onDone)
            onDone(ok);
    };
    const QUrl agentsUrl = agentsApiUrl(repo);
    const QString sessionToken = accountSessionTokenForUrl(agentsUrl);
    if (!m_networkAccess || sessionToken.isEmpty() ||
        !m_profileIdentity.isValid() ||
        !hasOwnerSigningCapability(repo.owner)) {
        finish(false);
        return;
    }

    MirrorCrypto::Identity identity;
    QString identityError;
    if (!loadOwnerEncryptionIdentity(&identity, &identityError)) {
        logSystem(QStringLiteral(
            "Agent sync remains local: the owner encryption vault is unavailable."));
        finish(false);
        return;
    }
    const QString ownerKeyId = identity.keyId();
    const QJsonObject publicBundle = identity.publicBundle();
    clearOwnerIdentity(&identity);
    const QString controlKey =
        agentE2EEControlKey(agentsUrl, repo, ownerKeyId);
    if (m_agentE2EEReady.contains(controlKey)) {
        finish(true);
        return;
    }
    if (m_agentE2EEInFlight.contains(controlKey)) {
        finish(false);
        return;
    }
    m_agentE2EEInFlight.insert(controlKey);

    QUrl keyUrl = agentsUrl;
    keyUrl.setPath(QStringLiteral("/api/security/owner-keys"));
    keyUrl.setQuery(QString());
    keyUrl.setFragment(QString());
    QNetworkRequest keyRequest(keyUrl);
    keyRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                         QStringLiteral("application/json"));
    keyRequest.setRawHeader(
        "Authorization",
        QByteArrayLiteral("Bearer ") + sessionToken.toUtf8());
    QNetworkReply *keyReply = m_networkAccess->post(
        keyRequest,
        QJsonDocument(QJsonObject{
                          {QStringLiteral("publicBundle"), publicBundle},
                      })
            .toJson(QJsonDocument::Compact));
    connect(
        keyReply, &QNetworkReply::finished, this,
        [this, keyReply, repo, ownerKeyId, controlKey, agentsUrl, sessionToken,
         finish = std::move(finish)]() mutable {
            const QByteArray keyBody = keyReply->readAll();
            const int keyStatus =
                keyReply
                    ->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const bool keyNetworkOk =
                keyReply->error() == QNetworkReply::NoError;
            keyReply->deleteLater();
            const QJsonObject keyResponse =
                QJsonDocument::fromJson(keyBody).object();
            if (!keyNetworkOk || keyStatus < 200 || keyStatus >= 300 ||
                !keyResponse.value(QStringLiteral("ok")).toBool() ||
                keyResponse.value(QStringLiteral("keyId")).toString() !=
                    ownerKeyId ||
                keyResponse.value(QStringLiteral("privateKeysStored"))
                    .toBool(true)) {
                m_agentE2EEInFlight.remove(controlKey);
                logSystem(QStringLiteral(
                    "Agent sync remains local: the relay did not accept the "
                    "public-only owner encryption key."));
                finish(false);
                return;
            }

            QUrl policyUrl = agentsUrl;
            QString policyPath = policyUrl.path();
            if (policyPath.endsWith(QStringLiteral("/agents")))
                policyPath.chop(QStringLiteral("/agents").size());
            policyUrl.setPath(policyPath + QStringLiteral("/privacy"));
            policyUrl.setQuery(QString());
            policyUrl.setFragment(QString());
            QNetworkRequest policyRequest(policyUrl);
            policyRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                    QStringLiteral("application/json"));
            policyRequest.setRawHeader(
                "Authorization",
                QByteArrayLiteral("Bearer ") +
                    sessionToken.toUtf8());
            QNetworkReply *policyReply = m_networkAccess->post(
                policyRequest,
                QJsonDocument(QJsonObject{
                                  {QStringLiteral("ownerKeyId"), ownerKeyId},
                              })
                    .toJson(QJsonDocument::Compact));
            connect(
                policyReply, &QNetworkReply::finished, this,
                [this, policyReply, controlKey, ownerKeyId,
                 finish = std::move(finish)]() mutable {
                    const QByteArray policyBody = policyReply->readAll();
                    const int policyStatus =
                        policyReply
                            ->attribute(
                                QNetworkRequest::HttpStatusCodeAttribute)
                            .toInt();
                    const bool policyNetworkOk =
                        policyReply->error() == QNetworkReply::NoError;
                    policyReply->deleteLater();
                    const QJsonObject policy =
                        QJsonDocument::fromJson(policyBody).object();
                    const bool ok =
                        policyNetworkOk && policyStatus >= 200 &&
                        policyStatus < 300 &&
                        policy.value(QStringLiteral("ok")).toBool() &&
                        policy.value(QStringLiteral("ownerKeyId")).toString() ==
                            ownerKeyId &&
                        policy.value(QStringLiteral("requireAgentE2EE"))
                            .toBool() &&
                        policy.value(QStringLiteral("agentBoundary"))
                                .toString() ==
                            QLatin1String("owner-only-e2ee");
                    m_agentE2EEInFlight.remove(controlKey);
                    if (ok)
                        m_agentE2EEReady.insert(controlKey);
                    else
                        logSystem(QStringLiteral(
                            "Agent sync remains local: the relay did not "
                            "confirm its owner-only encryption boundary."));
                    finish(ok);
                });
        });
}

// The account's other enrolled owner devices — today a signed-in mobile node.
// Their public bundles are fetched from the same registry this desktop
// registers its own key with, so a device the owner revoked on the website
// stops receiving new snapshots on the next refresh.
QList<QJsonObject> MainWindow::companionOwnerBundles(const QString &ownerKeyId,
                                                     qint64 nowMs)
{
    constexpr qint64 kRefreshAfterMs = 10 * 60 * 1000;
    const qint64 fetchedAt = m_companionOwnerBundlesFetchedAtMs.value(ownerKeyId);
    if (fetchedAt <= 0 || nowMs - fetchedAt >= kRefreshAfterMs)
        refreshCompanionOwnerBundles(ownerKeyId);
    // A never-completed first fetch returns nothing, so the very first push
    // after launch is sealed to this desktop alone rather than being delayed.
    return m_companionOwnerBundles.value(ownerKeyId);
}

void MainWindow::refreshCompanionOwnerBundles(const QString &ownerKeyId)
{
    if (!m_networkAccess || ownerKeyId.isEmpty() ||
        m_companionOwnerBundleFetchesInFlight.contains(ownerKeyId))
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/security/owner-keys"));
    url.setQuery(QString());
    url.setFragment(QString());
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (sessionToken.isEmpty())
        return;

    m_companionOwnerBundleFetchesInFlight.insert(ownerKeyId);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader("Authorization",
                         QByteArrayLiteral("Bearer ") + sessionToken.toUtf8());
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, ownerKeyId] {
                const QByteArray payload = reply->readAll();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const bool networkOk = reply->error() == QNetworkReply::NoError;
                reply->deleteLater();
                m_companionOwnerBundleFetchesInFlight.remove(ownerKeyId);
                if (!networkOk || status < 200 || status >= 300)
                    return; // keep the previous list; the next push retries
                const QJsonObject response =
                    QJsonDocument::fromJson(payload).object();
                if (!response.value(QStringLiteral("ok")).toBool())
                    return;
                QList<QJsonObject> bundles;
                QStringList bundleKeyIds;
                QSet<QString> seen{ownerKeyId};
                for (const QJsonValue &value :
                     response.value(QStringLiteral("keys")).toArray()) {
                    const QJsonObject key = value.toObject();
                    const QJsonObject bundle =
                        key.value(QStringLiteral("publicBundle")).toObject();
                    // Trust the bundle bytes, never the row's stated id: the
                    // key id is a digest of the public keys, so recomputing it
                    // is what proves the two agree.
                    const QString keyId = MirrorCrypto::publicKeyId(bundle);
                    if (keyId.isEmpty() ||
                        keyId != key.value(QStringLiteral("keyId")).toString() ||
                        seen.contains(keyId))
                        continue;
                    seen.insert(keyId);
                    bundles.append(bundle);
                    bundleKeyIds.append(keyId);
                    // Leave room for this desktop's own wrap entry inside the
                    // recipient bound both runtimes enforce.
                    if (bundles.size() >= 5)
                        break;
                }
                // Compare the recipient set itself, not its size: swapping one
                // enrolled device for another leaves the count unchanged while
                // still meaning the next snapshot must be sealed differently.
                const QStringList previous =
                    m_companionOwnerBundleKeyIds.value(ownerKeyId);
                m_companionOwnerBundles.insert(ownerKeyId, bundles);
                m_companionOwnerBundleKeyIds.insert(ownerKeyId, bundleKeyIds);
                m_companionOwnerBundlesFetchedAtMs.insert(
                    ownerKeyId, QDateTime::currentMSecsSinceEpoch());
                if (previous != bundleKeyIds) {
                    logSystem(
                        bundles.isEmpty()
                            ? QStringLiteral(
                                  "Agent snapshots are sealed to this desktop "
                                  "only: no other owner device is enrolled.")
                            : QStringLiteral(
                                  "Agent snapshots are now sealed to this "
                                  "desktop and %1 other owner device%2.")
                                  .arg(bundles.size())
                                  .arg(bundles.size() == 1 ? QString()
                                                           : QStringLiteral("s")));
                    // Re-seal on the next snapshot rather than waiting for a
                    // session to change: the recipient set itself changed.
                    m_lastAgentPushPayload.clear();
                    scheduleAgentSessionsPush();
                }
            });
}

void MainWindow::pushAgentSessionsSnapshot()
{
    if (!m_networkAccess || !m_agentStore || m_repositories.isEmpty())
        return;
    // Off by default: a session started here stays local unless the user opts
    // into relaying owner-encrypted agent snapshots in Settings.
    if (!QSettings().value(kPublishAgentsToWebSetting, false).toBool())
        return;
    QHash<QString, QList<AgentSession>> byRepo;
    for (const AgentSession &s : m_agentSessions) {
        if (s.owner.isEmpty() || s.name.isEmpty())
            continue;
        // A cloud row came from the relay; the other devices read it there.
        if (s.cloud)
            continue;
        if (s.owner.compare(m_userName, Qt::CaseInsensitive) != 0)
            continue;
        if (s.deletedAtMs > 0 || s.associationOnly || isExternalSession(s.id))
            continue;
        byRepo[s.owner + "/" + s.name].append(s);
    }
    const QStringList published =
        QSettings().value(kPublishedAgentReposSetting).toStringList();
    if (byRepo.isEmpty() && published.isEmpty())
        return;

    QSet<QString> seen;
    for (const RepositoryRecord &repo : m_repositories) {
        if (repo.previewOnly || !repo.publishToNetwork)
            continue;
        const QString key = repo.owner + "/" + repo.name;
        if (seen.contains(key) ||
            (!byRepo.contains(key) && !published.contains(key)))
            continue;
        seen.insert(key);
        const RepositoryRecord writable = writableRecordFor(repo);
        IssueStore probe(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                         m_userName);
        if (!probe.canWrite())
            continue;
        pushAgentSessionsForRepo(repo, byRepo.value(key));
    }
}

void MainWindow::pushAgentSessionsForRepo(RepositoryRecord repo,
                                          QList<AgentSession> sessions)
{
    if (!m_networkAccess || !m_agentStore)
        return;
    if (sessions.size() > 300) // worker contract caps at 300
        sessions = sessions.mid(0, 300);

    const QString pushKey = repo.owner.trimmed().toLower() + QLatin1Char('/') +
                            repo.name.trimmed().toLower();
    if (m_agentSnapshotReadsInFlight.contains(pushKey)) {
        m_agentSnapshotReadsPending.insert(pushKey);
        return;
    }
    m_agentSnapshotReadsInFlight.insert(pushKey);

    AgentStore *store = m_agentStore;
    runOffThread<QHash<int, QString>>(
        [store, sessions] {
            QHash<int, QString> transcripts;
            transcripts.reserve(sessions.size());
            for (const AgentSession &session : sessions) {
                constexpr int kMaxTranscript = 16000;
                QString transcript = store->readLog(session);
                if (transcript.size() > kMaxTranscript)
                    transcript = transcript.right(kMaxTranscript);
                transcripts.insert(session.id, std::move(transcript));
            }
            return transcripts;
        },
        [this, repo, sessions, pushKey, store](
            QHash<int, QString> transcripts) mutable {
            m_agentSnapshotReadsInFlight.remove(pushKey);
            if (m_agentStore == store && m_networkAccess) {
                pushAgentSessionsForRepoWithTranscripts(
                    repo, std::move(sessions), std::move(transcripts));
            }
            if (m_agentSnapshotReadsPending.remove(pushKey)) {
                QTimer::singleShot(
                    0, this, &MainWindow::pushAgentSessionsSnapshot);
            }
        });
}

void MainWindow::pushAgentSessionsForRepoWithTranscripts(
    RepositoryRecord repo, QList<AgentSession> sessions,
    QHash<int, QString> transcripts)
{
    if (!m_networkAccess)
        return;

    MirrorCrypto::Identity ownerIdentity;
    QString identityError;
    if (!loadOwnerEncryptionIdentity(&ownerIdentity, &identityError)) {
        logSystem(QStringLiteral(
            "Agent sync remains local: the owner encryption vault is unavailable."));
        return;
    }
    const QString ownerKeyId = ownerIdentity.keyId();
    const QString controlKey =
        agentE2EEControlKey(catalogApiUrl(), repo, ownerKeyId);
    if (!m_agentE2EEReady.contains(controlKey)) {
        clearOwnerIdentity(&ownerIdentity);
        ensureAgentE2EEControlPlane(
            repo, [this, repo, sessions](bool ok) {
                if (ok)
                    pushAgentSessionsForRepo(repo, sessions);
            });
        return;
    }

    QJsonArray plainSessions;
    const QStringList providerCredentialValues =
        localProviderCredentialValues();
    for (const AgentSession &s : sessions) {
        const QString transcript = transcripts.value(s.id);
        const QJsonObject ownerSealedSnapshot{
            {"kind", "forkmesh.agent-session"},
            {"v", 1},
            {"credentialBoundary", "owner-device-only"},
            {"sharedWorkspaceBoundary",
             "owner-sealed-task-state-artifacts-only"},
            {"repositoryOwner", repo.owner.trimmed().toLower()},
            {"repositoryName", repo.name.trimmed().toLower()},
            {"id", s.id},
            {"issueNumber", s.issueNumber},
            {"issueTitle", s.issueTitle},
            {"priority", s.priority},
            {"status", s.status},
            {"provider", s.provider},
            {"model", s.model},
            {"branchName", s.branchName},
            {"createdAtMs", s.createdAtMs},
            {"startedAtMs", s.startedAtMs},
            {"finishedAtMs", s.finishedAtMs},
            {"numTurns", s.numTurns},
            {"durationMs", s.durationMs},
            {"costUsd", s.costUsd},
            {"lastError", s.lastError},
            {"transcript", transcript},
        };
        // Encryption protects this snapshot from the relay and teammates, but
        // credentials are outside the workspace data model entirely: redact
        // them before serialization even into an owner-only envelope.
        plainSessions.append(
            redactProviderCredentials(
                QJsonValue(ownerSealedSnapshot), providerCredentialValues));
    }

    QUrl url = agentsApiUrl(repo);
    const QString backoffKey = "agentPush:" + url.toString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_pollBackoff.ready(backoffKey, nowMs))
        return;

    const QString pushKey = repo.owner + "/" + repo.name;
    const QByteArray plainPayload =
        QJsonDocument(QJsonObject{{"sessions", plainSessions}})
            .toJson(QJsonDocument::Compact);
    if (m_lastAgentPushPayload.value(pushKey) == plainPayload) {
        clearOwnerIdentity(&ownerIdentity);
        return;
    }

    // This desktop's vault key comes first — it is the policy key the relay
    // requires — followed by the account's other enrolled owner devices, so a
    // signed-in phone can open the same snapshot without the relay ever
    // holding a key for either of them.
    QList<QJsonObject> recipientBundles{ownerIdentity.publicBundle()};
    recipientBundles.append(companionOwnerBundles(ownerKeyId, nowMs));
    QJsonArray encryptedSessions;
    for (const QJsonValue &value : plainSessions) {
        const QJsonObject session = value.toObject();
        QString sealError;
        const QJsonObject envelope = MirrorCrypto::sealOwnerPayload(
            QJsonDocument(session).toJson(QJsonDocument::Compact),
            recipientBundles, &sealError);
        if (envelope.isEmpty()) {
            clearOwnerIdentity(&ownerIdentity);
            logSystem(QStringLiteral(
                "Agent sync remains local: a session snapshot could not be "
                "owner-encrypted."));
            return;
        }
        encryptedSessions.append(QJsonObject{
            {QStringLiteral("id"), session.value(QStringLiteral("id"))},
            {QStringLiteral("envelope"), envelope},
        });
    }
    clearOwnerIdentity(&ownerIdentity);
    const QByteArray body =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("encryptedSessions"),
                           encryptedSessions},
                      })
            .toJson(QJsonDocument::Compact);

    const QString owner = repoSegment(repo.owner, QStringLiteral("owner"));
    if (!hasOwnerSigningCapability(owner))
        return;
    const QString ts = QString::number(nowMs);
    const QByteArray canonical =
        ("forkmesh-issues-pull-v1\n" + owner + "\n" + ts).toUtf8();
    const QString sig = m_profileIdentity.signData(canonical);

    QUrlQuery query;
    query.addQueryItem("owner", owner);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", sig);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(request, body);
    const bool anySession = !encryptedSessions.isEmpty();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, backoffKey, pushKey, plainPayload, controlKey,
             anySession] {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!networkOk) {
            if (status == 409 || status == 426 || status == 428)
                m_agentE2EEReady.remove(controlKey);
            m_pollBackoff.noteFailure(backoffKey, QDateTime::currentMSecsSinceEpoch());
            return;
        }
        m_pollBackoff.noteSuccess(backoffKey);
        m_lastAgentPushPayload.insert(pushKey, plainPayload);
        QSettings settings;
        QStringList published =
            settings.value(kPublishedAgentReposSetting).toStringList();
        const bool listed = published.contains(pushKey);
        if (anySession == listed)
            return;
        if (anySession)
            published.append(pushKey);
        else
            published.removeAll(pushKey);
        if (published.isEmpty())
            settings.remove(kPublishedAgentReposSetting);
        else
            settings.setValue(kPublishedAgentReposSetting, published);
    });
}

void MainWindow::scheduleAgentSessionsPush()
{
    // Every session change comes through here, relayed or not; a phone on the
    // local network hears of it directly.
    notifyLocalControlAgentsChanged();
    if (!m_agentSyncDebounceTimer) {
        m_agentSyncDebounceTimer = new QTimer(this);
        m_agentSyncDebounceTimer->setSingleShot(true);
        connect(m_agentSyncDebounceTimer, &QTimer::timeout, this,
                &MainWindow::pushAgentSessionsSnapshot);
    }
    m_agentSyncDebounceTimer->start(3000);
}

void MainWindow::drainAgentPrompts()
{
    if (!m_networkAccess || m_repositories.isEmpty())
        return;
    if (!QSettings().value(kPublishAgentsToWebSetting, false).toBool())
        return;
    QSet<QString> seen;
    for (const RepositoryRecord &repo : m_repositories) {
        if (repo.previewOnly || !repo.publishToNetwork)
            continue;
        const QString key = repo.owner + "/" + repo.name;
        if (seen.contains(key))
            continue;
        const RepositoryRecord writable = writableRecordFor(repo);
        IssueStore probe(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                         m_userName);
        if (!probe.canWrite())
            continue; // not the owner/hoster of this repo
        seen.insert(key);
        drainOrgAgentJobsFor(repo);
        drainAgentPromptsFor(repo);
    }
    reportCompletedOrgAgentJobs();
}

// What this machine can run right now, for callers outside this translation
// unit: the Agents list asks before it takes a queued website run over, so a
// node with no CLI installed says so instead of claiming the job and failing
// the person's session for them.
QJsonObject MainWindow::agentProviderAvailability(const QString &provider) const
{
    return localCliAvailability(provider);
}

void MainWindow::drainOrgAgentJobsFor(RepositoryRecord repo)
{
    if (!m_networkAccess || !hasOwnerSigningCapability(repo.owner))
        return;
    const QString drainKey =
        repo.owner.trimmed().toLower() + QLatin1Char('/') +
        repo.name.trimmed().toLower();
    if (m_orgAgentJobDrainsInFlight.contains(drainKey)) {
        m_orgAgentJobDrainsPending.insert(drainKey);
        return;
    }
    QUrl url = agentsApiUrl(repo);
    QString path = url.path();
    if (path.endsWith(QStringLiteral("/agents")))
        path.chop(QStringLiteral("/agents").size());
    url.setPath(path + QStringLiteral("/org-agent-jobs"));
    const QString backoffKey = QStringLiteral("orgAgentDrain:") + url.toString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_pollBackoff.ready(backoffKey, nowMs))
        return;
    url.setQuery(signedInboxQuery(
        repoSegment(repo.owner, QStringLiteral("owner"))));
    m_orgAgentJobDrainsInFlight.insert(drainKey);
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, backoffKey, drainKey] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        m_orgAgentJobDrainsInFlight.remove(drainKey);
        const bool drainAgain =
            m_orgAgentJobDrainsPending.remove(drainKey);
        if (!ok) {
            m_pollBackoff.noteFailure(
                backoffKey, QDateTime::currentMSecsSinceEpoch());
            return;
        }
        m_pollBackoff.noteSuccess(backoffKey);
        applyOrgAgentJobsPayload(
            repo, payload.value(QStringLiteral("jobs")).toArray());
        if (drainAgain) {
            QTimer::singleShot(0, this, [this, repo] {
                drainOrgAgentJobsFor(repo);
            });
        }
    });
}

void MainWindow::applyOrgAgentJobsPayload(const RepositoryRecord &repo,
                                          const QJsonArray &jobs)
{
    for (const QJsonValue &value : jobs) {
        const QJsonObject job = value.toObject();
        const qint64 jobId =
            qint64(job.value(QStringLiteral("jobId")).toDouble());
        const QString leaseId =
            job.value(QStringLiteral("leaseId")).toString();
        const QString sessionId =
            job.value(QStringLiteral("sessionId")).toString();
        const QString kind = job.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("stop")) {
            // The owner stopped this run from another of their devices (the
            // relay's "stop" action on the session). It stops only the local
            // run bound to that same relay session, through the same path as
            // the Stop button, and never starts anything.
            if (jobId <= 0 || leaseId.isEmpty() || sessionId.isEmpty())
                continue;
            const int localId =
                job.value(QStringLiteral("localAgentId")).toInt();
            const auto boundSession = [this, localId]() -> QString {
                const QJsonObject bound = m_orgAgentBindings.value(localId);
                if (!bound.isEmpty())
                    return bound.value(QStringLiteral("sessionId")).toString();
                const AgentSession *local = findAgentSession(localId);
                return local ? local->orgAgentJob.value(QStringLiteral("sessionId"))
                                   .toString()
                             : QString();
            };
            const bool ours = localId > 0 && boundSession() == sessionId;
            const bool stopped = ours && stopAgentSessionById(localId);
            logSystem(QStringLiteral("Website stop for agent session %1 (%2/%3): %4.")
                          .arg(sessionId, repo.owner, repo.name,
                               stopped ? QStringLiteral("stopped local agent %1")
                                             .arg(localId)
                                       : QStringLiteral("nothing was running")));
            reportOrgAgentJob(repo, job, QStringLiteral("approved"),
                              QStringLiteral("failed"), ours ? localId : 0,
                              stopped ? QStringLiteral("Stopped by the owner.")
                                      : QStringLiteral("Nothing was running on "
                                                       "this node for the session."));
            continue;
        }
        const QString provider =
            job.value(QStringLiteral("provider")).toString();
        const QString prompt =
            job.value(QStringLiteral("prompt")).toString();
        const QString requestedModel =
            codexChatGptModelId(job.value(QStringLiteral("model")).toString());
        const int issueNumber =
            job.value(QStringLiteral("issueNumber")).toInt();
        const QSet<QString> allowedWebsiteModels = {
            QStringLiteral("claude-haiku-4-5"),
            QStringLiteral("claude-sonnet-5"),
            QStringLiteral("claude-opus-5-5"),
            QStringLiteral("claude-opus-5"),
            QStringLiteral("claude-sonnet-4-6"),
            QStringLiteral("claude-opus-4-8"),
            QStringLiteral("claude-fable-5"),
            QStringLiteral("gpt-5.6-sol"),
            QStringLiteral("gpt-5.6-luna"),
            QStringLiteral("gpt-5.6-terra"),
            QStringLiteral("gpt-5.3-codex-spark"),
        };
        if (jobId <= 0 || leaseId.isEmpty() || sessionId.isEmpty() ||
            prompt.trimmed().isEmpty() || prompt.size() > 8000 ||
            (provider != QLatin1String("claude-code") &&
             !agentIsCodexProvider(provider)) ||
            (!requestedModel.isEmpty() &&
             !allowedWebsiteModels.contains(requestedModel)) ||
            issueNumber < 0 || issueNumber > 10000000 ||
            !forkmesh::orgagentjobs::parseAttachments(
                job.value(QStringLiteral("attachments")).toArray(), nullptr)) {
            reportOrgAgentJob(repo, job, QStringLiteral("rejected"),
                              QStringLiteral("rejected"), 0,
                              QStringLiteral("Malformed or unsafe job envelope."));
            continue;
        }
        const QString token =
            QStringLiteral("%1/%2:%3").arg(repo.owner, repo.name).arg(jobId);
        if (m_orgAgentJobsInFlight.contains(token))
            continue;
        m_orgAgentJobsInFlight.insert(token);
        const QString acceptedKey =
            QStringLiteral("orgAgentJobs/accepted/%1").arg(token);
        const int acceptedId = QSettings().value(acceptedKey, 0).toInt();
        if (acceptedId > 0) {
            const QString acceptedKind =
                job.value(QStringLiteral("kind")).toString();
            if (acceptedKind == QLatin1String("start") ||
                acceptedKind == QLatin1String("continue"))
                persistOrgAgentBinding(acceptedId, job);
            reportOrgAgentJob(repo, job, QStringLiteral("approved"),
                              QStringLiteral("running"), acceptedId,
                              QStringLiteral("Previously accepted exact job."));
            continue;
        }
        if (QSettings().value(acceptedKey, -1).toInt() == 0) {
            reportOrgAgentJob(repo, job, QStringLiteral("approved"),
                              QStringLiteral("running"),
                              job.value(QStringLiteral("localAgentId")).toInt(),
                              QStringLiteral("Previously accepted exact follow-up."));
            continue;
        }
        // Website agent jobs no longer run a model safety preflight; the node
        // accepts the run as work for the account that owns it.
        if (!job.value(QStringLiteral("attachments")).toArray().isEmpty() &&
            (kind == QLatin1String("start") || kind == QLatin1String("steer")))
            fetchOrgAgentJobAttachments(repo, job);
        else
            startApprovedOrgAgentJob(repo, job, QStringLiteral("Prompt dispatched."));
    }
}

void MainWindow::fetchOrgAgentJobAttachments(const RepositoryRecord &repo,
                                             QJsonObject job, QStringList saved)
{
    namespace jobattachments = forkmesh::orgagentjobs;
    QList<jobattachments::Attachment> attachments;
    jobattachments::parseAttachments(
        job.value(QStringLiteral("attachments")).toArray(), &attachments);
    const auto fail = [this, repo, job, saved](const QString &reason) {
        for (const QString &path : saved)
            QFile::remove(path);
        logSystem(QStringLiteral("Website agent job for %1/%2 failed: %3")
                      .arg(repo.owner, repo.name, reason));
        reportOrgAgentJob(repo, job, QStringLiteral("approved"),
                          QStringLiteral("failed"), 0, reason);
    };
    if (saved.size() >= attachments.size()) {
        job.insert(QStringLiteral("prompt"),
                   jobattachments::promptWithAttachments(
                       job.value(QStringLiteral("prompt")).toString(), saved));
        startApprovedOrgAgentJob(repo, job, QStringLiteral("Prompt dispatched."));
        return;
    }
    if (!m_networkAccess) {
        fail(QStringLiteral("This desktop could not download the prompt's images."));
        return;
    }
    const int index = saved.size();
    const jobattachments::Attachment attachment = attachments.at(index);
    const qint64 jobId = qint64(job.value(QStringLiteral("jobId")).toDouble());
    QUrl url = agentsApiUrl(repo);
    QString path = url.path();
    if (path.endsWith(QStringLiteral("/agents")))
        path.chop(QStringLiteral("/agents").size());
    url.setPath(path + QStringLiteral("/org-agent-jobs/%1/attachments/%2")
                           .arg(jobId)
                           .arg(attachment.id));
    QUrlQuery query = signedInboxQuery(repoSegment(repo.owner, QStringLiteral("owner")));
    query.addQueryItem(QStringLiteral("leaseId"),
                       job.value(QStringLiteral("leaseId")).toString());
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply, limit = attachment.size](qint64 received, qint64) {
        if (received > limit)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, job, saved, attachment, index,
             count = attachments.size(), fail] {
        const QByteArray data = reply->readAll();
        const QString networkError =
            reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        reply->deleteLater();
        const QString problem =
            !networkError.isEmpty() ? networkError
                                    : jobattachments::checkAttachment(attachment, data);
        const QString where = QStringLiteral("image %1 of %2").arg(index + 1).arg(count);
        if (!problem.isEmpty()) {
            fail(QStringLiteral("Could not fetch the prompt's %1: %2.").arg(where, problem));
            return;
        }
        const QString written = jobattachments::saveAttachment(
            AgentPromptImages::directory(), attachment, data);
        if (written.isEmpty()) {
            fail(QStringLiteral("Could not save the prompt's %1 on this desktop.").arg(where));
            return;
        }
        fetchOrgAgentJobAttachments(repo, job, saved + QStringList{written});
    });
}

void MainWindow::startApprovedOrgAgentJob(const RepositoryRecord &repo,
                                         const QJsonObject &job,
                                         const QString &reason)
{
    const QString kind =
        job.value(QStringLiteral("kind")).toString();
    int localAgentId = 0;
    if (kind == QLatin1String("steer")) {
        localAgentId =
            job.value(QStringLiteral("localAgentId")).toInt();
        if (!findAgentSession(localAgentId)) {
            reportOrgAgentJob(
                repo, job, QStringLiteral("approved"),
                QStringLiteral("failed"), 0,
                QStringLiteral("The scoped local agent session is unavailable."));
            return;
        }
        deliverQueuedAgentPrompt(
            localAgentId,
            job.value(QStringLiteral("prompt")).toString());
        // A follow-up to a finished run resumes it (deliverQueuedAgentPrompt
        // continues a Done session the way the composer does), and the
        // website has to hear how the resumed run ends. The binding that
        // reported the first run was cleared when it finished, so this job
        // takes over the reporting - unless the run is still bound to the
        // job that started it, which keeps reporting as before.
        if (!m_orgAgentBindings.contains(localAgentId))
            persistOrgAgentBinding(localAgentId, job);
    } else if (kind == QLatin1String("continue")) {
        // A session another of this account's desks moved into the cloud. The
        // work is already in Git on the named branch, so this node fetches the
        // branch and resumes on it rather than starting from an empty tree.
        localAgentId = startCloudHandoffJob(repo, job);
        if (localAgentId <= 0)
            return; // startCloudHandoffJob already reported why
        persistOrgAgentBinding(localAgentId, job);
    } else if (kind == QLatin1String("start")) {
        if (!forkmesh::vm::active()) {
            const QJsonObject providerAvailability =
                localCliAvailability(
                    job.value(QStringLiteral("provider")).toString());
            if (!providerAvailability
                     .value(QStringLiteral("binaryFound")).toBool()) {
                reportOrgAgentJob(
                    repo, job, QStringLiteral("approved"),
                    QStringLiteral("failed"), 0,
                    providerAvailability
                        .value(QStringLiteral("message")).toString());
                return;
            }
            if (providerAvailability
                    .value(QStringLiteral("loginState")).toString() !=
                QLatin1String("available")) {
                reportOrgAgentJob(
                    repo, job, QStringLiteral("approved"),
                    QStringLiteral("failed"), 0,
                    providerAvailability
                        .value(QStringLiteral("message")).toString());
                return;
            }
        }
        int repoIndex = -1;
        for (int i = 0; i < m_repositories.size(); ++i) {
            const RepositoryRecord &candidate = m_repositories.at(i);
            if (candidate.owner.compare(repo.owner, Qt::CaseInsensitive) == 0 &&
                candidate.name.compare(repo.name, Qt::CaseInsensitive) == 0 &&
                !candidate.localPath.isEmpty()) {
                repoIndex = i;
                break;
            }
        }
        const QString provider =
            job.value(QStringLiteral("provider")).toString();
        const QString requestedModel =
            job.value(QStringLiteral("model")).toString().trimmed();
        const int issueNumber =
            job.value(QStringLiteral("issueNumber")).toInt();
        const bool createPr =
            job.value(QStringLiteral("createPr")).toBool(false);
        if (repoIndex < 0) {
            reportOrgAgentJob(
                repo, job, QStringLiteral("approved"),
                QStringLiteral("failed"), 0,
                QStringLiteral("No eligible local checkout could start the agent."));
            return;
        }
        if (issueNumber > 0) {
            const RepositoryRecord &localRepo = m_repositories.at(repoIndex);
            const QList<Issue> issues =
                IssueStore(localRepo.localPath, localRepo.mirrorPath,
                           &m_profileIdentity, m_userName)
                    .loadAll();
            const Issue *selectedIssue = nullptr;
            for (const Issue &candidate : issues) {
                if (candidate.number == issueNumber) {
                    selectedIssue = &candidate;
                    break;
                }
            }
            if (!selectedIssue) {
                reportOrgAgentJob(
                    repo, job, QStringLiteral("approved"),
                    QStringLiteral("failed"), 0,
                    QStringLiteral(
                        "The commit-pinned issue record is unavailable on this mirror."));
                return;
            }
            localAgentId = startAgentForIssue(
                *selectedIssue, provider, createPr,
                /*quiet=*/true, requestedModel, &localRepo);
        } else {
            const QString requestedMode =
                job.value(QStringLiteral("mode")).toString().trimmed();
            const QString requestedEffort =
                job.value(QStringLiteral("effort")).toString().trimmed().toLower();
            static const QStringList kWebModes = {
                kClaudeAutoModeLabel, kAgentAskModeLabel, QStringLiteral("Plan"),
                QStringLiteral("Edit")};
            static const QStringList kWebEfforts = {
                QStringLiteral("low"), QStringLiteral("medium"),
                QStringLiteral("high"), QStringLiteral("xhigh"),
                QStringLiteral("max")};
            localAgentId = startAdHocAgentForRepo(
                repoIndex, job.value(QStringLiteral("prompt")).toString(),
                provider, createPr, requestedModel, QString(), false, true,
                QString(),
                kWebModes.contains(requestedMode) ? requestedMode : QString(),
                QString(), 50, false,
                kWebEfforts.contains(requestedEffort) ? requestedEffort
                                                      : QString());
        }
        if (localAgentId <= 0) {
            reportOrgAgentJob(
                repo, job, QStringLiteral("approved"),
                QStringLiteral("failed"), 0,
                QStringLiteral("No eligible local checkout could start the agent."));
            return;
        }
        persistOrgAgentBinding(localAgentId, job);
    } else {
        reportOrgAgentJob(repo, job, QStringLiteral("rejected"),
                          QStringLiteral("rejected"), 0,
                          QStringLiteral("Unknown job kind."));
        return;
    }
    const qint64 jobId =
        qint64(job.value(QStringLiteral("jobId")).toDouble());
    const QString acceptedKey = QStringLiteral(
        "orgAgentJobs/accepted/%1/%2:%3")
        .arg(repo.owner, repo.name)
        .arg(jobId);
    QSettings().setValue(acceptedKey, localAgentId);
    reportOrgAgentJob(repo, job, QStringLiteral("approved"),
                      QStringLiteral("running"), localAgentId, reason);
}

// Take delivery of a session another of this account's desks moved into the
// cloud. The job names a branch the origin published to its served mirror, so
// the only thing missing here is the history: fetch it, then resume on that
// branch with the task the origin was working on. A branch that has not
// reached this node's mirror yet is reported as a failure the operator can act
// on rather than started on the wrong tree.
int MainWindow::startCloudHandoffJob(const RepositoryRecord &repo,
                                     const QJsonObject &job)
{
    const auto reject = [&](const QString &reason) {
        reportOrgAgentJob(repo, job, QStringLiteral("approved"),
                          QStringLiteral("failed"), 0, reason);
        return 0;
    };
    const QString branch = job.value(QStringLiteral("branch")).toString().trimmed();
    if (branch.isEmpty())
        return reject(QStringLiteral("The handoff named no branch to continue."));
    int repoIndex = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &candidate = m_repositories.at(i);
        if (candidate.owner.compare(repo.owner, Qt::CaseInsensitive) == 0 &&
            candidate.name.compare(repo.name, Qt::CaseInsensitive) == 0 &&
            !candidate.localPath.isEmpty()) {
            repoIndex = i;
            break;
        }
    }
    if (repoIndex < 0)
        return reject(QStringLiteral(
            "No eligible local checkout could continue the moved session."));
    const RepositoryRecord local = writableRecordFor(m_repositories.at(repoIndex));
    const QString gitDir = local.localPath.trimmed();
    if (gitDir.isEmpty())
        return reject(QStringLiteral(
            "This node holds the repository read-only, so it cannot continue "
            "the moved session."));
    if (!fetchServedBranch(local, branch, /*update=*/false))
        return reject(
            QStringLiteral("Branch %1 has not reached this node yet, so "
                           "the moved session has no history to continue.")
                .arg(branch));
    const QString prompt = job.value(QStringLiteral("prompt")).toString();
    QString title = job.value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty())
        title = prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
    const int sessionId = startAgentOnExistingBranch(
        repoIndex, branch,
        job.value(QStringLiteral("baseBranch")).toString().trimmed(),
        /*prNumber=*/0, title, prompt,
        job.value(QStringLiteral("provider")).toString(),
        codexChatGptModelId(job.value(QStringLiteral("model")).toString()));
    if (sessionId <= 0)
        return reject(QStringLiteral(
            "This node could not open a worktree on the moved session's "
            "branch."));
    if (AgentSession *session = findAgentSession(sessionId); session && m_agentStore) {
        const QString from =
            job.value(QStringLiteral("fromDevice")).toString().trimmed();
        m_agentStore->appendLog(
            *session,
            QStringLiteral("==> Continuing a session moved into the cloud%1, "
                           "on branch %2.\n")
                .arg(from.isEmpty() ? QString()
                                    : QStringLiteral(" from %1").arg(from),
                     branch));
    }
    return sessionId;
}

bool MainWindow::fetchServedBranch(const RepositoryRecord &local,
                                   const QString &branch, bool update)
{
    const QString gitDir = local.localPath.trimmed();
    if (gitDir.isEmpty() || branch.trimmed().isEmpty())
        return false;
    if (!update && !worktreeBranchCommit(gitDir, branch).isEmpty())
        return true;
    const QString refspec =
        QStringLiteral("refs/heads/%1:refs/heads/%1").arg(branch);
    for (const QString &source : {local.mirrorPath.trimmed(),
                                  local.cloneUrl.trimmed()}) {
        if (source.isEmpty())
            continue;
        if (runGitCapture(gitDir,
                          {QStringLiteral("fetch"), QStringLiteral("--quiet"),
                           QStringLiteral("--no-tags"), source, refspec},
                          nullptr, nullptr))
            return !worktreeBranchCommit(gitDir, branch).isEmpty();
    }
    return false;
}

void MainWindow::persistOrgAgentBinding(int localAgentId,
                                        const QJsonObject &job)
{
    if (localAgentId <= 0 || job.isEmpty())
        return;
    m_orgAgentBindings.insert(localAgentId, job);
    AgentSession *session = findAgentSession(localAgentId);
    if (!session)
        return;
    // Which website run this local run is. The lease below is cleared when the
    // result is acknowledged; this is not, so the Agents list keeps showing
    // the finished run once rather than as a local row and a cloud row.
    const QString webSessionId =
        job.value(QStringLiteral("sessionId")).toString().trimmed();
    const bool linked = !webSessionId.isEmpty() &&
                        session->webSessionId != webSessionId;
    if (!linked && session->orgAgentJob == job)
        return;
    if (linked)
        session->webSessionId = webSessionId;
    session->orgAgentJob = job;
    if (m_agentStore)
        m_agentStore->saveSession(*session);
}

void MainWindow::clearOrgAgentBinding(int localAgentId)
{
    if (localAgentId <= 0)
        return;
    AgentSession *session = findAgentSession(localAgentId);
    if (session && !session->orgAgentJob.isEmpty()) {
        session->orgAgentJob = QJsonObject();
        if (m_agentStore)
            m_agentStore->saveSession(*session);
    }
    m_orgAgentBindings.remove(localAgentId);
}

void MainWindow::reportOrgAgentJob(const RepositoryRecord &repo,
                                   const QJsonObject &job,
                                   const QString &securityVerdict,
                                   const QString &status,
                                   int localAgentId,
                                   const QString &reason,
                                   const QString &result)
{
    if (!m_networkAccess)
        return;
    const qint64 jobId =
        qint64(job.value(QStringLiteral("jobId")).toDouble());
    QUrl url = agentsApiUrl(repo);
    QString path = url.path();
    if (path.endsWith(QStringLiteral("/agents")))
        path.chop(QStringLiteral("/agents").size());
    url.setPath(path + QStringLiteral("/org-agent-jobs/%1/result").arg(jobId));
    url.setQuery(signedInboxQuery(
        repoSegment(accountOwner().trimmed().toLower(), QStringLiteral("owner"))));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    AgentSession *session =
        localAgentId > 0 ? findAgentSession(localAgentId) : nullptr;
    QJsonObject agentInfo;
    if (session) {
        agentInfo = {
            {QStringLiteral("owner"), session->owner},
            {QStringLiteral("repository"), session->name},
            {QStringLiteral("provider"), session->provider},
            {QStringLiteral("model"), session->model},
            {QStringLiteral("mode"), session->mode},
            {QStringLiteral("status"), session->status},
            {QStringLiteral("issueNumber"), session->issueNumber},
            {QStringLiteral("prNumber"), session->prNumber},
            {QStringLiteral("createPr"), session->createPr},
            {QStringLiteral("branchName"), session->branchName},
            {QStringLiteral("baseRef"), session->baseRef},
            {QStringLiteral("baseBranch"), session->baseBranch},
            {QStringLiteral("merged"), session->merged},
            {QStringLiteral("mergedAt"), double(session->mergedAtMs)},
            {QStringLiteral("createdAt"), double(session->createdAtMs)},
            {QStringLiteral("startedAt"), double(session->startedAtMs)},
            {QStringLiteral("finishedAt"), double(session->finishedAtMs)},
            {QStringLiteral("promptTokens"), session->promptTokens},
            {QStringLiteral("completionTokens"), session->completionTokens},
            {QStringLiteral("totalTokens"), session->totalTokens},
            {QStringLiteral("contextTokens"), session->contextTokens},
            {QStringLiteral("contextWindow"), session->contextWindow},
            {QStringLiteral("maxOutputTokens"), session->maxOutputTokens},
            {QStringLiteral("estimatedCredits"), session->estimatedCredits},
            {QStringLiteral("costUsd"), session->costUsd},
            {QStringLiteral("numTurns"), session->numTurns},
            {QStringLiteral("durationMs"), double(session->durationMs)},
            {QStringLiteral("lastError"), session->lastError.left(1000)},
        };
    }
    QJsonObject availability = localCliAvailability(
        job.value(QStringLiteral("provider")).toString());
    if (reason.contains(QStringLiteral("Claude Code"), Qt::CaseInsensitive) &&
        (reason.contains(QStringLiteral("missing"), Qt::CaseInsensitive) ||
         reason.contains(QStringLiteral("login"), Qt::CaseInsensitive))) {
        availability =
            localCliAvailability(QStringLiteral("claude-code"));
        availability.insert(QStringLiteral("message"), reason.left(400));
    }
    QJsonObject payload{
        {QStringLiteral("leaseId"),
         job.value(QStringLiteral("leaseId")).toString()},
        {QStringLiteral("securityVerdict"), securityVerdict},
        {QStringLiteral("securityReason"), reason.left(240)},
        {QStringLiteral("status"), status},
        {QStringLiteral("localAgentId"), localAgentId},
        {QStringLiteral("result"), result.left(16000)},
        {QStringLiteral("agentInfo"), agentInfo},
        {QStringLiteral("availability"), availability},
    };
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, job, status, localAgentId] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const qint64 jobId =
            qint64(job.value(QStringLiteral("jobId")).toDouble());
        const QString token =
            QStringLiteral("%1/%2:%3").arg(repo.owner, repo.name).arg(jobId);
        m_orgAgentJobsInFlight.remove(token);
        if (!ok)
            return;
        if (status != QLatin1String("running") && localAgentId > 0) {
            clearOrgAgentBinding(localAgentId);
            QSettings().remove(
                QStringLiteral("orgAgentJobs/accepted/%1").arg(token));
        }
        drainOrgAgentJobsFor(repo);
    });
}

void MainWindow::reportCompletedOrgAgentJobs()
{
    const QList<int> ids = m_orgAgentBindings.keys();
    for (int id : ids) {
        AgentSession *session = findAgentSession(id);
        if (!session)
            continue;
        QString status;
        if (session->status == AgentStatus::Success)
            status = QStringLiteral("completed");
        else if (session->status == AgentStatus::Failed ||
                 session->status == AgentStatus::Stopped)
            status = QStringLiteral("failed");
        if (status.isEmpty())
            continue;
        const QJsonObject job = m_orgAgentBindings.value(id);
        RepositoryRecord repo;
        bool found = false;
        for (const RepositoryRecord &candidate : m_repositories) {
            if (candidate.owner.compare(session->owner, Qt::CaseInsensitive) == 0 &&
                candidate.name.compare(session->name, Qt::CaseInsensitive) == 0) {
                repo = candidate;
                found = true;
                break;
            }
        }
        if (!found)
            continue;
        const qint64 jobId =
            qint64(job.value(QStringLiteral("jobId")).toDouble());
        const QString token =
            QStringLiteral("%1/%2:%3").arg(repo.owner, repo.name).arg(jobId);
        if (m_orgAgentJobsInFlight.contains(token))
            continue;
        m_orgAgentJobsInFlight.insert(token);
        if (!m_agentStore) {
            m_orgAgentJobsInFlight.remove(token);
            continue;
        }
        AgentStore *store = m_agentStore;
        const AgentSession snapshot = *session;
        runOffThread<QString>(
            [store, snapshot] {
                QString result = store->readLog(snapshot);
                if (result.size() > 16000)
                    result = result.right(16000);
                return result;
            },
            [this, repo, job, status, id, token, store](QString result) {
                if (m_agentStore != store || !m_networkAccess) {
                    m_orgAgentJobsInFlight.remove(token);
                    return;
                }
                reportOrgAgentJob(
                    repo, job, QStringLiteral("approved"), status, id,
                    QStringLiteral("Haiku-approved exact prompt."), result);
            });
    }
}

void MainWindow::drainAgentPromptsFor(RepositoryRecord repo)
{
    if (!m_networkAccess)
        return;
    MirrorCrypto::Identity ownerIdentity;
    QString identityError;
    if (!loadOwnerEncryptionIdentity(&ownerIdentity, &identityError))
        return;
    const QString controlKey =
        agentE2EEControlKey(catalogApiUrl(), repo, ownerIdentity.keyId());
    clearOwnerIdentity(&ownerIdentity);
    if (!m_agentE2EEReady.contains(controlKey)) {
        ensureAgentE2EEControlPlane(
            repo, [this, repo](bool ok) {
                if (ok)
                    drainAgentPromptsFor(repo);
            });
        return;
    }
    QUrl url = agentsApiUrl(repo);
    const QString backoffKey = "agentDrain:" + url.toString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_pollBackoff.ready(backoffKey, nowMs))
        return;

    const QString owner = repoSegment(repo.owner, QStringLiteral("owner"));
    if (!hasOwnerSigningCapability(owner))
        return;
    const QString ts = QString::number(nowMs);
    const QByteArray canonical =
        ("forkmesh-issues-pull-v1\n" + owner + "\n" + ts).toUtf8();
    const QString sig = m_profileIdentity.signData(canonical);

    QUrlQuery query;
    query.addQueryItem("owner", owner);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", sig);
    url.setQuery(query);

    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, backoffKey, repo, controlKey] {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QByteArray responseBody = reply->readAll();
        reply->deleteLater();
        if (!networkOk) {
            if (status == 409 || status == 426 || status == 428)
                m_agentE2EEReady.remove(controlKey);
            m_pollBackoff.noteFailure(backoffKey, QDateTime::currentMSecsSinceEpoch());
            return;
        }
        m_pollBackoff.noteSuccess(backoffKey);
        const QJsonObject payload =
            QJsonDocument::fromJson(responseBody).object();
        if (!payload.value(QStringLiteral("prompts")).toArray().isEmpty())
            logSystem(QStringLiteral(
                "Rejected relay-readable agent prompts; owner encryption is "
                "mandatory."));
        applyAgentPromptsPayload(
            repo,
            payload.value(QStringLiteral("encryptedPrompts")).toArray());
    });
}

// Authenticate and open owner-sealed steering prompts locally, then deliver
// them to their agent sessions. The clear routing id is cross-checked against
// the authenticated payload; relay-readable legacy prompts are never executed.
void MainWindow::applyAgentPromptsPayload(const RepositoryRecord &repo,
                                          const QJsonArray &prompts)
{
    if (!QSettings().value(kPublishAgentsToWebSetting, false).toBool() ||
        !hasOwnerSigningCapability(repo.owner))
        return;
    MirrorCrypto::Identity ownerIdentity;
    QString identityError;
    if (!loadOwnerEncryptionIdentity(&ownerIdentity, &identityError)) {
        logSystem(QStringLiteral(
            "Could not open queued agent prompts: the owner encryption vault "
            "is unavailable."));
        return;
    }
    QList<qint64> acceptedQueueIds;
    for (const QJsonValue &value : prompts) {
        const QJsonObject transport = value.toObject();
        const QJsonObject envelope =
            transport.value(QStringLiteral("envelope")).toObject();
        const qint64 queueId =
            qint64(transport.value(QStringLiteral("queueId")).toDouble());
        if (queueId <= 0 || envelope.isEmpty()) {
            logSystem(QStringLiteral(
                "Rejected a relay-readable or malformed agent prompt."));
            continue;
        }
        const QString acceptedToken = acceptedAgentPromptToken(
            catalogApiUrl(), repo, queueId, envelope);
        if (agentPromptWasAccepted(acceptedToken)) {
            acceptedQueueIds.append(queueId);
            continue;
        }

        QString openError;
        const QByteArray plaintext = MirrorCrypto::openOwnerPayload(
            envelope, ownerIdentity, &openError);
        QJsonParseError parseError;
        const QJsonDocument promptDocument =
            QJsonDocument::fromJson(plaintext, &parseError);
        if (plaintext.isEmpty() ||
            parseError.error != QJsonParseError::NoError ||
            !promptDocument.isObject()) {
            logSystem(QStringLiteral(
                "Could not authenticate a queued owner-encrypted agent prompt."));
            continue;
        }
        const QJsonObject item = promptDocument.object();
        const QString text = item.value("text").toString();
        const bool scopedPrompt =
            item.value(QStringLiteral("kind")).toString() ==
                QLatin1String("forkmesh.agent-prompt") &&
            item.value(QStringLiteral("v")).toInt() == 1 &&
            item.value(QStringLiteral("repositoryOwner"))
                    .toString()
                    .compare(repo.owner.trimmed(), Qt::CaseInsensitive) == 0 &&
            item.value(QStringLiteral("repositoryName"))
                    .toString()
                    .compare(repo.name.trimmed(), Qt::CaseInsensitive) == 0;
        QString agentId;
        if (item.value(QStringLiteral("agentId")).isString())
            agentId = item.value(QStringLiteral("agentId")).toString();
        else if (item.value(QStringLiteral("agentId")).isDouble())
            agentId =
                QString::number(item.value(QStringLiteral("agentId")).toInt());
        const int routedAgentId =
            transport.value(QStringLiteral("agentId")).toInt();
        bool numericAgentIdOk = false;
        const int numericAgentId = agentId.toInt(&numericAgentIdOk);
        if (!scopedPrompt || text.trimmed().isEmpty() || text.size() > 8000 ||
            agentId.isEmpty() ||
            (routedAgentId > 0 &&
             (!numericAgentIdOk || numericAgentId != routedAgentId))) {
            logSystem(QStringLiteral(
                "Rejected a malformed owner-encrypted agent prompt."));
            continue;
        }
        rememberAcceptedAgentPrompt(acceptedToken);
        acceptedQueueIds.append(queueId);
        if (agentId == QLatin1String("new")) {
            QStringList images;
            const QJsonArray imageArr = item.value("images").toArray();
            for (const QJsonValue &imageValue : imageArr) {
                const QString src = imageValue.toString();
                if (!src.isEmpty())
                    images.append(src);
            }
            startWebNewAgentForRepo(repo, text,
                                    item.value("provider").toString(), images);
            continue;
        }
        bool ok = false;
        const int sessionId = agentId.toInt(&ok);
        if (!ok) {
            logSystem(QStringLiteral(
                "Dropped a malformed website agent prompt."));
            continue;
        }
        deliverQueuedAgentPrompt(sessionId, text);
    }
    clearOwnerIdentity(&ownerIdentity);
    if (!acceptedQueueIds.isEmpty())
        acknowledgeAgentPrompts(repo, acceptedQueueIds);
}

void MainWindow::acknowledgeAgentPrompts(
    const RepositoryRecord &repo, const QList<qint64> &queueIds)
{
    if (!m_networkAccess || queueIds.isEmpty() ||
        !hasOwnerSigningCapability(repo.owner))
        return;
    QJsonArray ids;
    QSet<qint64> seen;
    for (const qint64 id : queueIds) {
        if (id > 0 && !seen.contains(id)) {
            seen.insert(id);
            ids.append(double(id));
        }
    }
    if (ids.isEmpty())
        return;
    QUrl url = agentsApiUrl(repo);
    url.setPath(url.path() + QStringLiteral("/ack"));
    url.setQuery(signedInboxQuery(
        repoSegment(repo.owner, QStringLiteral("owner"))));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{
                          {QStringLiteral("queueIds"), ids},
                      })
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply] {
        reply->deleteLater();
    });
}

void MainWindow::deliverQueuedAgentPrompt(int sessionId, const QString &text)
{
    if (!findAgentSession(sessionId)) {
        logSystem(QStringLiteral(
            "Dropped a website prompt: no local agent session #%1.")
                      .arg(sessionId));
        return;
    }
    sendPromptToAgentSession(sessionId, text);
}

void MainWindow::startWebNewAgentForRepo(const RepositoryRecord &repo,
                                         const QString &task,
                                         const QString &providerOverride,
                                         const QStringList &images)
{
    int repoIndex = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        if (r.owner.compare(repo.owner, Qt::CaseInsensitive) == 0
            && r.name.compare(repo.name, Qt::CaseInsensitive) == 0
            && !r.localPath.isEmpty()) {
            repoIndex = i;
            break;
        }
    }
    if (repoIndex < 0) {
        logSystem(QStringLiteral(
            "Dropped a website \"new agent\" prompt: no local checkout for %1/%2.")
                      .arg(repo.owner, repo.name));
        return;
    }
    const QString provider =
        (providerOverride == QLatin1String("claude-code")
         || providerOverride == QLatin1String("claude-api")
         || agentIsCodexProvider(providerOverride)
         || providerOverride == QLatin1String("openai"))
            ? providerOverride
            : defaultAgentProvider();
    const QString model = provider == QLatin1String("claude-code")
                              ? QSettings().value(kClaudeCodeModelSetting).toString()
                              : QString();
    QString finalTask = task;
    const QStringList savedImages = saveWebAgentImages(repo, images);
    if (!savedImages.isEmpty()) {
        const bool one = savedImages.size() == 1;
        const QString shots = one ? QStringLiteral("screenshot")
                                  : QStringLiteral("screenshots");
        const QString them = one ? QStringLiteral("it") : QStringLiteral("them");
        QString note = QStringLiteral(
            "\n\nThe user attached %1 %2 with this prompt. "
            "Open %3 with your file/image tools to view %3:")
                           .arg(QString::number(savedImages.size()), shots, them);
        for (const QString &path : savedImages)
            note += QStringLiteral("\n- %1").arg(path);
        finalTask += note;
    }
    logSystem(QStringLiteral("Starting a new agent for %1/%2 from a website prompt%3.")
                  .arg(repo.owner, repo.name,
                       savedImages.isEmpty()
                           ? QString()
                           : QStringLiteral(" (%1 screenshot(s) attached)")
                                 .arg(QString::number(savedImages.size()))));
    startAdHocAgentForRepo(repoIndex, finalTask, provider, /*createPr=*/false, model);
}

QStringList MainWindow::saveWebAgentImages(const RepositoryRecord &repo,
                                           const QStringList &images)
{
    QStringList paths;
    if (images.isEmpty())
        return paths;
    QString baseDir =
        forkmesh::paths::dataRoot();
    if (baseDir.isEmpty())
        baseDir = QDir::tempPath();
    QDir dir(baseDir + QStringLiteral("/agent-screenshots/%1-%2-%3")
                           .arg(repo.owner, repo.name)
                           .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!dir.mkpath(QStringLiteral(".")))
        return paths;
    int index = 0;
    for (const QString &src : images) {
        const int comma = src.indexOf(QLatin1Char(','));
        if (!src.startsWith(QLatin1String("data:image/")) || comma < 0)
            continue;
        const QString header = src.left(comma);
        if (!header.contains(QLatin1String("base64")))
            continue;
        QString subtype = header.mid(QStringLiteral("data:image/").size());
        subtype = subtype.section(QLatin1Char(';'), 0, 0).toLower();
        bool cleanSubtype = !subtype.isEmpty();
        for (const QChar &ch : subtype) {
            if (!ch.isLetterOrNumber()) {
                cleanSubtype = false;
                break;
            }
        }
        QString ext = subtype == QLatin1String("jpeg") ? QStringLiteral("jpg")
                      : cleanSubtype                   ? subtype
                                                       : QStringLiteral("png");
        const QByteArray bytes = QByteArray::fromBase64(
            src.mid(comma + 1).toLatin1());
        if (bytes.isEmpty())
            continue;
        const QString path = dir.filePath(
            QStringLiteral("screenshot-%1.%2").arg(QString::number(++index), ext));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) {
            file.close();
            paths.append(QDir::toNativeSeparators(path));
        }
    }
    return paths;
}

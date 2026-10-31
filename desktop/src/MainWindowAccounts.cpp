// Profile and account session handling: profile rename, logout, recent
// accounts, account switching and uninstall. Split out of
// MainWindowRepos.cpp.

#include "MainWindow.h"
#include "ProcessReaper.h"
#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
#include "SettingsSectionsView.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "UserAvatarStore.h"
#include "SettingsWrite.h"
#include "SafeDelete.h"
#include "UserActivityLog.h"

#include <QDateTimeEdit>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QStandardPaths>
#include <QUuid>

using namespace forkmesh::ui;

void MainWindow::onProfileNameChanged(const QString &name)
{
    const QString trimmed = accountNameFromInput(name, QString());
    if (trimmed.isEmpty()) {
        if (m_settingsNameEdit)
            m_settingsNameEdit->setText(m_userName);
        return;
    }
    if (m_settingsNameEdit && m_settingsNameEdit->text() != trimmed)
        m_settingsNameEdit->setText(trimmed);
    if (m_nameEdit && m_nameEdit->text() != trimmed)
        m_nameEdit->setText(trimmed);
    if (trimmed == m_userName)
        return;
    const QString oldOwner =
        accountNameFromInput(m_accountName.isEmpty() ? m_userName : m_accountName,
                             QStringLiteral("owner"));
    setDesktopCapability(m_accountName, false);
    m_accountAuthenticated = false;
    m_accountEmail.clear();
    m_accountTier = QStringLiteral("free");
    m_accountSolanaVerified = false;
    m_isAdmin = false;
    updateAdminCrownBadge();
    m_adminPendingPolled = false;
    forgetOwnUserStatus();
    forgetUserActivitySync();
    QSettings().remove(kAuthedAccountSetting);
    m_userName = trimmed;
    m_accountName = trimmed;
    saveProfileName(trimmed);
    migrateReposForProfileName(oldOwner, trimmed);
    refreshSettingsEmailVerifiedBadge();
    updateUserSwitcher();
    updateChatIdentity();
    refreshRepositoryList();
    logSystem("Name changed to " + trimmed + ".");
}

void MainWindow::onAvatarChosen(const QByteArray &pngData)
{
    m_userAvatar = pngData;
    UserAvatarStore::store(pngData);
    updateAvatarButton();
    updateUserAvatarButton();
    updateChatIdentity();
    pushAccountAvatar();
}

void MainWindow::logout()
{
    const QString previousAccount = m_accountName;
    if (m_heartbeatTimer)
        m_heartbeatTimer->stop();
    // Revoke the website session first — it is what the browser and the
    // account-scoped worker APIs see, so leaving it alive would keep this
    // machine "signed in" on the site after a desktop logout.
    revokeAccountSession();
    if (m_officeChannelMirror)
        m_officeChannelMirror->stop();
    m_accountSessionCache.clear();
    setDesktopCapability(m_accountName, false);
    m_accountAuthenticated = false;
    m_accountTier = QStringLiteral("free");
    m_accountSolanaVerified = false;
    m_isAdmin = false;
    updateAdminCrownBadge();
    m_seenPendingUsers.clear();
    // Drop the cached catalog: the next account pulls its own on first open.
    if (m_organizationTaskStore)
        m_organizationTaskStore->reset();
    m_adminPendingPolled = false;
    m_accountName.clear();
    m_accountEmail.clear();
    m_nodeOwnerUser.clear();
    m_profileIsUserAccount = false;
    m_profileLinkedNodes.clear();
    forgetOwnUserStatus();
    forgetUserActivitySync();
    QSettings().remove(kAuthedAccountSetting);
    QSettings().remove(kAccountNameSetting);
    refreshSettingsEmailVerifiedBadge();
    leaveSession();
    updateUserSwitcher();
    promptRelogin(previousAccount);
}

void MainWindow::revokeAccountSession()
{
    QString token = m_accountSessionToken.trimmed();
    const QString instanceUrl = m_accountSessionInstanceUrl;
    m_accountSessionToken.clear();
    m_accountSessionInstanceUrl.clear();
    m_accountSessionCache.removeToken(token);
    revokeAccountSessionToken(token, instanceUrl);
    token.fill(QChar(u'\0'));
    token.clear();
}

void MainWindow::revokeAccountSessionToken(const QString &token,
                                           const QString &serverUrl)
{
    if (token.isEmpty() || !m_networkAccess)
        return;
    if (serverUrl.trimmed().isEmpty()) {
        const QUrl currentLogoutUrl =
            accountsApiUrl(QStringLiteral("logout"));
        if (accountSessionTokenForUrl(currentLogoutUrl) != token)
            return;
        int status = 0;
        postAccountSync(QStringLiteral("logout"),
                        QJsonObject{{QStringLiteral("sessionToken"), token}},
                        &status);
        return;
    }
    const QString instanceUrl = serverUrl.trimmed().isEmpty()
                                    ? m_accountSessionInstanceUrl
                                    : serverUrl;
    const QUrl url = accountApiUrlForInstance(
        instanceUrl, QStringLiteral("logout"));
    const forkmesh::accounts::CachedSession bound{
        m_accountName, instanceUrl, token,
        QString(), false};
    if (!forkmesh::accounts::SessionCache::maySendTo(bound, url))
        return;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("sessionToken"), token}})
            .toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
}

namespace {

bool uninstallMayErase(const QString &path)
{
    namespace sd = forkmesh::safedelete;
    const QString clean = sd::detail::clean(path);
    if (clean.isEmpty() || clean == QLatin1String("/"))
        return false;
    for (const QString &home : sd::detail::homes()) {
        if (sd::detail::within(home, clean))
            return false;
    }
    if (!sd::detail::gitEntry(clean).isEmpty())
        return false;
    for (const auto &root : sd::detail::protectedRoots()) {
        const bool container = root.second == sd::RootKind::Projects ||
                               root.second == sd::RootKind::Project ||
                               root.second == sd::RootKind::Frozen ||
                               root.first.endsWith(QStringLiteral("/src"));
        if (sd::detail::inside(root.first, clean) || (container && root.first == clean))
            return false;
    }
    return true;
}

bool uninstallHoldsCheckout(const QString &path)
{
    namespace sd = forkmesh::safedelete;
    const QString clean = sd::detail::clean(path);
    const QStringList children =
        QDir(clean).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &child : children) {
        if (!sd::detail::gitEntry(clean + QLatin1Char('/') + child).isEmpty())
            return true;
    }
    return false;
}

QString normalizedAccountInstanceUrl(QString input)
{
    QUrl url(canonicalServerUrl(input));
    if (url.scheme() == QLatin1String("http"))
        url.setScheme(QStringLiteral("ws"));
    else if (url.scheme() == QLatin1String("https"))
        url.setScheme(QStringLiteral("wss"));
    if (url.path().isEmpty() || url.path() == QLatin1String("/"))
        url.setPath(kMainnodeRoomPath);
    url.setQuery(QString());
    url.setFragment(QString());
    return url.adjusted(QUrl::NormalizePathSegments)
        .toString(QUrl::FullyEncoded);
}

QString accountInstanceUrl(QString input)
{
    input = normalizedAccountInstanceUrl(input);
    QString roomName = kDefaultRoomName;
    forkmesh::mainnode::migrateSavedDefaultRoom(&input, &roomName);
    return input;
}

QJsonObject recentUserInstances()
{
    return QJsonDocument::fromJson(
               QSettings().value(kRecentUserInstancesSetting).toByteArray())
        .object();
}

struct RecentUserIdentity {
    QString account;
    QString instanceId;
    QString instanceUrl;
    qint64 lastUsedMs = 0;
};

void saveRecentUserIdentities(const QList<RecentUserIdentity> &identities)
{
    QJsonArray saved;
    for (const RecentUserIdentity &identity : identities) {
        if (!isValidNodeName(identity.account) || identity.instanceUrl.isEmpty())
            continue;
        saved.append(QJsonObject{
            {QStringLiteral("account"), identity.account},
            {QStringLiteral("instanceId"), identity.instanceId},
            {QStringLiteral("instanceUrl"), identity.instanceUrl},
            {QStringLiteral("lastUsedMs"),
             static_cast<double>(identity.lastUsedMs)},
        });
    }
    QSettings().setValue(
        kRecentUserIdentitiesSetting,
        QJsonDocument(saved).toJson(QJsonDocument::Compact));
}

QList<RecentUserIdentity> recentUserIdentities()
{
    QList<RecentUserIdentity> identities;
    const QJsonArray stored = QJsonDocument::fromJson(
                                  QSettings()
                                      .value(kRecentUserIdentitiesSetting)
                                      .toByteArray())
                                  .array();
    for (const QJsonValue &value : stored) {
        const QJsonObject object = value.toObject();
        const QString account = AccountCapability::normalizedAccount(
            object.value(QStringLiteral("account")).toString());
        const QString instanceId =
            object.value(QStringLiteral("instanceId")).toString().trimmed();
        const QString instanceUrl = accountInstanceUrl(
            object.value(QStringLiteral("instanceUrl")).toString());
        if (!isValidNodeName(account) || instanceUrl.isEmpty())
            continue;
        const auto duplicate = std::find_if(
            identities.cbegin(), identities.cend(),
            [&account, &instanceId,
             &instanceUrl](const RecentUserIdentity &candidate) {
                const bool sameInstance =
                    !instanceId.isEmpty() && !candidate.instanceId.isEmpty()
                        ? candidate.instanceId == instanceId
                        : candidate.instanceUrl == instanceUrl;
                return candidate.account == account && sameInstance;
            });
        if (duplicate == identities.cend()) {
            identities.append({account, instanceId, instanceUrl,
                               object.value(QStringLiteral("lastUsedMs"))
                                   .toVariant()
                                   .toLongLong()});
        }
    }
    if (!identities.isEmpty())
        return identities;

    // One-time migration: the old object could retain only one relay per
    // username. Preserve every row it can represent, then use the pair array
    // exclusively so future same-name accounts do not overwrite each other.
    const QJsonObject legacy = recentUserInstances();
    qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    for (auto it = legacy.constBegin(); it != legacy.constEnd(); ++it) {
        const QString account = AccountCapability::normalizedAccount(it.key());
        const QString instanceUrl = accountInstanceUrl(it.value().toString());
        if (isValidNodeName(account) && !instanceUrl.isEmpty())
            identities.append({account, QString(), instanceUrl, stamp--});
    }
    saveRecentUserIdentities(identities);
    return identities;
}

QString recentUserInstance(const QString &accountName)
{
    const QString account =
        AccountCapability::normalizedAccount(accountName);
    QJsonObject instances = recentUserInstances();
    const QString stored = instances.value(account).toString();
    if (stored.isEmpty()) {
        for (const RecentUserIdentity &identity : recentUserIdentities()) {
            if (identity.account == account)
                return identity.instanceUrl;
        }
        return {};
    }
    const QString migrated = accountInstanceUrl(stored);
    if (migrated != stored) {
        instances.insert(account, migrated);
        QSettings().setValue(
            kRecentUserInstancesSetting,
            QJsonDocument(instances).toJson(QJsonDocument::Compact));
    }
    QList<RecentUserIdentity> identities = recentUserIdentities();
    const auto found = std::find_if(
        identities.cbegin(), identities.cend(),
        [&account, &migrated](const RecentUserIdentity &candidate) {
            return candidate.account == account &&
                   candidate.instanceUrl == migrated;
        });
    if (found == identities.cend()) {
        identities.prepend(
            {account, QString(), migrated,
             QDateTime::currentMSecsSinceEpoch()});
        saveRecentUserIdentities(identities);
    }
    return migrated;
}

void saveRecentUserInstance(const QString &accountName,
                            const QString &serverUrl,
                            const QString &instanceId = QString())
{
    const QString account =
        AccountCapability::normalizedAccount(accountName);
    if (!isValidNodeName(account) || serverUrl.trimmed().isEmpty())
        return;
    const QString instanceUrl = accountInstanceUrl(serverUrl);
    QJsonObject instances = recentUserInstances();
    if (instances.value(account).toString() != instanceUrl) {
        instances.insert(account, instanceUrl);
        QSettings().setValue(
            kRecentUserInstancesSetting,
            QJsonDocument(instances).toJson(QJsonDocument::Compact));
    }

    QList<RecentUserIdentity> identities = recentUserIdentities();
    if (!identities.isEmpty() && identities.first().account == account &&
        identities.first().instanceUrl == instanceUrl &&
        identities.first().instanceId == instanceId.trimmed())
        return;
    identities.removeIf(
        [&account, &instanceId,
         &instanceUrl](const RecentUserIdentity &candidate) {
            const bool sameInstance =
                !instanceId.isEmpty() && !candidate.instanceId.isEmpty()
                    ? candidate.instanceId == instanceId
                    : candidate.instanceUrl == instanceUrl;
            return candidate.account == account && sameInstance;
        });
    identities.prepend(
        {account, instanceId.trimmed(), instanceUrl,
         QDateTime::currentMSecsSinceEpoch()});
    saveRecentUserIdentities(identities);
}

} // namespace

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testRecentUserInstance(const QString &accountName) const
{
    return recentUserInstance(accountName);
}

int MainWindow::testRecentUserIdentityCount() const
{
    return recentUserIdentities().size();
}

int MainWindow::testRecentUserIdentityCount(const QString &accountName) const
{
    const QString account =
        AccountCapability::normalizedAccount(accountName);
    const QList<RecentUserIdentity> identities = recentUserIdentities();
    return static_cast<int>(std::count_if(
        identities.cbegin(), identities.cend(),
        [&account](const RecentUserIdentity &identity) {
            return identity.account == account;
        }));
}

void MainWindow::testRememberUserIdentity(const QString &accountName,
                                          const QString &instanceUrl)
{
    rememberUserAccount(accountName, instanceUrl);
}

void MainWindow::testCacheAccountSession(
    const QString &accountName, const QString &instanceUrl,
    const QString &token, bool desktopCapable,
    const QString &desktopPublicKey)
{
    m_accountSessionCache.store({accountName, instanceUrl, token,
                                 desktopPublicKey, desktopCapable});
}

QString MainWindow::testCachedAccountSession(
    const QString &accountName, const QString &instanceUrl) const
{
    forkmesh::accounts::CachedSession session;
    if (!m_accountSessionCache.lookup(accountName, instanceUrl, &session))
        return {};
    return session.token;
}

bool MainWindow::testCachedAccountSessionMaySendTo(
    const QString &accountName, const QString &instanceUrl,
    const QUrl &target) const
{
    forkmesh::accounts::CachedSession session;
    return m_accountSessionCache.lookup(accountName, instanceUrl, &session) &&
           forkmesh::accounts::SessionCache::maySendTo(session, target);
}

bool MainWindow::testCachedAccountSessionMayRestoreDesktop(
    const QString &accountName, const QString &instanceUrl,
    const QString &currentPublicKey,
    const QString &validatedAccountPublicKey) const
{
    forkmesh::accounts::CachedSession session;
    return m_accountSessionCache.lookup(accountName, instanceUrl, &session) &&
           forkmesh::accounts::SessionCache::mayRestoreDesktopCapability(
               session, currentPublicKey, validatedAccountPublicKey);
}

QString MainWindow::testAccountInstanceUrl(const QString &instanceUrl) const
{
    return accountInstanceUrl(instanceUrl);
}

QUrl MainWindow::testAccountApiUrlForInstance(
    const QString &instanceUrl, const QString &leaf) const
{
    return accountApiUrlForInstance(instanceUrl, leaf);
}

void MainWindow::testClearAccountSessionCache()
{
    m_accountSessionCache.clear();
}
#endif

QString MainWindow::currentAccountInstanceUrl() const
{
    const QString edited =
        m_serverUrlEdit ? m_serverUrlEdit->text().trimmed() : QString();
    QString serverUrl = edited;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size()) {
        const QString active = m_servers.at(m_activeServer).url;
        if (edited.isEmpty() ||
            (!edited.contains(QStringLiteral("://")) &&
             serverHost(edited) == serverHost(active))) {
            serverUrl = active;
        }
    }
    return accountInstanceUrl(serverUrl);
}

QUrl MainWindow::accountApiUrlForInstance(const QString &instanceUrl,
                                          const QString &leaf) const
{
    QUrl url(accountInstanceUrl(instanceUrl));
    if (url.scheme() == QLatin1String("ws"))
        url.setScheme(QStringLiteral("http"));
    else if (url.scheme() == QLatin1String("wss"))
        url.setScheme(QStringLiteral("https"));
    url.setPath(QStringLiteral("/api/accounts/") + leaf);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QString MainWindow::accountSessionTokenForUrl(const QUrl &url) const
{
    if (!hasActiveAccountSession() ||
        m_accountSessionToken.trimmed().isEmpty() ||
        m_accountSessionInstanceUrl.trimmed().isEmpty()) {
        return {};
    }
    forkmesh::accounts::CachedSession active;
    if (!m_accountSessionCache.lookup(
            m_accountName, m_accountSessionInstanceUrl, &active) ||
        active.token != m_accountSessionToken.trimmed()) {
        active.clearSensitive();
        return {};
    }
    const bool maySend =
        forkmesh::accounts::SessionCache::maySendTo(active, url);
    active.clearSensitive();
    return maySend ? m_accountSessionToken.trimmed() : QString();
}

QString MainWindow::activeAccountSessionToken() const
{
    return accountSessionTokenForUrl(catalogApiUrl());
}

void MainWindow::cacheActiveAccountSession(const QString &instanceUrl)
{
    const QString account =
        AccountCapability::normalizedAccount(m_accountName);
    const QString token = m_accountSessionToken.trimmed();
    const QString canonicalInstance = accountInstanceUrl(instanceUrl);
    if (!isValidNodeName(account) || token.isEmpty())
        return;
    if (!m_accountSessionInstanceUrl.trimmed().isEmpty() &&
        accountInstanceUrl(m_accountSessionInstanceUrl) != canonicalInstance) {
        return;
    }
    const bool desktopCapable =
        m_accountDesktopCapable && hasOwnerSigningCapability(account);
    if (m_accountSessionCache.store(
            {account, canonicalInstance, token,
             desktopCapable ? m_profileIdentity.publicKey().trimmed()
                            : QString(),
             desktopCapable})) {
        m_accountSessionInstanceUrl = canonicalInstance;
    }
}

bool MainWindow::restoreCachedAccountSession(const QString &accountName,
                                             const QString &instanceUrl)
{
    const QString account =
        AccountCapability::normalizedAccount(accountName);
    const QString canonicalInstance = accountInstanceUrl(instanceUrl);
    forkmesh::accounts::CachedSession session;
    if (!m_networkAccess ||
        !m_accountSessionCache.lookup(account, canonicalInstance, &session)) {
        return false;
    }

    const QUrl url = accountApiUrlForInstance(
        canonicalInstance, QStringLiteral("profile"));
    if (!forkmesh::accounts::SessionCache::maySendTo(session, url)) {
        m_accountSessionCache.remove(account, canonicalInstance, session.token);
        session.clearSensitive();
        return false;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(15000);
    request.setRawHeader(
        QByteArrayLiteral("Authorization"),
        QByteArrayLiteral("Bearer ") + session.token.toUtf8());
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(QJsonObject()).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QJsonObject payload =
        QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();

    const QString returnedAccount = AccountCapability::normalizedAccount(
        payload.value(QStringLiteral("nodeName")).toString());
    const QString returnedToken =
        payload.value(QStringLiteral("sessionToken")).toString().trimmed();
    const bool valid = networkError == QNetworkReply::NoError &&
                       status == 200 &&
                       payload.value(QStringLiteral("ok")).toBool() &&
                       payload.value(QStringLiteral("status"))
                               .toString(QStringLiteral("active")) ==
                           QLatin1String("active") &&
                       returnedAccount == account &&
                       returnedToken == session.token;
    if (!valid) {
        if (status == 401 || status == 403 ||
            payload.value(QStringLiteral("error")).toString() ==
                QLatin1String("invalid_session") ||
            (!returnedAccount.isEmpty() && returnedAccount != account)) {
            m_accountSessionCache.remove(account, canonicalInstance,
                                         session.token);
        }
        session.clearSensitive();
        return false;
    }

    const QString localKey = m_profileIdentity.publicKey().trimmed();
    const QString primaryKey =
        payload.value(QStringLiteral("pubkey")).toString();
    const QStringList accountDesktopKeys =
        session.desktopCapable && !localKey.isEmpty() &&
                primaryKey.trimmed() != localKey
            ? accountDesktopKeysForSession(session)
            : QStringList();
    const bool ownsDesktopKey =
        forkmesh::accounts::SessionCache::mayRestoreDesktopCapability(
            session, localKey, primaryKey, accountDesktopKeys);
    activateAccountSession(payload, account, ownsDesktopKey,
                           canonicalInstance);
    session.clearSensitive();
    return true;
}

QStringList MainWindow::accountDesktopKeysForSession(
    const forkmesh::accounts::CachedSession &session)
{
    QStringList keys;
    const QUrl url = accountApiUrlForInstance(session.instanceUrl,
                                              QStringLiteral("desktops"));
    if (!m_networkAccess || session.token.isEmpty() ||
        !forkmesh::accounts::SessionCache::maySendTo(session, url)) {
        return keys;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader(
        QByteArrayLiteral("Authorization"),
        QByteArrayLiteral("Bearer ") + session.token.toUtf8());
    QNetworkReply *reply = m_networkAccess->get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status == 200;
    const QJsonObject payload =
        QJsonDocument::fromJson(reply->read(256 * 1024)).object();
    reply->deleteLater();
    if (!ok || !payload.value(QStringLiteral("ok")).toBool() ||
        AccountCapability::normalizedAccount(
            payload.value(QStringLiteral("account")).toString()) !=
            AccountCapability::normalizedAccount(session.account)) {
        return keys;
    }
    for (const QJsonValue &value :
         payload.value(QStringLiteral("desktops")).toArray()) {
        const QString key =
            value.toObject().value(QStringLiteral("key")).toString().trimmed();
        if (!key.isEmpty() && !keys.contains(key))
            keys.append(key);
    }
    return keys;
}

void MainWindow::clearAccountSessionMemory()
{
    m_accountSessionCache.clear();
    m_accountSessionToken.fill(QChar(u'\0'));
    m_accountSessionToken.clear();
    m_accountSessionInstanceUrl.clear();
}

void MainWindow::isolateAccountForServerSwitch(const QString &accountName)
{
    // Keep other instances' sessions in the in-memory exact-origin cache, but
    // drop every active capability before selecting/restoring the destination.
    // Nothing account-scoped may remain logically active during the handoff.
    if (m_officeChannelMirror)
        m_officeChannelMirror->stop();
    m_accountSessionToken.fill(QChar(u'\0'));
    m_accountSessionToken.clear();
    m_accountSessionInstanceUrl.clear();
    m_accountAuthenticated = false;
    m_accountDesktopCapable = false;
    m_accountEmail.clear();
    m_accountTier = QStringLiteral("free");
    m_accountSolanaVerified = false;
    m_isAdmin = false;
    m_profileIsUserAccount = false;
    m_profileLinkedNodes.clear();
    m_nodeOwnerUser.clear();
    m_seenPendingUsers.clear();
    if (m_organizationTaskStore)
        m_organizationTaskStore->reset();
    m_adminPendingPolled = false;
    forgetOwnUserStatus();
    forgetUserActivitySync();
    QSettings().remove(kAuthedAccountSetting);
    updateAdminCrownBadge();

    QString account = AccountCapability::normalizedAccount(accountName);
    if (!isValidNodeName(account))
        account = AccountCapability::normalizedAccount(machineNodeName());
    if (!isValidNodeName(account))
        account = QStringLiteral("forkmesh-node");
    m_accountName = account;
    m_userName = account;
    QSettings().setValue(kAccountNameSetting, account);
    if (m_nameEdit)
        m_nameEdit->setText(account);
    if (m_settingsNameEdit)
        m_settingsNameEdit->setText(account);
    updateUserSwitcher();
    refreshSettingsEmailVerifiedBadge();
}

void MainWindow::forgetServerInstance(const ServerConfig &server)
{
    const QString instanceUrl = accountInstanceUrl(server.url);
    m_accountSessionCache.removeInstance(instanceUrl);

    QList<RecentUserIdentity> identities = recentUserIdentities();
    identities.removeIf(
        [&server, &instanceUrl](const RecentUserIdentity &identity) {
            return (!server.id.trimmed().isEmpty() &&
                    identity.instanceId == server.id.trimmed()) ||
                   identity.instanceUrl == instanceUrl;
        });
    saveRecentUserIdentities(identities);

    QSettings settings;
    if (!server.id.trimmed().isEmpty()) {
        settings.remove(QStringLiteral("chat/privateChannelsByInstance/") +
                        server.id.trimmed());
    }
    QJsonObject legacy = recentUserInstances();
    for (auto it = legacy.begin(); it != legacy.end();) {
        if (accountInstanceUrl(it.value().toString()) == instanceUrl)
            it = legacy.erase(it);
        else
            ++it;
    }
    settings.setValue(
        kRecentUserInstancesSetting,
        QJsonDocument(legacy).toJson(QJsonDocument::Compact));
}

QStringList MainWindow::recentUserAccounts() const
{
    QStringList accounts;
    for (const RecentUserIdentity &identity : recentUserIdentities()) {
        const QString account = identity.account;
        if (isValidNodeName(account) && !accounts.contains(account))
            accounts.append(account);
    }
    const QString current =
        AccountCapability::normalizedAccount(m_accountName);
    if (isValidNodeName(current)) {
        accounts.removeAll(current);
        accounts.prepend(current);
    }
    return accounts;
}

void MainWindow::rememberUserAccount(const QString &accountName,
                                     const QString &instanceUrl)
{
    const QString account =
        AccountCapability::normalizedAccount(accountName);
    if (!isValidNodeName(account))
        return;
    QStringList accounts = recentUserAccounts();
    accounts.removeAll(account);
    accounts.prepend(account);
    forkmesh::settings::put(QSettings(), kRecentUserAccountsSetting, accounts);
    const QString target = instanceUrl.trimmed().isEmpty()
                               ? currentAccountInstanceUrl()
                               : instanceUrl;
    QString instanceId;
    const QString canonicalTarget = accountInstanceUrl(target);
    for (const ServerConfig &server : std::as_const(m_servers)) {
        if (accountInstanceUrl(server.url) == canonicalTarget) {
            instanceId = server.id;
            break;
        }
    }
    saveRecentUserInstance(account, target, instanceId);
}

bool MainWindow::switchToUserAccount(const QString &accountName,
                                     const QString &serverUrl)
{
    const QString account =
        AccountCapability::normalizedAccount(accountName);
    if (!isValidNodeName(account)) {
        QMessageBox::warning(
            this, QStringLiteral("Switch account"),
            QStringLiteral("Choose a valid ForkMesh username."));
        return false;
    }
    QString previousServer;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size())
        previousServer = m_servers.at(m_activeServer).url;
    else if (m_serverUrlEdit)
        previousServer = m_serverUrlEdit->text();
    previousServer = accountInstanceUrl(previousServer);
    QString targetServer = serverUrl.trimmed();
    if (targetServer.isEmpty())
        targetServer = recentUserInstance(account);
    if (targetServer.isEmpty())
        targetServer = previousServer;
    targetServer = accountInstanceUrl(targetServer);
    const QUrl parsedTarget(targetServer);
    if (!parsedTarget.isValid() || parsedTarget.host().isEmpty() ||
        (parsedTarget.scheme() != QLatin1String("ws") &&
         parsedTarget.scheme() != QLatin1String("wss")) ||
        !parsedTarget.userName().isEmpty() ||
        !parsedTarget.password().isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Switch account"),
            QStringLiteral("Choose a valid ws:// or wss:// ForkMesh instance."));
        return false;
    }
    if (std::none_of(m_servers.cbegin(), m_servers.cend(),
                     [&targetServer](const ServerConfig &server) {
                         return accountInstanceUrl(server.url) == targetServer;
                     }) &&
        refuseInstanceOfAnotherProject(targetServer))
        return false;

    if (hasActiveAccountSession() &&
        AccountCapability::normalizedAccount(m_accountName) == account &&
        targetServer == previousServer) {
        return true;
    }

    const QString previousAccount = m_accountName;
    const bool previousWasUserSession =
        hasActiveAccountSession() &&
        (!m_accountSessionToken.trimmed().isEmpty() || m_profileIsUserAccount);
    if (previousWasUserSession) {
        rememberUserAccount(previousAccount);
        cacheActiveAccountSession(previousServer);
    }

    const QString previousServerEdit =
        m_serverUrlEdit ? m_serverUrlEdit->text() : QString();
    if (m_serverUrlEdit)
        m_serverUrlEdit->setText(targetServer);
    const bool restored = restoreCachedAccountSession(account, targetServer);
    if (!restored && !runLoginFlow(account)) {
        if (m_serverUrlEdit)
            m_serverUrlEdit->setText(previousServerEdit);
        return false;
    }
    const QUrl targetProfileUrl = accountApiUrlForInstance(
        targetServer, QStringLiteral("profile"));
    if (AccountCapability::normalizedAccount(m_accountName) != account ||
        accountInstanceUrl(m_accountSessionInstanceUrl) != targetServer ||
        accountSessionTokenForUrl(targetProfileUrl).isEmpty()) {
        if (m_serverUrlEdit)
            m_serverUrlEdit->setText(previousServerEdit);
        QMessageBox::warning(
            this, QStringLiteral("Switch account"),
            QStringLiteral("The selected instance did not return an active "
                           "session for @%1. Your current account and instance "
                           "were left unchanged.")
                .arg(account));
        return false;
    }
    if (m_serverUrlEdit)
        m_serverUrlEdit->setText(previousServerEdit);

    int targetServerIndex = -1;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (accountInstanceUrl(m_servers.at(i).url) == targetServer) {
            targetServerIndex = i;
            break;
        }
    }
    if (targetServerIndex < 0) {
        ServerConfig instance;
        instance.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        instance.url = targetServer;
        instance.name = serverHost(targetServer);
        instance.room = kDefaultRoomName;
        instance.lastAccount = account;
        m_servers.append(instance);
        m_legacyServerUrls.append(targetServer);
        m_legacyServerRooms.append(kDefaultRoomName);
        targetServerIndex = m_servers.size() - 1;
    }
    m_servers[targetServerIndex].lastAccount = account;
    QSettings accountSettings;
    accountSettings.setValue(kAccountNameSetting, m_accountName);
    m_userName = m_accountName;
    if (m_settingsNameEdit)
        m_settingsNameEdit->setText(m_accountName);
    if (m_nameEdit)
        m_nameEdit->setText(m_accountName);
    if (targetServerIndex != m_activeServer || !m_backend)
        switchToServer(targetServerIndex);

    rememberUserAccount(m_accountName, targetServer);
    saveServers();
    if (m_settingsMachineNodeEdit)
        m_settingsMachineNodeEdit->setText(machineNodeName());
    refreshSettingsEmailVerifiedBadge();
    updateUserSwitcher();
    updateChatIdentity();
    refreshRepositoryList();
    QMessageBox::information(
        this, QStringLiteral("Account switched"),
        hasOwnerSigningCapability()
            ? QStringLiteral("Signed in as %1 on %2. This desktop is one of "
                             "the account's desktops, so owner-signed actions "
                             "work here too.")
                  .arg(m_accountName, serverHost(targetServer))
            : QStringLiteral("Signed in as %1 on %2. Owner-signed actions "
                             "stay off on this desktop until you sign in to "
                             "the account again from here, which adds this "
                             "desktop's key to the account.")
                  .arg(m_accountName, serverHost(targetServer)));
    return true;
}

void MainWindow::openAccountManagement()
{
    showSection(1);
    if (m_settingsSections && m_profileSettingsTabIndex >= 0)
        m_settingsSections->setCurrentIndex(m_profileSettingsTabIndex);
}

void MainWindow::showUserAccountMenu()
{
    if (!m_accountSwitcherButton)
        return;
    QMenu menu(this);
    const QString current =
        AccountCapability::normalizedAccount(m_accountName);
    QString activeInstanceName;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size()) {
        activeInstanceName = m_servers.at(m_activeServer).name.trimmed();
        if (activeInstanceName.isEmpty())
            activeInstanceName = serverHost(m_servers.at(m_activeServer).url);
    }
    QAction *identity = menu.addAction(
        current.isEmpty() ? QStringLiteral("No account session")
        : activeInstanceName.isEmpty()
            ? QStringLiteral("Signed in as @%1").arg(current)
            : QStringLiteral("Signed in as @%1 on %2")
                  .arg(current, activeInstanceName));
    identity->setEnabled(false);
    // Away and the custom status need the account session the server
    // authenticates POST /api/accounts/user-status with.
    QAction *awayAction = nullptr;
    QAction *statusAction = nullptr;
    QAction *clearStatusAction = nullptr;
    if (!accountSessionTokenForUrl(accountsApiUrl(QStringLiteral("user-status")))
             .isEmpty()) {
        using namespace forkmesh::userstatus;
        const UserStatus own =
            m_ownUserStatusKnown
                ? visible(m_ownUserStatus, QDateTime::currentMSecsSinceEpoch())
                : UserStatus();
        menu.addSeparator();
        awayAction = menu.addAction(own.away
                                        ? QStringLiteral("Set yourself active")
                                        : QStringLiteral("Set yourself away"));
        const QString current = summary(own);
        statusAction = menu.addAction(
            current.isEmpty()
                ? QStringLiteral("Set a status…")
                : QStringLiteral("%1 · Edit status…").arg(current));
        if (own.hasCustom())
            clearStatusAction = menu.addAction(QStringLiteral("Clear status"));
    }
    menu.addSeparator();
    QAction *manage = menu.addAction(
        activeInstanceName.isEmpty()
            ? QStringLiteral("Manage account")
            : QStringLiteral("Manage account on %1").arg(activeInstanceName));
    QAction *instance =
        menu.addAction(QStringLiteral("Manage relay instances"));
    QAction *mirrors =
        menu.addAction(QStringLiteral("Manage mirror servers"));
    QAction *membership =
        menu.addAction(QStringLiteral("Become a member…"));

    QHash<QAction *, QPair<QString, QString>> accountActions;
    const QList<RecentUserIdentity> recent = recentUserIdentities();
    const QString currentInstance = currentAccountInstanceUrl();
    if (!recent.isEmpty()) {
        menu.addSection(QStringLiteral("Switch user"));
        for (const RecentUserIdentity &identity : recent) {
            if (identity.account == current &&
                identity.instanceUrl == currentInstance)
                continue;
            const QString instanceHost =
                serverHost(identity.instanceUrl);
            QAction *action = menu.addAction(
                instanceHost.isEmpty()
                    ? QStringLiteral("Switch to @%1…").arg(identity.account)
                    : QStringLiteral("Switch to @%1 on %2…")
                          .arg(identity.account, instanceHost));
            accountActions.insert(
                action, qMakePair(identity.account, identity.instanceUrl));
        }
    }
    menu.addSeparator();
    QAction *another =
        menu.addAction(QStringLiteral("Sign in on another instance…"));
    QAction *logoutAction = menu.addAction(QStringLiteral("Log out"));

    QAction *settingsAction = nullptr;
    QAction *offlineAction = nullptr;
    if (Theme::isNext()) {
        QAction *first = menu.actions().value(1);
        settingsAction = new QAction(QStringLiteral("Settings"), &menu);
        offlineAction = new QAction(QStringLiteral("Offline mode"), &menu);
        offlineAction->setCheckable(true);
        offlineAction->setChecked(m_offlineMode);
        auto *version = new QAction(
            QStringLiteral("ForkMesh %1").arg(QCoreApplication::applicationVersion()),
            &menu);
        version->setEnabled(false);
        menu.insertSeparator(first);
        menu.insertAction(first, settingsAction);
        menu.insertAction(first, offlineAction);
        menu.insertAction(first, version);
    }
    QPoint anchor = topBarMenuAnchor(m_accountSwitcherButton);
    if (property("accountMenuAt").isValid())
        anchor = property("accountMenuAt").toPoint();
    if (property("accountMenuAbove").isValid())
        anchor = property("accountMenuAbove").toPoint() -
                 QPoint(0, menu.sizeHint().height());
    setProperty("accountMenuAt", QVariant());
    setProperty("accountMenuAbove", QVariant());
    QAction *chosen = menu.exec(anchor);
    if (!chosen)
        return;
    if (settingsAction && chosen == settingsAction) {
        if (m_settingsNavButton)
            m_settingsNavButton->click();
    } else if (offlineAction && chosen == offlineAction) {
        setOfflineMode(!m_offlineMode);
    } else if (awayAction && chosen == awayAction) {
        const bool away = !(m_ownUserStatusKnown && m_ownUserStatus.away);
        postOwnUserStatus(
            QJsonObject{{QStringLiteral("away"), away}},
            [this, away](bool ok, const QString &error) {
                flashMessage(ok ? (away ? QStringLiteral("You're set to away.")
                                        : QStringLiteral("You're set to active."))
                                : error,
                             !ok);
            });
    } else if (statusAction && chosen == statusAction) {
        showSetUserStatusDialog();
    } else if (clearStatusAction && chosen == clearStatusAction) {
        postOwnUserStatus(
            QJsonObject{{QStringLiteral("clear"), true}},
            [this](bool ok, const QString &error) {
                flashMessage(ok ? QStringLiteral("Status cleared.") : error, !ok);
            });
    } else if (chosen == manage) {
        openAccountManagement();
    } else if (chosen == instance) {
        showNodesForRole(QStringLiteral("relay"));
    } else if (chosen == mirrors) {
        showNetworkTab(kNetworkHostsTab);
    } else if (chosen == membership) {
        openMembershipOptions();
    } else if (chosen == another) {
        loginToUserAccount();
    } else if (chosen == logoutAction) {
        logout();
    } else if (accountActions.contains(chosen)) {
        const auto identity = accountActions.value(chosen);
        switchToUserAccount(identity.first, identity.second);
    }
}

void MainWindow::fetchOwnUserStatus()
{
    // An empty body is a read of the caller's own status.
    if (accountSessionTokenForUrl(accountsApiUrl(QStringLiteral("user-status")))
            .isEmpty())
        return;
    postOwnUserStatus(QJsonObject());
}

void MainWindow::postOwnUserStatus(
    const QJsonObject &body,
    const std::function<void(bool ok, const QString &error)> &done)
{
    const QUrl url = accountsApiUrl(QStringLiteral("user-status"));
    const QString token = accountSessionTokenForUrl(url);
    if (!m_networkAccess || token.isEmpty()) {
        if (done)
            done(false, QStringLiteral("Sign in to a ForkMesh account to set "
                                       "your status."));
        return;
    }
    QJsonObject payload = body;
    payload.insert(QStringLiteral("sessionToken"), token);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const bool write = !body.isEmpty();
    const QString account = AccountCapability::normalizedAccount(m_accountName);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, done, write, account] {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QJsonObject resp =
                    QJsonDocument::fromJson(reply->readAll()).object();
                reply->deleteLater();
                bool ok = status >= 200 && status < 300 &&
                          resp.value(QStringLiteral("ok")).toBool();
                // The account switched while this was in flight: the answer
                // describes somebody else's status now.
                const bool sameAccount =
                    AccountCapability::normalizedAccount(m_accountName) ==
                        account &&
                    AccountCapability::normalizedAccount(
                        resp.value(QStringLiteral("nodeName")).toString()) ==
                        account;
                forkmesh::userstatus::UserStatus answer;
                if (ok && sameAccount &&
                    forkmesh::userstatus::readFrom(resp, &answer)) {
                    if (write)
                        m_ownUserStatusWrittenMs =
                            QDateTime::currentMSecsSinceEpoch();
                    applyOwnUserStatus(answer);
                } else if (ok && !sameAccount) {
                    ok = false;
                }
                if (done)
                    done(ok, ok ? QString()
                                : forkmesh::userstatus::saveErrorText(
                                      resp.value(QStringLiteral("error"))
                                          .toString(),
                                      status));
            });
}

void MainWindow::applyOwnUserStatus(
    const forkmesh::userstatus::UserStatus &status)
{
    const bool changed = !m_ownUserStatusKnown || status != m_ownUserStatus;
    m_ownUserStatus = status;
    m_ownUserStatusKnown = true;
    // Peers repaint from our next presence frame, which the backend sends
    // at once.
    if (m_backend)
        m_backend->setUserStatus(forkmesh::userstatus::toJson(status));
    if (changed)
        refreshUserStatusPresentation();
}

void MainWindow::forgetOwnUserStatus()
{
    if (!m_ownUserStatusKnown)
        return;
    m_ownUserStatus = forkmesh::userstatus::UserStatus();
    m_ownUserStatusKnown = false;
    m_ownUserStatusWrittenMs = 0;
    if (m_backend)
        m_backend->setUserStatus(QJsonObject());
    if (m_userStatusDialog)
        m_userStatusDialog->close();
    refreshUserStatusPresentation();
}

void MainWindow::showSetUserStatusDialog()
{
    using namespace forkmesh::userstatus;
    if (m_userStatusDialog) {
        m_userStatusDialog->raise();
        m_userStatusDialog->activateWindow();
        return;
    }
    if (accountSessionTokenForUrl(accountsApiUrl(QStringLiteral("user-status")))
            .isEmpty()) {
        flashMessage(QStringLiteral(
                         "Sign in to a ForkMesh account to set your status."),
                     true);
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName("setUserStatusDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Set a status"));
    dialog->setMinimumWidth(460);
    m_userStatusDialog = dialog;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const UserStatus current =
        m_ownUserStatusKnown ? visible(m_ownUserStatus, nowMs) : UserStatus();

    auto *intro = new QLabel(QStringLiteral(
        "Your status shows beside your name in chat, direct messages and your "
        "profile, on every device."));
    intro->setObjectName("modeHint");
    intro->setWordWrap(true);

    // The emoji: a free field that takes exactly one, with a row of quick
    // picks that fill it.
    auto *emojiEdit = new QLineEdit;
    emojiEdit->setObjectName("userStatusEmojiEdit");
    emojiEdit->setMaxLength(32);
    emojiEdit->setFixedWidth(64);
    emojiEdit->setAlignment(Qt::AlignCenter);
    emojiEdit->setPlaceholderText(QStringLiteral("🙂"));
    emojiEdit->setToolTip(QStringLiteral("One emoji"));
    emojiEdit->setText(current.emoji);
    auto *emojiRow = new QHBoxLayout;
    emojiRow->setContentsMargins(0, 0, 0, 0);
    emojiRow->setSpacing(2);
    emojiRow->addWidget(emojiEdit);
    emojiRow->addSpacing(6);
    for (const QString &emoji : quickEmoji()) {
        auto *pick = new QToolButton;
        pick->setText(emoji);
        pick->setAutoRaise(true);
        pick->setCursor(Qt::PointingHandCursor);
        pick->setToolTip(QStringLiteral("Use %1").arg(emoji));
        connect(pick, &QToolButton::clicked, emojiEdit,
                [emojiEdit, emoji] { emojiEdit->setText(emoji); });
        emojiRow->addWidget(pick);
    }
    emojiRow->addStretch(1);

    auto *textEdit = new QLineEdit;
    textEdit->setObjectName("userStatusTextEdit");
    textEdit->setMaxLength(kTextMax);
    textEdit->setPlaceholderText(QStringLiteral("What's your status?"));
    textEdit->setText(current.text);

    auto *clearCombo = new QComboBox;
    clearCombo->setObjectName("userStatusClearAfterCombo");
    for (const ClearAfter choice : clearAfterChoices())
        clearCombo->addItem(clearAfterLabel(choice), int(choice));
    auto *customEdit = new QDateTimeEdit;
    customEdit->setObjectName("userStatusCustomClearEdit");
    customEdit->setCalendarPopup(true);
    customEdit->setDisplayFormat(QStringLiteral("ddd MMM d, yyyy h:mm AP"));
    const QDateTime now = QDateTime::currentDateTime();
    customEdit->setMinimumDateTime(now);
    customEdit->setMaximumDateTime(now.addMSecs(kMaxDurationMs));
    const auto selectClearAfter = [clearCombo](ClearAfter choice) {
        const int index = clearCombo->findData(int(choice));
        if (index >= 0)
            clearCombo->setCurrentIndex(index);
    };
    if (current.expiresAtMs > 0) {
        customEdit->setDateTime(QDateTime::fromMSecsSinceEpoch(current.expiresAtMs));
        selectClearAfter(ClearAfter::Custom);
    } else {
        customEdit->setDateTime(now.addSecs(60 * 60));
        selectClearAfter(ClearAfter::Never);
    }
    const auto chosenClearAfter = [clearCombo] {
        return ClearAfter(clearCombo->currentData().toInt());
    };
    customEdit->setVisible(chosenClearAfter() == ClearAfter::Custom);
    connect(clearCombo, &QComboBox::currentIndexChanged, customEdit,
            [customEdit, chosenClearAfter] {
                customEdit->setVisible(chosenClearAfter() == ClearAfter::Custom);
            });
    auto *clearRow = new QHBoxLayout;
    clearRow->setContentsMargins(0, 0, 0, 0);
    clearRow->addWidget(clearCombo);
    clearRow->addWidget(customEdit, 1);

    auto *fields = new QWidget;
    auto *form = new QFormLayout(fields);
    form->setContentsMargins(0, 0, 0, 0);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setSpacing(8);
    form->addRow(QStringLiteral("Emoji"), emojiRow);
    form->addRow(QStringLiteral("Status"), textEdit);
    form->addRow(QStringLiteral("Clear after"), clearRow);

    // Suggestions: one click fills the emoji, the text and the clear time.
    auto *suggestionsLabel = new QLabel(QStringLiteral("Suggestions"));
    suggestionsLabel->setObjectName("modeHint");
    auto *suggestions = new QVBoxLayout;
    suggestions->setContentsMargins(0, 0, 0, 0);
    suggestions->setSpacing(2);
    suggestions->addWidget(suggestionsLabel);
    for (const Preset &preset : presets()) {
        auto *button = new QPushButton(presetLabel(preset));
        button->setObjectName("ghostButton");
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(Theme::remapSheet(QStringLiteral("text-align:left;")));
        connect(button, &QPushButton::clicked, dialog,
                [emojiEdit, textEdit, selectClearAfter, preset] {
                    emojiEdit->setText(preset.emoji);
                    textEdit->setText(preset.text);
                    selectClearAfter(preset.clearAfter);
                });
        suggestions->addWidget(button);
    }

    auto *hint = new QLabel;
    hint->setObjectName("userStatusHint");
    hint->setWordWrap(true);
    const auto setHint = [hint](const QString &text, bool error) {
        hint->setText(text);
        hint->setStyleSheet(Theme::remapSheet(error ? QStringLiteral("color:#f85149;")
                                  : QString()));
    };

    auto *clearButton = new QPushButton(QStringLiteral("Clear status"));
    clearButton->setObjectName("ghostButton");
    clearButton->setCursor(Qt::PointingHandCursor);
    clearButton->setEnabled(current.hasCustom());
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"));
    cancelButton->setObjectName("ghostButton");
    cancelButton->setCursor(Qt::PointingHandCursor);
    connect(cancelButton, &QPushButton::clicked, dialog, &QDialog::reject);
    auto *saveButton = new QPushButton(QStringLiteral("Save"));
    saveButton->setObjectName("primaryButton");
    saveButton->setCursor(Qt::PointingHandCursor);
    saveButton->setDefault(true);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(clearButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(saveButton);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    layout->addWidget(intro);
    layout->addWidget(fields);
    layout->addLayout(suggestions);
    layout->addWidget(hint);
    layout->addStretch(1);
    layout->addLayout(buttonRow);

    QPointer<QDialog> dialogGuard(dialog);
    // Both buttons post and wait; the dialog closes on success and shows the
    // server's reason otherwise.
    const auto submit = [this, dialogGuard, saveButton, clearButton,
                         setHint](const QJsonObject &body) {
        saveButton->setEnabled(false);
        clearButton->setEnabled(false);
        setHint(QStringLiteral("Saving…"), false);
        postOwnUserStatus(body, [dialogGuard, saveButton, clearButton,
                                 setHint](bool ok, const QString &error) {
            if (!dialogGuard)
                return;
            if (ok) {
                dialogGuard->accept();
                return;
            }
            saveButton->setEnabled(true);
            clearButton->setEnabled(true);
            setHint(error, true);
        });
    };
    connect(clearButton, &QPushButton::clicked, dialog, [submit] {
        submit(QJsonObject{{QStringLiteral("clear"), true}});
    });
    connect(saveButton, &QPushButton::clicked, dialog,
            [emojiEdit, textEdit, customEdit, chosenClearAfter, setHint,
             submit] {
                const QString rawEmoji = emojiEdit->text().trimmed();
                const QString emoji = cleanEmoji(rawEmoji);
                if (!rawEmoji.isEmpty() && emoji.isEmpty()) {
                    emojiEdit->setFocus();
                    setHint(saveErrorText(QStringLiteral("bad_status_emoji"), 400),
                            true);
                    return;
                }
                const QString text = cleanText(textEdit->text());
                if (emoji.isEmpty() && text.isEmpty()) {
                    submit(QJsonObject{{QStringLiteral("clear"), true}});
                    return;
                }
                const QDateTime now = QDateTime::currentDateTime();
                const qint64 expiresAt = clearAfterExpiry(
                    chosenClearAfter(), now, customEdit->dateTime());
                if (expiresAt != 0 && expiresAt <= now.toMSecsSinceEpoch()) {
                    customEdit->setFocus();
                    setHint(saveErrorText(QStringLiteral("status_expiry_in_past"),
                                          400),
                            true);
                    return;
                }
                submit(QJsonObject{{QStringLiteral("emoji"), emoji},
                                   {QStringLiteral("text"), text},
                                   {QStringLiteral("expiresAt"),
                                    double(expiresAt)}});
            });

    dialog->show();
    textEdit->setFocus();
}

bool MainWindow::promptRelogin(const QString &previousAccount)
{
    QString accountName = AccountCapability::normalizedAccount(previousAccount);
    if (!isValidNodeName(accountName)) {
        bool ok = false;
        accountName = QInputDialog::getText(this, "Log back in",
                                            "ForkMesh username:",
                                            QLineEdit::Normal, QString(), &ok)
                          .trimmed()
                          .toLower();
        if (!ok || !isValidNodeName(accountName))
            return false;
    }
    if (!runLoginFlow(accountName))
        return false;

    rememberUserAccount(m_accountName);

    QSettings().setValue(kAccountNameSetting, m_accountName);
    if (m_settingsNameEdit)
        m_settingsNameEdit->setText(m_accountName);
    if (m_settingsMachineNodeEdit)
        m_settingsMachineNodeEdit->setText(machineNodeName());
    refreshSettingsEmailVerifiedBadge();
    if (m_nameEdit)
        m_nameEdit->setText(m_accountName);
    startSession();
    return true;
}

void MainWindow::loginToUserAccount()
{
    const QString suggested = m_accountName.isEmpty()
                                  ? QSettings().value(kAccountNameSetting).toString()
                                  : m_accountName;
    QString suggestedServer = recentUserInstance(suggested);
    if (suggestedServer.isEmpty() && m_activeServer >= 0 &&
        m_activeServer < m_servers.size()) {
        suggestedServer = m_servers.at(m_activeServer).url;
    }

    QString accountInput;
    QString serverInput;
    if (Theme::isNext()) {
        if (!NextAccess::promptInstanceAccount(
                *this, suggested, serverHostDisplay(suggestedServer),
                &accountInput, &serverInput)) {
            return;
        }
    } else {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Sign in to a ForkMesh instance"));
        auto *form = new QFormLayout(&dialog);
        auto *intro = new QLabel(
            QStringLiteral("Use the same account you use on the ForkMesh website. "
                           "Your username is the name on your profile, not your email; "
                           "change it below if the suggested one is not yours. The "
                           "instance is the ForkMesh server that holds your account: "
                           "leave it as it is unless someone gave you a different one. "
                           "Continue only signs you in; it does not create anything."));
        intro->setWordWrap(true);
        form->addRow(intro);
        auto *accountEdit = new QLineEdit(suggested, &dialog);
        accountEdit->setPlaceholderText(QStringLiteral("username"));
        auto *serverEdit =
            new QLineEdit(serverHostDisplay(suggestedServer), &dialog);
        serverEdit->setPlaceholderText(
            serverHostDisplay(kDefaultServerUrl));
        form->addRow(QStringLiteral("ForkMesh username"), accountEdit);
        form->addRow(QStringLiteral("Instance or relay"), serverEdit);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Continue"));
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(460, dialog.sizeHint().height());
        if (dialog.exec() != QDialog::Accepted)
            return;
        accountInput = accountEdit->text();
        serverInput = serverEdit->text();
    }

    const QString accountName = accountInput.trimmed().toLower();
    if (!isValidNodeName(accountName)) {
        QMessageBox::warning(this, "Log in",
                             "Enter a valid username (lowercase letters, numbers "
                             "and hyphens; start with a letter).");
        return;
    }
    const QString instanceUrl = accountInstanceUrl(serverInput);
    const QUrl parsedInstance(instanceUrl);
    if (!parsedInstance.isValid() || parsedInstance.host().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Log in"),
                             QStringLiteral("Enter a valid ForkMesh instance host."));
        return;
    }
    switchToUserAccount(accountName, instanceUrl);
}

// code-health: allow(dir-walk-in-loop) confirmed uninstall; lists the children of the few ForkMesh folders that hold a checkout, and removes folders itself only when the detached rm fails, then exits the app
void MainWindow::uninstallForkMesh()
{
    const QString dataHome =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString configHome =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString cacheHome =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);

    QStringList dirs;
    QStringList files;
    QStringList kept;
    auto addFile = [&files](const QString &f) {
        const QFileInfo info(f);
        if (!f.isEmpty() && (info.exists() || info.isSymLink()) &&
            !files.contains(info.absoluteFilePath()))
            files << info.absoluteFilePath();
    };
    std::function<void(const QString &, bool)> addDir =
        [&](const QString &d, bool descend) {
            if (d.isEmpty())
                return;
            const QFileInfo info(d);
            if (info.isSymLink()) {
                if (uninstallMayErase(d))
                    addFile(d);
                return;
            }
            if (!info.isDir())
                return;
            const QString path = QDir(d).absolutePath();
            if (dirs.contains(path) || kept.contains(path))
                return;
            for (const QString &listed : std::as_const(dirs)) {
                if (forkmesh::safedelete::detail::inside(path, listed))
                    return;
            }
            if (!uninstallMayErase(path)) {
                kept << path;
                return;
            }
            if (!uninstallHoldsCheckout(path)) {
                dirs << path;
                return;
            }
            if (!descend) {
                kept << path;
                return;
            }
            const QFileInfoList entries = QDir(path).entryInfoList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            for (const QFileInfo &entry : entries) {
                if (entry.isDir() && !entry.isSymLink())
                    addDir(entry.absoluteFilePath(), true);
                else
                    addFile(entry.absoluteFilePath());
            }
        };
    addDir(forkmesh::paths::dataDir(), true);
    addDir(forkmesh::paths::dataRoot(), true);
    addDir(forkmesh::paths::localDataRoot(), true);
    addDir(forkmesh::paths::cacheRoot(), true);
    // Only a settings folder ForkMesh owns: never the registry key read as a
    // path, never the shared macOS ~/Library/Preferences (its plist is
    // removed as a file below).
    addDir(forkmesh::paths::settingsDir(), true);
    addDir(QDir::homePath() + QStringLiteral("/.forkmesh"), true);
    // The projects layout: this project's roots. The project folder itself is
    // left in place, since other projects and the user's own files may share it.
    addDir(forkmesh::paths::stateRoot(), true);
    addDir(forkmesh::paths::logsDir(), true);
    addDir(forkmesh::paths::sshDir(), true);
    if (forkmesh::paths::isActive() &&
        forkmesh::paths::configRoot().startsWith(forkmesh::paths::projectDir() +
                                                 QLatin1Char('/')))
        addDir(forkmesh::paths::configRoot(), true);
    addDir(repositoryMirrorRoot(), false);
    addDir(repositoryPreviewRoot(), false);
    addDir(backupRoot(), false);
    addDir(dataHome + QStringLiteral("/ForkMesh"), true);
    addDir(configHome + QStringLiteral("/ForkMesh"), true);
    addDir(cacheHome + QStringLiteral("/ForkMesh"), true);
    addDir(dataHome + QStringLiteral("/forkmesh"), true);

    addFile(autostartDesktopPath());
    if (forkmesh::paths::settingsDir().isEmpty())
        addFile(forkmesh::paths::settingsFilePath(QSettings()));
    addFile(QDir::homePath() + QStringLiteral("/.local/bin/forkmesh"));
    addFile(dataHome + QStringLiteral("/applications/forkmesh.desktop"));
    addFile(dataHome + QStringLiteral("/icons/forkmesh.png"));
    addFile(dataHome + QStringLiteral("/nautilus/scripts/Open with ForkMesh"));
    addFile(dataHome + QStringLiteral("/kio/servicemenus/forkmesh-open.desktop"));
    addFile(dataHome + QStringLiteral("/nemo/actions/forkmesh-open.nemo_action"));
    QDirIterator iconIt(dataHome + QStringLiteral("/icons/hicolor"),
                        {QStringLiteral("forkmesh.png"),
                         QStringLiteral("forkmesh.svg")},
                        QDir::Files,
                        QDirIterator::Subdirectories);
    while (iconIt.hasNext())
        addFile(iconIt.next());

    QString detail = QStringLiteral(
        "This permanently and irreversibly erases ForkMesh from this computer, "
        "including:\n\n"
        "  •  every mirrored repository\n"
        "  •  this machine's identity key (your account cannot be recovered)\n"
        "  •  all chat history, settings and caches\n"
        "  •  the desktop launcher and icons\n"
        "  •  the ForkMesh program files\n\nFolders removed:\n");
    for (const QString &d : std::as_const(dirs))
        detail += "    " + d + "/\n";
    if (!kept.isEmpty()) {
        detail += QStringLiteral(
            "\nKept, because they are git checkouts or hold other projects; "
            "delete them yourself if you no longer need them:\n");
        for (const QString &d : std::as_const(kept))
            detail += "    " + d + "/\n";
    }
    detail += QStringLiteral("\nForkMesh will quit when it is done.");

    QMessageBox box(QMessageBox::Warning, QStringLiteral("Uninstall ForkMesh"),
                    detail, QMessageBox::Cancel, this);
    QPushButton *go =
        box.addButton(QStringLiteral("Uninstall…"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;

    bool ok = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Confirm uninstall"),
        QStringLiteral("Type DELETE to permanently erase ForkMesh:"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || typed.trimmed().compare(QStringLiteral("DELETE"),
                                       Qt::CaseInsensitive) != 0)
        return;

    if (m_heartbeatTimer)
        m_heartbeatTimer->stop();
    stopNodeEventSocket();
    if (m_backend) {
        m_backend->disconnect(this);
        m_backend->shutdown();
        m_backend->deleteLater();
        m_backend = nullptr;
    }

    const QStringList all = dirs + files;
#if defined(Q_OS_UNIX)
    auto shQuote = [](const QString &s) {
        return QLatin1Char('\'') + QString(s).replace(QStringLiteral("'"),
                                                      QStringLiteral("'\\''")) +
               QLatin1Char('\'');
    };
    QStringList quoted;
    for (const QString &p : all)
        quoted << shQuote(p);
    QDir::setCurrent(QDir::homePath());
    const bool spawned = forkmesh::processreaper::startReleased(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"),
         QStringLiteral("sleep 1; rm -rf ") + quoted.join(QLatin1Char(' '))});
    if (!spawned) {
        for (const QString &f : std::as_const(files))
            QFile::remove(f);
        for (const QString &d : std::as_const(dirs))
            QDir(d).removeRecursively();
    }
#else
    for (const QString &f : std::as_const(files))
        QFile::remove(f);
    for (const QString &d : std::as_const(dirs))
        QDir(d).removeRecursively();
#endif

    QCoreApplication::exit(0);
}

void MainWindow::installChatRetentionKeyResolver()
{
    QPointer<MainWindow> self(this);
    forkmesh::chatretention::keyResolver() =
        [self](const QString &conversation) -> QString {
        if (!self || !self->m_officeChannelMirror ||
            !forkmesh::office::isOfficeConversation(conversation))
            return {};
        const QString id =
            self->m_officeChannelMirror->channelIdFor(conversation);
        return id.isEmpty() ? QString() : forkmesh::chatretention::officeKey(id);
    };
}

namespace {

constexpr int kUserActivitySyncMs = 2 * 60 * 1000;
constexpr qint64 kUserActivityReadMs = 10 * 60 * 1000;
constexpr qint64 kUserActivityForcedFloorMs = 10 * 1000;

}

void MainWindow::startUserActivitySync()
{
    if (!m_userActivitySyncTimer) {
        m_userActivitySyncTimer = new QTimer(this);
        m_userActivitySyncTimer->setInterval(kUserActivitySyncMs);
        connect(m_userActivitySyncTimer, &QTimer::timeout, this,
                [this] { syncUserActivity(false); });
    }
    m_userActivitySyncTimer->start();
    QTimer::singleShot(0, this, [this] { syncUserActivity(true); });
}

void MainWindow::forgetUserActivitySync()
{
    if (m_userActivitySyncTimer)
        m_userActivitySyncTimer->stop();
    m_userActivitySentRevision = ~quint64(0);
    m_userActivitySyncedMs = 0;
    if (auto *log = forkmesh::activity::UserActivityLog::instance())
        log->clearOthers();
}

void MainWindow::syncUserActivity(bool force)
{
    using namespace forkmesh::activity;
    UserActivityLog *log = UserActivityLog::instance();
    if (!log || !m_networkAccess || m_userActivitySyncBusy)
        return;
    const QUrl url = accountsApiUrl(QStringLiteral("activity-today"));
    const QString token = accountSessionTokenForUrl(url);
    if (token.isEmpty())
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 revision = log->revision();
    const bool changed = revision != m_userActivitySentRevision;
    const qint64 since = nowMs - m_userActivitySyncedMs;
    if (!changed && since < (force ? kUserActivityForcedFloorMs
                                   : kUserActivityReadMs))
        return;
    const QDate day = QDate::currentDate();
    QJsonObject payload{
        {QStringLiteral("sessionToken"), token},
        {QStringLiteral("day"), day.toString(Qt::ISODate)},
        {QStringLiteral("device"), log->deviceKey()},
        {QStringLiteral("kind"), QStringLiteral("desktop")},
    };
    if (changed) {
        QJsonArray counts;
        for (int value : log->localSlotsFor(day))
            counts.append(value);
        payload.insert(QStringLiteral("counts"), counts);
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(15000);
    m_userActivitySyncBusy = true;
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QString account = AccountCapability::normalizedAccount(m_accountName);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, account, day, revision, changed, nowMs] {
                m_userActivitySyncBusy = false;
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QJsonObject resp =
                    QJsonDocument::fromJson(reply->readAll()).object();
                reply->deleteLater();
                if (status < 200 || status >= 300 ||
                    AccountCapability::normalizedAccount(m_accountName) != account ||
                    AccountCapability::normalizedAccount(
                        resp.value(QStringLiteral("nodeName")).toString()) != account)
                    return;
                auto *log = forkmesh::activity::UserActivityLog::instance();
                const QJsonArray others =
                    resp.value(QStringLiteral("others")).toArray();
                if (!log || others.size() != forkmesh::activity::kActivitySlots)
                    return;
                if (changed)
                    m_userActivitySentRevision = revision;
                m_userActivitySyncedMs = nowMs;
                QVector<int> counts;
                counts.reserve(others.size());
                for (const QJsonValue &value : others)
                    counts.append(value.toInt());
                QStringList kinds;
                for (const QJsonValue &value :
                     resp.value(QStringLiteral("sources")).toArray()) {
                    const QJsonObject source = value.toObject();
                    if (!source.value(QStringLiteral("self")).toBool() &&
                        source.value(QStringLiteral("total")).toInt() > 0)
                        kinds.append(source.value(QStringLiteral("kind")).toString());
                }
                log->setOthers(day, counts, kinds);
            });
}

void MainWindow::fetchChatRetention()
{
    postChatRetention(QJsonObject());
}

void MainWindow::postChatRetention(const QJsonObject &body)
{
    const QUrl url = accountsApiUrl(
        QString::fromLatin1(forkmesh::chatretention::kEndpointLeaf));
    const QString token = accountSessionTokenForUrl(url);
    if (!m_networkAccess || token.isEmpty())
        return;
    QJsonObject payload = body;
    payload.insert(QStringLiteral("sessionToken"), token);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QString account = AccountCapability::normalizedAccount(m_accountName);
    const bool write = !body.isEmpty();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, account, write] {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QJsonObject resp =
                    QJsonDocument::fromJson(reply->readAll()).object();
                reply->deleteLater();
                const bool sameAccount =
                    AccountCapability::normalizedAccount(m_accountName) ==
                        account &&
                    AccountCapability::normalizedAccount(
                        resp.value(QStringLiteral("nodeName")).toString()) ==
                        account;
                if (status >= 200 && status < 300 && sameAccount &&
                    resp.value(QStringLiteral("chatRetention")).isObject()) {
                    applyChatRetention(
                        resp.value(QStringLiteral("chatRetention")).toObject());
                    return;
                }
                if (write)
                    showPageNotice(
                        QStringLiteral("Could not save when chat messages "
                                       "disappear. It applies on this "
                                       "computer until the next sign-in."),
                        true);
            });
}

void MainWindow::applyChatRetention(const QJsonObject &object)
{
    setChatRetentionPreferences(forkmesh::chatretention::fromJson(object));
}

void MainWindow::setChatRetentionPreferences(
    const forkmesh::chatretention::Preferences &prefs)
{
    if (!forkmesh::chatretention::setPreferences(prefs))
        return;
    syncChatRetentionControls();
    pruneExpiredChatHistory();
    if (!m_currentConversation.isEmpty())
        rebuildConversationView();
}

void MainWindow::syncChatRetentionControls()
{
    if (!m_chatRetentionCombo)
        return;
    const QSignalBlocker blocker(m_chatRetentionCombo);
    const int index = m_chatRetentionCombo->findData(
        forkmesh::chatretention::defaultMs());
    m_chatRetentionCombo->setCurrentIndex(index < 0 ? 0 : index);
}

void MainWindow::setDefaultChatRetention(qint64 ms)
{
    namespace retention = forkmesh::chatretention;
    setChatRetentionPreferences(
        retention::withDefault(retention::preferences(), ms));
    postChatRetention(retention::defaultBody(ms));
}

void MainWindow::setChatRetentionFor(const QString &conversation, qint64 ms)
{
    namespace retention = forkmesh::chatretention;
    const QString key = retention::chatKeyFor(conversation);
    if (!retention::validKey(key))
        return;
    setChatRetentionPreferences(
        retention::withChat(retention::preferences(), key, ms));
    postChatRetention(retention::chatBody(key, ms));
}

void MainWindow::addChatRetentionMenu(QMenu *menu, const QString &conversation)
{
    namespace retention = forkmesh::chatretention;
    if (!menu || conversation.isEmpty())
        return;
    QMenu *sub = menu->addMenu(QStringLiteral("Messages disappear after"));
    const qint64 current = retention::overrideMsFor(conversation);
    QAction *useDefault = sub->addAction(
        QStringLiteral("Default (%1)").arg(retention::label(retention::defaultMs())));
    useDefault->setCheckable(true);
    useDefault->setChecked(!current);
    connect(useDefault, &QAction::triggered, this, [this, conversation] {
        setChatRetentionFor(conversation, 0);
    });
    sub->addSeparator();
    for (const retention::Choice &choice : retention::choices()) {
        QAction *action = sub->addAction(choice.label);
        action->setCheckable(true);
        action->setChecked(current == choice.ms);
        const qint64 ms = choice.ms;
        connect(action, &QAction::triggered, this, [this, conversation, ms] {
            setChatRetentionFor(conversation, ms);
        });
    }
}

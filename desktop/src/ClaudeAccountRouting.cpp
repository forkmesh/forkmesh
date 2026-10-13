#include "ClaudeAccountRouting.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace forkmesh::agentaccounts {

namespace {

const QString kOauthAccount = QStringLiteral("oauthAccount");

bool readJsonObject(const QString &path, QJsonObject *out, bool *exists)
{
    QFile file(path);
    *exists = file.exists();
    if (!*exists) {
        *out = QJsonObject();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = file.readAll();
    if (bytes.trimmed().isEmpty()) {
        *out = QJsonObject();
        return true;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *out = document.object();
    return true;
}

bool writePrivateFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size())
        return false;
    return file.commit();
}

void fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

QString ClaudeLoginIdentity::key() const
{
    if (!isValid())
        return QString();
    return accountUuid + QLatin1Char('/') + organizationUuid;
}

QString ClaudeLoginIdentity::label() const
{
    if (!email.isEmpty())
        return email;
    if (!displayName.isEmpty())
        return displayName;
    return accountUuid;
}

QString claudeStateFile(const QString &configDir, bool homeStateFile)
{
    if (homeStateFile)
        return QDir::homePath() + QStringLiteral("/.claude.json");
    return QDir(configDir).filePath(QStringLiteral(".claude.json"));
}

QString claudeCredentialFile(const QString &configDir)
{
    return QDir(configDir).filePath(QStringLiteral(".credentials.json"));
}

ClaudeLoginIdentity readClaudeLoginIdentity(const QString &stateFile)
{
    QJsonObject state;
    bool exists = false;
    if (!readJsonObject(stateFile, &state, &exists))
        return {};
    const QJsonObject account = state.value(kOauthAccount).toObject();
    ClaudeLoginIdentity identity;
    identity.accountUuid =
        account.value(QStringLiteral("accountUuid")).toString().trimmed();
    identity.organizationUuid =
        account.value(QStringLiteral("organizationUuid")).toString().trimmed();
    identity.email =
        account.value(QStringLiteral("emailAddress")).toString().trimmed();
    identity.displayName =
        account.value(QStringLiteral("displayName")).toString().trimmed();
    return identity;
}

LoginRoute routeClaudeLogin(const QString &identityKey,
                            const QString &requestedId,
                            const QList<AccountBinding> &accounts)
{
    if (identityKey.isEmpty())
        return {};
    for (const AccountBinding &account : accounts)
        if (account.id == requestedId && account.boundKey == identityKey)
            return {LoginRouteKind::Existing, account.id};
    for (const AccountBinding &account : accounts)
        if (account.boundKey == identityKey)
            return {LoginRouteKind::Existing, account.id};
    for (const AccountBinding &account : accounts)
        if (!requestedId.isEmpty() && account.id == requestedId &&
            account.boundKey.isEmpty() && !account.signedIn)
            return {LoginRouteKind::Adopt, account.id};
    return {};
}

bool moveClaudeLogin(const QString &fromDir, const QString &fromStateFile,
                     const QString &toDir, const QString &toStateFile,
                     QString *error)
{
    const QString fromCredentials = claudeCredentialFile(fromDir);
    const QString toCredentials = claudeCredentialFile(toDir);
    if (QFileInfo(fromCredentials).absoluteFilePath() ==
        QFileInfo(toCredentials).absoluteFilePath()) {
        fail(error, QStringLiteral("the login is already in %1").arg(toDir));
        return false;
    }
    QFile credentials(fromCredentials);
    if (!credentials.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("no login to move in %1").arg(fromDir));
        return false;
    }
    const QByteArray credentialBytes = credentials.readAll();
    credentials.close();

    QJsonObject fromState;
    bool fromExists = false;
    if (!readJsonObject(fromStateFile, &fromState, &fromExists) ||
        !fromState.value(kOauthAccount).isObject()) {
        fail(error, QStringLiteral("%1 does not name the signed-in account")
                        .arg(fromStateFile));
        return false;
    }
    QJsonObject toState;
    bool toExists = false;
    if (!readJsonObject(toStateFile, &toState, &toExists)) {
        fail(error, QStringLiteral("%1 could not be read").arg(toStateFile));
        return false;
    }

    if (!QDir().mkpath(toDir)) {
        fail(error, QStringLiteral("could not create %1").arg(toDir));
        return false;
    }
    if (!writePrivateFile(toCredentials, credentialBytes)) {
        fail(error, QStringLiteral("could not write %1").arg(toCredentials));
        return false;
    }
    toState.insert(kOauthAccount, fromState.value(kOauthAccount));
    const QString onboarded = QStringLiteral("hasCompletedOnboarding");
    if (!toState.contains(onboarded) && fromState.contains(onboarded))
        toState.insert(onboarded, fromState.value(onboarded));
    if (!writePrivateFile(toStateFile,
                          QJsonDocument(toState).toJson(QJsonDocument::Indented))) {
        fail(error, QStringLiteral("could not write %1").arg(toStateFile));
        return false;
    }

    QFile::remove(fromCredentials);
    fromState.remove(kOauthAccount);
    writePrivateFile(fromStateFile,
                     QJsonDocument(fromState).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace forkmesh::agentaccounts

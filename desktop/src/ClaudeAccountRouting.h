#pragma once

#include <QList>
#include <QString>

namespace forkmesh::agentaccounts {

struct ClaudeLoginIdentity {
    QString accountUuid;
    QString organizationUuid;
    QString email;
    QString displayName;

    bool isValid() const { return !accountUuid.isEmpty(); }
    QString key() const;
    QString label() const;
};

QString claudeStateFile(const QString &configDir, bool homeStateFile);

QString claudeCredentialFile(const QString &configDir);

ClaudeLoginIdentity readClaudeLoginIdentity(const QString &stateFile);

struct AccountBinding {
    QString id;
    QString boundKey;
    bool signedIn = false;
};

enum class LoginRouteKind { Existing, Adopt, New };

struct LoginRoute {
    LoginRouteKind kind = LoginRouteKind::New;
    QString accountId;
};

LoginRoute routeClaudeLogin(const QString &identityKey,
                            const QString &requestedId,
                            const QList<AccountBinding> &accounts);

bool moveClaudeLogin(const QString &fromDir, const QString &fromStateFile,
                     const QString &toDir, const QString &toStateFile,
                     QString *error = nullptr);

} // namespace forkmesh::agentaccounts

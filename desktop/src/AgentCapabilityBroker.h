#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace AgentHostCapabilities {
inline const QString FileRead = QStringLiteral("file.read");
inline const QString FileWrite = QStringLiteral("file.write");
inline const QString NetworkConnect = QStringLiteral("network.connect");
inline const QString ProcessExecute = QStringLiteral("process.execute");
inline const QString DeviceControl = QStringLiteral("device.control");
inline const QString SecretRead = QStringLiteral("secret.read");
inline const QString ToolInvoke = QStringLiteral("tool.invoke");
inline const QString ResourceRead = QStringLiteral("resource.read");
} // namespace AgentHostCapabilities

enum class AgentCapabilityDecision {
    Prompt,
    Allow,
    Deny,
};

enum class AgentGrantLifetime {
    Once,
    Session,
    Worktree,
    Persistent,
};

struct AgentCapabilityRequest {
    QString capability;
    // An opaque host-owned identifier, never a secret value. File resources
    // should be normalized absolute paths; tools/resources should use stable
    // namespaced IDs.
    QString resource;
    QString sessionId;
    QString worktreePath;

    bool isValid(QString *error = nullptr) const;
};

struct AgentCapabilityGrant {
    QString id;
    QString capability;
    QString resource;
    QString sessionId;
    QString worktreePath;
    AgentGrantLifetime lifetime = AgentGrantLifetime::Once;
    AgentCapabilityDecision decision = AgentCapabilityDecision::Prompt;
    qint64 sequence = 0;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, AgentCapabilityGrant *out,
                         QString *error = nullptr);
};

class AgentCapabilityBroker
{
public:
    AgentCapabilityDecision evaluate(const AgentCapabilityRequest &request,
                                     QString *matchedGrantId = nullptr);
    QString grant(const AgentCapabilityRequest &request,
                  AgentGrantLifetime lifetime,
                  AgentCapabilityDecision decision,
                  QString *error = nullptr);
    bool revoke(const QString &grantId);
    void clearSession(const QString &sessionId);
    void clearWorktree(const QString &worktreePath);
    void clearEphemeral();

    QJsonArray persistentGrants() const;
    bool loadPersistentGrants(const QJsonArray &grants,
                              QString *error = nullptr);
    int size() const { return m_grants.size(); }

private:
    static QString normalizedWorktree(const QString &path);
    static bool matches(const AgentCapabilityGrant &grant,
                        const AgentCapabilityRequest &request);
    static int precedence(const AgentCapabilityGrant &grant,
                          const AgentCapabilityRequest &request);

    QList<AgentCapabilityGrant> m_grants;
    qint64 m_nextSequence = 1;
};

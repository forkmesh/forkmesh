#pragma once

#include "AgentCapabilityBroker.h"
#include "AgentRuntimeSession.h"

#include <QHash>
#include <QSet>
#include <QString>

enum class AgentPermissionStatus {
    Invalid,
    Prompt,
    Resolved,
};

struct AgentPermissionResult {
    AgentPermissionStatus status = AgentPermissionStatus::Invalid;
    AgentCapabilityDecision decision = AgentCapabilityDecision::Deny;
    AgentCapabilityRequest request;
    AgentRuntimeCommand response;
    QString matchedGrantId;
    QString error;
};

// Host-side coordinator for normalized runtime permission requests. It does
// not display UI or execute a tool; callers must show Prompt results and may
// dispatch only Resolved response commands to the originating runtime.
class AgentPermissionController
{
public:
    explicit AgentPermissionController(AgentCapabilityBroker *broker);

    AgentPermissionResult handleRequest(const AgentRuntimeEvent &event,
                                        const QString &hostSessionId,
                                        const QString &hostWorktreePath);
    AgentPermissionResult resolve(const QString &sessionId,
                                  const QString &requestId,
                                  AgentGrantLifetime lifetime,
                                  AgentCapabilityDecision decision);

    void clearSession(const QString &sessionId);
    void clearWorktree(const QString &worktreePath);
    bool isPending(const QString &sessionId, const QString &requestId) const;
    int pendingCount() const { return m_pending.size(); }

private:
    struct Pending {
        AgentCapabilityRequest request;
    };

    static QString requestKey(const QString &sessionId,
                              const QString &requestId);
    AgentRuntimeCommand responseCommand(const QString &requestId,
                                        AgentCapabilityDecision decision,
                                        const QString &source,
                                        const QString &grantId = QString(),
                                        AgentGrantLifetime lifetime =
                                            AgentGrantLifetime::Once);
    AgentPermissionResult deny(const QString &requestId,
                               const QString &message);

    AgentCapabilityBroker *m_broker = nullptr;
    QHash<QString, Pending> m_pending;
    QSet<QString> m_seen;
    QHash<QString, int> m_seenBySession;
    qint64 m_nextSequence = 1;
};

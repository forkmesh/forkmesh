#include "AgentPermissionController.h"

#include <QDir>

namespace {

constexpr int kMaximumPendingRequestsPerSession = 128;
constexpr int kMaximumPendingRequests = 1024;
constexpr int kMaximumSeenRequestsPerSession = 4096;

QString decisionName(AgentCapabilityDecision decision)
{
    return decision == AgentCapabilityDecision::Allow
               ? QStringLiteral("allow")
               : QStringLiteral("deny");
}

QString lifetimeName(AgentGrantLifetime lifetime)
{
    switch (lifetime) {
    case AgentGrantLifetime::Once:
        return QStringLiteral("once");
    case AgentGrantLifetime::Session:
        return QStringLiteral("session");
    case AgentGrantLifetime::Worktree:
        return QStringLiteral("worktree");
    case AgentGrantLifetime::Persistent:
        return QStringLiteral("persistent");
    }
    return {};
}

bool validRequestId(const QString &requestId)
{
    return !requestId.isEmpty() && requestId.size() <= 256 &&
           !requestId.contains(QChar::Null) &&
           !requestId.contains(QLatin1Char('\n')) &&
           !requestId.contains(QLatin1Char('\r'));
}

} // namespace

AgentPermissionController::AgentPermissionController(
    AgentCapabilityBroker *broker)
    : m_broker(broker)
{}

AgentPermissionResult AgentPermissionController::handleRequest(
    const AgentRuntimeEvent &event, const QString &hostSessionId,
    const QString &hostWorktreePath)
{
    if (!m_broker)
        return deny(event.requestId,
                    QStringLiteral("capability broker is unavailable"));
    QString envelopeError;
    if (!event.isValid(&envelopeError))
        return deny(event.requestId, envelopeError);
    if (event.type != AgentRuntimeEventTypes::PermissionRequested)
        return deny(event.requestId,
                    QStringLiteral("event is not a permission request"));
    if (hostSessionId.isEmpty() || event.sessionId != hostSessionId)
        return deny(event.requestId,
                    QStringLiteral("permission request session mismatch"));
    if (!validRequestId(event.requestId))
        return deny({}, QStringLiteral("permission request id is invalid"));

    const QString key = requestKey(hostSessionId, event.requestId);
    if (m_seen.contains(key))
        return deny(event.requestId,
                    QStringLiteral("permission request id was replayed"));
    if (m_seenBySession.value(hostSessionId) >=
        kMaximumSeenRequestsPerSession)
        return deny(event.requestId,
                    QStringLiteral("permission request history limit reached"));
    m_seen.insert(key);
    ++m_seenBySession[hostSessionId];

    AgentCapabilityRequest request{
        event.data.value(QStringLiteral("capability")).toString(),
        event.data.value(QStringLiteral("resource")).toString(),
        hostSessionId,
        hostWorktreePath.trimmed().isEmpty()
            ? QString()
            : QDir::cleanPath(hostWorktreePath)};
    QString validationError;
    if (!request.isValid(&validationError))
        return deny(event.requestId, validationError);

    QString matchedGrantId;
    const AgentCapabilityDecision decision =
        m_broker->evaluate(request, &matchedGrantId);
    if (decision != AgentCapabilityDecision::Prompt) {
        AgentPermissionResult result;
        result.status = AgentPermissionStatus::Resolved;
        result.decision = decision;
        result.request = request;
        result.matchedGrantId = matchedGrantId;
        result.response = responseCommand(event.requestId, decision,
                                          QStringLiteral("stored-grant"),
                                          matchedGrantId);
        return result;
    }

    const QString sessionPrefix = hostSessionId + QChar::Null;
    int sessionPending = 0;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it.key().startsWith(sessionPrefix))
            ++sessionPending;
    }
    if (sessionPending >= kMaximumPendingRequestsPerSession ||
        m_pending.size() >= kMaximumPendingRequests)
        return deny(event.requestId,
                    QStringLiteral("too many pending permission requests"));
    m_pending.insert(key, Pending{request});
    AgentPermissionResult result;
    result.status = AgentPermissionStatus::Prompt;
    result.decision = AgentCapabilityDecision::Prompt;
    result.request = request;
    return result;
}

AgentPermissionResult AgentPermissionController::resolve(
    const QString &sessionId, const QString &requestId,
    AgentGrantLifetime lifetime, AgentCapabilityDecision decision)
{
    if (!m_broker)
        return deny(requestId,
                    QStringLiteral("capability broker is unavailable"));
    if (decision == AgentCapabilityDecision::Prompt)
        return deny(requestId,
                    QStringLiteral("a permission response must allow or deny"));
    const QString key = requestKey(sessionId, requestId);
    const auto pending = m_pending.constFind(key);
    if (pending == m_pending.constEnd())
        return deny(requestId,
                    QStringLiteral("permission request is not pending"));

    QString error;
    const QString grantId =
        m_broker->grant(pending->request, lifetime, decision, &error);
    if (grantId.isEmpty()) {
        m_pending.erase(pending);
        return deny(requestId, error);
    }

    QString consumedGrantId;
    const AgentCapabilityDecision applied =
        m_broker->evaluate(pending->request, &consumedGrantId);
    if (applied != decision || consumedGrantId != grantId) {
        m_broker->revoke(grantId);
        m_pending.erase(pending);
        return deny(requestId,
                    QStringLiteral("permission grant could not be applied"));
    }

    const AgentCapabilityRequest request = pending->request;
    m_pending.erase(pending);
    AgentPermissionResult result;
    result.status = AgentPermissionStatus::Resolved;
    result.decision = decision;
    result.request = request;
    result.matchedGrantId = grantId;
    result.response = responseCommand(requestId, decision,
                                      QStringLiteral("user"), grantId,
                                      lifetime);
    return result;
}

void AgentPermissionController::clearSession(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return;
    if (m_broker)
        m_broker->clearSession(sessionId);
    const QString prefix = sessionId + QChar::Null;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it.key().startsWith(prefix))
            it = m_pending.erase(it);
        else
            ++it;
    }
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (it->startsWith(prefix))
            it = m_seen.erase(it);
        else
            ++it;
    }
    m_seenBySession.remove(sessionId);
}

void AgentPermissionController::clearWorktree(const QString &worktreePath)
{
    const QString normalized =
        worktreePath.trimmed().isEmpty() ? QString()
                                        : QDir::cleanPath(worktreePath);
    if (normalized.isEmpty())
        return;
    if (m_broker)
        m_broker->clearWorktree(normalized);
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->request.worktreePath == normalized)
            it = m_pending.erase(it);
        else
            ++it;
    }
}

bool AgentPermissionController::isPending(const QString &sessionId,
                                          const QString &requestId) const
{
    return m_pending.contains(requestKey(sessionId, requestId));
}

QString AgentPermissionController::requestKey(const QString &sessionId,
                                              const QString &requestId)
{
    return sessionId + QChar::Null + requestId;
}

AgentRuntimeCommand AgentPermissionController::responseCommand(
    const QString &requestId, AgentCapabilityDecision decision,
    const QString &source, const QString &grantId, AgentGrantLifetime lifetime)
{
    QJsonObject data{{QStringLiteral("decision"), decisionName(decision)},
                     {QStringLiteral("source"), source}};
    if (!grantId.isEmpty())
        data.insert(QStringLiteral("grantId"), grantId);
    if (source == QLatin1String("user"))
        data.insert(QStringLiteral("lifetime"), lifetimeName(lifetime));
    return {1, AgentRuntimeCommandTypes::RequestRespond, requestId,
            m_nextSequence++, data};
}

AgentPermissionResult AgentPermissionController::deny(
    const QString &requestId, const QString &message)
{
    AgentPermissionResult result;
    result.decision = AgentCapabilityDecision::Deny;
    result.error = message;
    if (validRequestId(requestId)) {
        result.status = AgentPermissionStatus::Resolved;
        result.response = responseCommand(requestId,
                                          AgentCapabilityDecision::Deny,
                                          QStringLiteral("host-validation"));
    }
    return result;
}

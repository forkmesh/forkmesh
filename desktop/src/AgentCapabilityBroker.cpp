#include "AgentCapabilityBroker.h"

#include <QDir>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool validIdentifier(const QString &value, qsizetype maximum = 1024)
{
    if (value.isEmpty() || value.size() > maximum)
        return false;
    for (const QChar ch : value) {
        if (ch.isNull() || ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
            return false;
    }
    return true;
}

bool validCapability(const QString &value)
{
    if (value.isEmpty() || value.size() > 128)
        return false;
    for (const QChar ch : value) {
        const ushort code = ch.unicode();
        if (!((code >= 'a' && code <= 'z') ||
              (code >= '0' && code <= '9') || ch == QLatin1Char('.') ||
              ch == QLatin1Char('_') || ch == QLatin1Char('-') ||
              ch == QLatin1Char(':')))
            return false;
    }
    return true;
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

bool parseLifetime(const QString &name, AgentGrantLifetime *out)
{
    if (name == QLatin1String("once"))
        *out = AgentGrantLifetime::Once;
    else if (name == QLatin1String("session"))
        *out = AgentGrantLifetime::Session;
    else if (name == QLatin1String("worktree"))
        *out = AgentGrantLifetime::Worktree;
    else if (name == QLatin1String("persistent"))
        *out = AgentGrantLifetime::Persistent;
    else
        return false;
    return true;
}

QString decisionName(AgentCapabilityDecision decision)
{
    if (decision == AgentCapabilityDecision::Allow)
        return QStringLiteral("allow");
    if (decision == AgentCapabilityDecision::Deny)
        return QStringLiteral("deny");
    return QStringLiteral("prompt");
}

bool parseDecision(const QString &name, AgentCapabilityDecision *out)
{
    if (name == QLatin1String("allow"))
        *out = AgentCapabilityDecision::Allow;
    else if (name == QLatin1String("deny"))
        *out = AgentCapabilityDecision::Deny;
    else
        return false;
    return true;
}

} // namespace

bool AgentCapabilityRequest::isValid(QString *error) const
{
    if (!validCapability(capability))
        return fail(error, QStringLiteral("invalid capability id"));
    if (!validIdentifier(resource, 4096))
        return fail(error, QStringLiteral("invalid resource id"));
    if (sessionId.size() > 256 || sessionId.contains(QLatin1Char('\n')) ||
        sessionId.contains(QLatin1Char('\r')) || sessionId.contains(QChar::Null))
        return fail(error, QStringLiteral("invalid session id"));
    if (worktreePath.size() > 4096 || worktreePath.contains(QChar::Null) ||
        worktreePath.contains(QLatin1Char('\n')) ||
        worktreePath.contains(QLatin1Char('\r')))
        return fail(error, QStringLiteral("invalid worktree path"));
    if (!worktreePath.isEmpty() && !QDir::isAbsolutePath(worktreePath))
        return fail(error, QStringLiteral("worktree path must be absolute"));
    if ((capability == AgentHostCapabilities::FileRead ||
         capability == AgentHostCapabilities::FileWrite) &&
        resource != QLatin1String("*") && !QDir::isAbsolutePath(resource))
        return fail(error, QStringLiteral("file resource must be an absolute path"));
    if (error)
        error->clear();
    return true;
}

QJsonObject AgentCapabilityGrant::toJson() const
{
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("id"), id},
            {QStringLiteral("capability"), capability},
            {QStringLiteral("resource"), resource},
            {QStringLiteral("sessionId"), sessionId},
            {QStringLiteral("worktreePath"), worktreePath},
            {QStringLiteral("lifetime"), lifetimeName(lifetime)},
            {QStringLiteral("decision"), decisionName(decision)},
            {QStringLiteral("sequence"), sequence}};
}

bool AgentCapabilityGrant::fromJson(const QJsonObject &json,
                                    AgentCapabilityGrant *out,
                                    QString *error)
{
    if (!out)
        return fail(error, QStringLiteral("missing grant destination"));
    if (json.value(QStringLiteral("schemaVersion")).toInt() != 1)
        return fail(error, QStringLiteral("unsupported grant schema"));
    AgentCapabilityGrant parsed;
    parsed.id = json.value(QStringLiteral("id")).toString();
    parsed.capability = json.value(QStringLiteral("capability")).toString();
    parsed.resource = json.value(QStringLiteral("resource")).toString();
    parsed.sessionId = json.value(QStringLiteral("sessionId")).toString();
    parsed.worktreePath = json.value(QStringLiteral("worktreePath")).toString();
    parsed.sequence = json.value(QStringLiteral("sequence")).toVariant().toLongLong();
    if (!validIdentifier(parsed.id) || !validCapability(parsed.capability) ||
        !validIdentifier(parsed.resource, 4096) || parsed.sequence < 1)
        return fail(error, QStringLiteral("malformed grant"));
    if (!parseLifetime(json.value(QStringLiteral("lifetime")).toString(),
                       &parsed.lifetime) ||
        !parseDecision(json.value(QStringLiteral("decision")).toString(),
                       &parsed.decision))
        return fail(error, QStringLiteral("invalid grant policy"));
    if (parsed.lifetime != AgentGrantLifetime::Persistent)
        return fail(error, QStringLiteral("only persistent grants may be loaded"));
    parsed.sessionId.clear();
    parsed.worktreePath.clear();
    *out = parsed;
    if (error)
        error->clear();
    return true;
}

AgentCapabilityDecision
AgentCapabilityBroker::evaluate(const AgentCapabilityRequest &request,
                                QString *matchedGrantId)
{
    if (matchedGrantId)
        matchedGrantId->clear();
    if (!request.isValid())
        return AgentCapabilityDecision::Deny;

    int bestIndex = -1;
    int bestPrecedence = -1;
    for (int index = 0; index < m_grants.size(); ++index) {
        const AgentCapabilityGrant &candidate = m_grants.at(index);
        if (!matches(candidate, request))
            continue;
        const int candidatePrecedence = precedence(candidate, request);
        if (bestIndex >= 0 &&
            candidate.decision != m_grants.at(bestIndex).decision) {
            // A matching deny always wins. Allowing an override without first
            // revoking the deny makes overlapping persistent/session policies
            // ambiguous and risks an accidental privilege escalation.
            if (candidate.decision == AgentCapabilityDecision::Deny) {
                bestIndex = index;
                bestPrecedence = candidatePrecedence;
            }
            continue;
        }
        const bool stronger = candidatePrecedence > bestPrecedence;
        const bool sameDecisionButNewer =
            candidatePrecedence == bestPrecedence && bestIndex >= 0 &&
            candidate.sequence > m_grants.at(bestIndex).sequence;
        if (stronger || sameDecisionButNewer) {
            bestIndex = index;
            bestPrecedence = candidatePrecedence;
        }
    }
    if (bestIndex < 0)
        return AgentCapabilityDecision::Prompt;

    const AgentCapabilityGrant matched = m_grants.at(bestIndex);
    if (matchedGrantId)
        *matchedGrantId = matched.id;
    if (matched.lifetime == AgentGrantLifetime::Once)
        m_grants.removeAt(bestIndex);
    return matched.decision;
}

QString AgentCapabilityBroker::grant(const AgentCapabilityRequest &request,
                                     AgentGrantLifetime lifetime,
                                     AgentCapabilityDecision decision,
                                     QString *error)
{
    if (!request.isValid(error))
        return {};
    if (decision == AgentCapabilityDecision::Prompt) {
        fail(error, QStringLiteral("prompt is not a storable grant decision"));
        return {};
    }
    if (lifetime == AgentGrantLifetime::Session && request.sessionId.isEmpty()) {
        fail(error, QStringLiteral("session grants require a session id"));
        return {};
    }
    const QString worktree = normalizedWorktree(request.worktreePath);
    if (lifetime == AgentGrantLifetime::Worktree && worktree.isEmpty()) {
        fail(error, QStringLiteral("worktree grants require a worktree path"));
        return {};
    }

    AgentCapabilityGrant saved;
    saved.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    saved.capability = request.capability;
    saved.resource = request.resource;
    saved.lifetime = lifetime;
    saved.decision = decision;
    saved.sequence = m_nextSequence++;
    if (lifetime == AgentGrantLifetime::Once ||
        lifetime == AgentGrantLifetime::Session)
        saved.sessionId = request.sessionId;
    if (lifetime == AgentGrantLifetime::Once ||
        lifetime == AgentGrantLifetime::Worktree)
        saved.worktreePath = worktree;
    m_grants.append(saved);
    if (error)
        error->clear();
    return saved.id;
}

bool AgentCapabilityBroker::revoke(const QString &grantId)
{
    const auto before = m_grants.size();
    m_grants.erase(
        std::remove_if(m_grants.begin(), m_grants.end(),
                       [&grantId](const AgentCapabilityGrant &grant) {
                           return grant.id == grantId;
                       }),
        m_grants.end());
    return m_grants.size() != before;
}

void AgentCapabilityBroker::clearSession(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return;
    m_grants.erase(
        std::remove_if(m_grants.begin(), m_grants.end(),
                       [&sessionId](const AgentCapabilityGrant &grant) {
                           return (grant.lifetime == AgentGrantLifetime::Session ||
                                   grant.lifetime == AgentGrantLifetime::Once) &&
                                  grant.sessionId == sessionId;
                       }),
        m_grants.end());
}

void AgentCapabilityBroker::clearWorktree(const QString &worktreePath)
{
    const QString normalized = normalizedWorktree(worktreePath);
    if (normalized.isEmpty())
        return;
    m_grants.erase(
        std::remove_if(m_grants.begin(), m_grants.end(),
                       [&normalized](const AgentCapabilityGrant &grant) {
                           return (grant.lifetime == AgentGrantLifetime::Worktree ||
                                   grant.lifetime == AgentGrantLifetime::Once) &&
                                  grant.worktreePath == normalized;
                       }),
        m_grants.end());
}

void AgentCapabilityBroker::clearEphemeral()
{
    m_grants.erase(
        std::remove_if(m_grants.begin(), m_grants.end(),
                       [](const AgentCapabilityGrant &grant) {
                           return grant.lifetime !=
                                  AgentGrantLifetime::Persistent;
                       }),
        m_grants.end());
}

QJsonArray AgentCapabilityBroker::persistentGrants() const
{
    QJsonArray result;
    for (const AgentCapabilityGrant &grant : m_grants) {
        if (grant.lifetime == AgentGrantLifetime::Persistent)
            result.append(grant.toJson());
    }
    return result;
}

bool AgentCapabilityBroker::loadPersistentGrants(const QJsonArray &grants,
                                                  QString *error)
{
    QList<AgentCapabilityGrant> parsed;
    QSet<QString> ids;
    qint64 nextSequence = m_nextSequence;
    QList<AgentCapabilityGrant> ephemeral;
    for (const AgentCapabilityGrant &grant : std::as_const(m_grants)) {
        if (grant.lifetime == AgentGrantLifetime::Persistent)
            continue;
        ephemeral.append(grant);
        ids.insert(grant.id);
    }
    for (const QJsonValue &value : grants) {
        if (!value.isObject())
            return fail(error, QStringLiteral("grant entry is not an object"));
        AgentCapabilityGrant grant;
        if (!AgentCapabilityGrant::fromJson(value.toObject(), &grant, error))
            return false;
        if (ids.contains(grant.id))
            return fail(error, QStringLiteral("duplicate persistent grant id"));
        ids.insert(grant.id);
        nextSequence = qMax(nextSequence, grant.sequence + 1);
        parsed.append(grant);
    }
    // Replace the persisted policy only after every entry validates so callers
    // never observe a partially loaded grant set.
    m_grants = ephemeral;
    m_grants.append(parsed);
    m_nextSequence = nextSequence;
    if (error)
        error->clear();
    return true;
}

QString AgentCapabilityBroker::normalizedWorktree(const QString &path)
{
    const QString trimmed = path.trimmed();
    return trimmed.isEmpty() ? QString() : QDir::cleanPath(trimmed);
}

bool AgentCapabilityBroker::matches(const AgentCapabilityGrant &grant,
                                    const AgentCapabilityRequest &request)
{
    if (grant.capability != request.capability ||
        (grant.resource != request.resource && grant.resource != QLatin1String("*")))
        return false;
    switch (grant.lifetime) {
    case AgentGrantLifetime::Once:
        return (grant.sessionId.isEmpty() ||
                grant.sessionId == request.sessionId) &&
               (grant.worktreePath.isEmpty() ||
                grant.worktreePath == normalizedWorktree(request.worktreePath));
    case AgentGrantLifetime::Session:
        return grant.sessionId == request.sessionId;
    case AgentGrantLifetime::Worktree:
        return grant.worktreePath == normalizedWorktree(request.worktreePath);
    case AgentGrantLifetime::Persistent:
        return true;
    }
    return false;
}

int AgentCapabilityBroker::precedence(const AgentCapabilityGrant &grant,
                                      const AgentCapabilityRequest &request)
{
    int score = grant.resource == request.resource ? 100 : 0;
    switch (grant.lifetime) {
    case AgentGrantLifetime::Once:
        return score + 40;
    case AgentGrantLifetime::Session:
        return score + 30;
    case AgentGrantLifetime::Worktree:
        return score + 20;
    case AgentGrantLifetime::Persistent:
        return score + 10;
    }
    return score;
}

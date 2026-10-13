#include "AgentRuntimeSession.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool validName(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$"));
    return pattern.match(value).hasMatch();
}

bool validEnvelope(int schemaVersion, const QString &type, qint64 sequence,
                   QString *error)
{
    if (schemaVersion != 1)
        return fail(error, QStringLiteral("unsupported schema version"));
    if (!validName(type))
        return fail(error, QStringLiteral("type must be a stable dotted name"));
    if (sequence < 0)
        return fail(error, QStringLiteral("sequence must not be negative"));
    return true;
}

QJsonObject envelopeJson(int schemaVersion, const QString &type,
                         const QString &requestId, qint64 sequence,
                         const QJsonObject &data)
{
    QJsonObject json{{QStringLiteral("schemaVersion"), schemaVersion},
                     {QStringLiteral("type"), type},
                     {QStringLiteral("sequence"), sequence},
                     {QStringLiteral("data"), data}};
    if (!requestId.isEmpty())
        json.insert(QStringLiteral("requestId"), requestId);
    return json;
}

} // namespace

QString agentRuntimeIdForLegacyProvider(const QString &provider)
{
    const QString normalized = provider.trimmed().toLower();
    if (normalized == QLatin1String("codex") ||
        normalized == QLatin1String("openai-codex"))
        return AgentRuntimeIds::CodexAppServer;
    if (normalized == QLatin1String("claude-code"))
        return AgentRuntimeIds::ClaudeStreamJson;
    return AgentRuntimeIds::ManagedRunner;
}

bool AgentRuntimeSession::sendCommand(const AgentRuntimeCommand &command)
{
    if (!command.isValid())
        return false;
    if (command.type == AgentRuntimeCommandTypes::InputSubmit) {
        const QJsonValue text = command.data.value(QStringLiteral("text"));
        return text.isString() && sendUserText(text.toString());
    }
    if (command.type == AgentRuntimeCommandTypes::SessionStop) {
        stop();
        return true;
    }
    return false;
}

bool AgentRuntimeDescriptor::isValid(QString *error) const
{
    if (!validName(id))
        return fail(error, QStringLiteral("runtime id must be a stable name"));
    if (displayName.trimmed().isEmpty())
        return fail(error, QStringLiteral("runtime display name is required"));
    if (protocolMinimum < 1 || protocolMaximum < protocolMinimum)
        return fail(error, QStringLiteral("invalid protocol range"));
    for (const QString &capability : capabilities) {
        if (!validName(capability))
            return fail(error, QStringLiteral("invalid capability: %1")
                                   .arg(capability));
    }
    return true;
}

bool AgentRuntimeDescriptor::supports(const QString &capability) const
{
    return capabilities.contains(capability);
}

QJsonObject AgentRuntimeDescriptor::toJson() const
{
    QStringList sorted(capabilities.cbegin(), capabilities.cend());
    std::sort(sorted.begin(), sorted.end());
    QJsonArray advertised;
    for (const QString &capability : sorted)
        advertised.append(capability);
    return {{QStringLiteral("id"), id},
            {QStringLiteral("displayName"), displayName},
            {QStringLiteral("protocolMinimum"), protocolMinimum},
            {QStringLiteral("protocolMaximum"), protocolMaximum},
            {QStringLiteral("capabilities"), advertised}};
}

bool AgentRuntimeDescriptor::fromJson(const QJsonObject &json,
                                      AgentRuntimeDescriptor *out,
                                      QString *error)
{
    if (!out)
        return fail(error, QStringLiteral("descriptor output is required"));
    AgentRuntimeDescriptor parsed;
    parsed.id = json.value(QStringLiteral("id")).toString();
    parsed.displayName = json.value(QStringLiteral("displayName")).toString();
    parsed.protocolMinimum =
        json.value(QStringLiteral("protocolMinimum")).toInt();
    parsed.protocolMaximum =
        json.value(QStringLiteral("protocolMaximum")).toInt();
    const QJsonValue capabilitiesValue =
        json.value(QStringLiteral("capabilities"));
    if (!capabilitiesValue.isArray())
        return fail(error, QStringLiteral("capabilities must be an array"));
    for (const QJsonValue &value : capabilitiesValue.toArray()) {
        if (!value.isString())
            return fail(error,
                        QStringLiteral("capabilities must contain strings"));
        parsed.capabilities.insert(value.toString());
    }
    if (!parsed.isValid(error))
        return false;
    *out = parsed;
    return true;
}

bool AgentRuntimeEvent::isValid(QString *error) const
{
    return validEnvelope(schemaVersion, type, sequence, error);
}

QJsonObject AgentRuntimeEvent::toJson() const
{
    QJsonObject json = envelopeJson(schemaVersion, type, requestId, sequence, data);
    if (!sessionId.isEmpty())
        json.insert(QStringLiteral("sessionId"), sessionId);
    return json;
}

bool AgentRuntimeEvent::fromJson(const QJsonObject &json, AgentRuntimeEvent *out,
                                 QString *error)
{
    if (!out)
        return fail(error, QStringLiteral("event output is required"));
    if (!json.value(QStringLiteral("data")).isObject())
        return fail(error, QStringLiteral("event data must be an object"));
    AgentRuntimeEvent parsed;
    parsed.schemaVersion = json.value(QStringLiteral("schemaVersion")).toInt();
    parsed.type = json.value(QStringLiteral("type")).toString();
    parsed.sessionId = json.value(QStringLiteral("sessionId")).toString();
    parsed.requestId = json.value(QStringLiteral("requestId")).toString();
    parsed.sequence = json.value(QStringLiteral("sequence")).toInteger(-1);
    parsed.data = json.value(QStringLiteral("data")).toObject();
    if (!parsed.isValid(error))
        return false;
    *out = parsed;
    return true;
}

bool AgentRuntimeCommand::isValid(QString *error) const
{
    return validEnvelope(schemaVersion, type, sequence, error);
}

QJsonObject AgentRuntimeCommand::toJson() const
{
    return envelopeJson(schemaVersion, type, requestId, sequence, data);
}

bool AgentRuntimeCommand::fromJson(const QJsonObject &json,
                                   AgentRuntimeCommand *out, QString *error)
{
    if (!out)
        return fail(error, QStringLiteral("command output is required"));
    if (!json.value(QStringLiteral("data")).isObject())
        return fail(error, QStringLiteral("command data must be an object"));
    AgentRuntimeCommand parsed;
    parsed.schemaVersion = json.value(QStringLiteral("schemaVersion")).toInt();
    parsed.type = json.value(QStringLiteral("type")).toString();
    parsed.requestId = json.value(QStringLiteral("requestId")).toString();
    parsed.sequence = json.value(QStringLiteral("sequence")).toInteger(-1);
    parsed.data = json.value(QStringLiteral("data")).toObject();
    if (!parsed.isValid(error))
        return false;
    *out = parsed;
    return true;
}

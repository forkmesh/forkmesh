#include "AgentHub.h"

#include "AgentIntake.h"

#include <QJsonValue>
#include <QStringList>

namespace forkmesh::agenthub {

namespace {

bool fail(QString *error, const char *code)
{
    if (error)
        *error = QString::fromLatin1(code);
    return false;
}

QString stringField(const QJsonObject &params, const char *key, bool *wrongType)
{
    const QJsonValue value = params.value(QLatin1String(key));
    if (value.isUndefined() || value.isNull())
        return QString();
    if (!value.isString()) {
        *wrongType = true;
        return QString();
    }
    return value.toString();
}

QString quoted(const QString &text)
{
    QStringList lines;
    for (const QString &line : text.split(QLatin1Char('\n')))
        lines << QStringLiteral("> %1").arg(line);
    return lines.join(QLatin1Char('\n'));
}

}

bool parseSuggestRequest(const QJsonObject &params, SuggestRequest *out, QString *error)
{
    bool wrongType = false;
    const QString repository = stringField(params, "repository", &wrongType).trimmed();
    const QString prompt = stringField(params, "prompt", &wrongType).trimmed();
    const QString title = stringField(params, "title", &wrongType).simplified();
    const QString provider = stringField(params, "provider", &wrongType).trimmed();
    const QString model = stringField(params, "model", &wrongType).trimmed();
    const QString reason = stringField(params, "reason", &wrongType).simplified();
    if (wrongType)
        return fail(error, "bad_request");
    SuggestRequest request;
    if (!repository.isEmpty() &&
        !agentintake::repositorySlugValid(repository, &request.owner, &request.name))
        return fail(error, "bad_repository");
    if (prompt.isEmpty() || prompt.size() > agentintake::kMaxPromptChars)
        return fail(error, "bad_task");
    if (title.size() > agentintake::kMaxTitleChars)
        return fail(error, "bad_title");
    if (!provider.isEmpty() && !agentintake::providerIdValid(provider))
        return fail(error, "bad_provider");
    if (!model.isEmpty() && !agentintake::modelIdValid(model))
        return fail(error, "bad_model");
    if (reason.size() > kMaxReasonChars)
        return fail(error, "bad_reason");
    request.prompt = prompt;
    request.title = title;
    request.provider = provider;
    request.model = model;
    request.reason = reason;
    if (out)
        *out = request;
    return true;
}

bool parseListRequest(const QJsonObject &params, ListRequest *out, QString *error)
{
    ListRequest request;
    const QJsonValue finished = params.value(QStringLiteral("include_finished"));
    if (!finished.isUndefined() && !finished.isNull()) {
        if (!finished.isBool())
            return fail(error, "bad_request");
        request.includeFinished = finished.toBool();
    }
    const QJsonValue limit = params.value(QStringLiteral("limit"));
    if (!limit.isUndefined() && !limit.isNull()) {
        const double value = limit.toDouble(-1);
        if (!limit.isDouble() || value != double(qint64(value)) || value < 1 ||
            value > kMaxListed)
            return fail(error, "bad_limit");
        request.limit = int(value);
    }
    if (out)
        *out = request;
    return true;
}

bool parseMessageRequest(const QJsonObject &params, MessageRequest *out, QString *error)
{
    const QJsonValue target = params.value(QStringLiteral("agent_id"));
    const double id = target.toDouble(-1);
    if (!target.isDouble() || id != double(qint64(id)) || id < 1 || id > 2147483647.0)
        return fail(error, "bad_agent");
    bool wrongType = false;
    const QString text = stringField(params, "message", &wrongType).trimmed();
    if (wrongType || text.isEmpty() || text.size() > kMaxMessageChars)
        return fail(error, "bad_message");
    if (out) {
        out->target = int(id);
        out->text = text;
    }
    return true;
}

bool parseNotifyRequest(const QJsonObject &params, NotifyRequest *out, QString *error)
{
    bool wrongType = false;
    const QString title = stringField(params, "title", &wrongType).simplified();
    const QString body = stringField(params, "body", &wrongType).trimmed();
    if (wrongType)
        return fail(error, "bad_request");
    if (title.isEmpty() || title.size() > kMaxNotifyTitleChars)
        return fail(error, "bad_title");
    if (body.size() > kMaxNotifyBodyChars)
        return fail(error, "bad_body");
    if (out) {
        out->title = title;
        out->body = body;
    }
    return true;
}

QString suggestionTitle(const SuggestRequest &request)
{
    if (!request.title.isEmpty())
        return request.title;
    const QString line = request.prompt.section(QLatin1Char('\n'), 0, 0).simplified();
    return line.size() > 80 ? line.left(79) + QStringLiteral("…") : line;
}

QString suggestionBody(const SuggestRequest &request, const QString &slug,
                       const QString &fromLabel)
{
    QStringList parts;
    parts << QStringLiteral("### %1").arg(suggestionTitle(request));
    if (!request.reason.isEmpty())
        parts << request.reason;
    QString prompt = request.prompt;
    if (prompt.size() > kSuggestionPreviewChars)
        prompt = prompt.left(kSuggestionPreviewChars - 1) + QStringLiteral("…");
    parts << quoted(prompt);
    QStringList facts;
    if (!slug.isEmpty())
        facts << QStringLiteral("Repository: **%1**").arg(slug);
    if (!request.provider.isEmpty())
        facts << QStringLiteral("Agent: %1").arg(request.provider);
    if (!request.model.isEmpty())
        facts << QStringLiteral("Model: %1").arg(request.model);
    if (!fromLabel.isEmpty())
        facts << QStringLiteral("Suggested by %1").arg(fromLabel);
    if (!facts.isEmpty())
        parts << facts.join(QStringLiteral(" · "));
    return parts.join(QStringLiteral("\n\n"));
}

int permissionRank(const QString &modeLabel, int unknownRank)
{
    const QString label = modeLabel.trimmed().toLower();
    if (label == QLatin1String("plan"))
        return 0;
    if (label == QLatin1String("ask") || label == QLatin1String("ask before edits") ||
        label == QLatin1String("default"))
        return 1;
    if (label == QLatin1String("edit") || label == QLatin1String("edit automatically") ||
        label == QLatin1String("acceptedits"))
        return 2;
    if (label == QLatin1String("auto") || label == QLatin1String("auto mode") ||
        label == QLatin1String("bypasspermissions"))
        return 3;
    return unknownRank;
}

bool messageAllowed(const QString &senderMode, const QString &targetMode)
{
    return permissionRank(targetMode, 3) <= permissionRank(senderMode, 0);
}

QString messageLead(int senderId, const QString &senderTitle)
{
    const QString title = senderTitle.simplified();
    return title.isEmpty()
               ? QStringLiteral("Message from ForkMesh agent #%1 (another agent, not the "
                                "user):")
                     .arg(senderId)
               : QStringLiteral("Message from ForkMesh agent #%1 “%2” (another agent, "
                                "not the user):")
                     .arg(senderId)
                     .arg(title.left(80));
}

QString messageEnvelope(int senderId, const QString &senderTitle, const QString &text)
{
    return messageLead(senderId, senderTitle) + QStringLiteral("\n\n") + text +
           QStringLiteral("\n\nReply with send_agent_message (agent_id %1) if it needs an "
                          "answer.")
               .arg(senderId);
}

}

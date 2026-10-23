#include "ClaudeControlProtocol.h"

#include <QRegularExpression>
#include <QSet>

namespace forkmesh::claudecontrol {

namespace {

constexpr int kDetailLimit = 600;
constexpr int kTitleLimit = 200;

const QString kRefusalFallback = QStringLiteral("refusal_fallback_prompt");
const QString kOverageConsent = QStringLiteral("fable_overage_consent_prompt");
const QString kAutoModeServerFallback = QStringLiteral("auto_mode_server_fallback");

QString plain(const QString &text, int limit = kDetailLimit)
{
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;?]*[ -/]*[@-~]"));
    static const QRegularExpression control(QStringLiteral("[\\x00-\\x08\\x0B-\\x1F\\x7F]"));
    QString out = text;
    out.remove(ansi);
    out.remove(control);
    out = out.trimmed();
    if (out.size() > limit)
        out = out.left(limit - 1).trimmed() + QChar(0x2026);
    return out;
}

QString str(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

QJsonObject option(const QString &label, const QString &description)
{
    return QJsonObject{{QStringLiteral("label"), label},
                       {QStringLiteral("description"), description}};
}

QJsonObject card(const QString &title, const QString &header, const QString &question,
                 const QJsonArray &options)
{
    return QJsonObject{
        {QStringLiteral("title"), title},
        {QStringLiteral("questions"),
         QJsonArray{QJsonObject{{QStringLiteral("question"), question},
                                {QStringLiteral("header"), header},
                                {QStringLiteral("options"), options},
                                {QStringLiteral("multiSelect"), false}}}}};
}

QString toolDetail(const QString &tool, const QJsonObject &input)
{
    if (tool == QLatin1String("Bash") || tool == QLatin1String("PowerShell"))
        return str(input, "command");
    if (tool == QLatin1String("Write") || tool == QLatin1String("Edit") ||
        tool == QLatin1String("MultiEdit") || tool == QLatin1String("Read") ||
        tool == QLatin1String("NotebookEdit"))
        return str(input, "file_path").isEmpty() ? str(input, "notebook_path")
                                                 : str(input, "file_path");
    if (tool == QLatin1String("WebFetch"))
        return str(input, "url");
    if (tool == QLatin1String("WebSearch"))
        return str(input, "query");
    if (tool == QLatin1String("Glob") || tool == QLatin1String("Grep"))
        return str(input, "pattern");
    if (tool == QLatin1String("Task") || tool == QLatin1String("Agent"))
        return str(input, "description");
    return QString();
}

QString destinationText(const QString &destination)
{
    if (destination == QLatin1String("session"))
        return QStringLiteral("for this session");
    if (destination == QLatin1String("localSettings"))
        return QStringLiteral("in this project's local Claude settings");
    if (destination == QLatin1String("projectSettings"))
        return QStringLiteral("in this project's shared Claude settings");
    if (destination == QLatin1String("userSettings"))
        return QStringLiteral("in your Claude settings");
    if (destination == QLatin1String("cliArg"))
        return QStringLiteral("for this run");
    return QString();
}

QString modeText(const QString &mode)
{
    if (mode == QLatin1String("acceptEdits"))
        return QStringLiteral("accept edits without asking");
    if (mode == QLatin1String("plan"))
        return QStringLiteral("plan only");
    if (mode == QLatin1String("bypassPermissions"))
        return QStringLiteral("run without asking");
    if (mode == QLatin1String("default"))
        return QStringLiteral("ask before changes");
    return mode;
}

QString suggestionText(const QJsonArray &suggestions)
{
    QStringList parts;
    for (const QJsonValue &value : suggestions) {
        const QJsonObject s = value.toObject();
        const QString type = str(s, "type");
        const QString where = destinationText(str(s, "destination"));
        QString part;
        if (type == QLatin1String("setMode")) {
            part = QStringLiteral("switch this session to %1").arg(modeText(str(s, "mode")));
        } else if (type == QLatin1String("addRules") ||
                   type == QLatin1String("replaceRules")) {
            QStringList rules;
            for (const QJsonValue &rv : s.value(QStringLiteral("rules")).toArray()) {
                const QJsonObject rule = rv.toObject();
                const QString content = str(rule, "ruleContent");
                rules << (content.isEmpty() ? str(rule, "toolName")
                                            : QStringLiteral("%1(%2)").arg(
                                                  str(rule, "toolName"), content));
            }
            const QString behavior = str(s, "behavior");
            part = QStringLiteral("%1 %2")
                       .arg(behavior == QLatin1String("deny")
                                ? QStringLiteral("deny")
                                : (behavior == QLatin1String("ask") ? QStringLiteral("ask for")
                                                                     : QStringLiteral("allow")),
                            rules.join(QStringLiteral(", ")));
        } else if (type == QLatin1String("addDirectories")) {
            QStringList dirs;
            for (const QJsonValue &dv : s.value(QStringLiteral("directories")).toArray())
                dirs << dv.toString();
            part = QStringLiteral("allow access to %1").arg(dirs.join(QStringLiteral(", ")));
        }
        if (part.trimmed().isEmpty())
            continue;
        if (!where.isEmpty() && type != QLatin1String("setMode"))
            part += QLatin1Char(' ') + where;
        parts << part;
    }
    QString text = parts.join(QStringLiteral("; "));
    if (!text.isEmpty())
        text[0] = text.at(0).toUpper();
    return plain(text, 300);
}

QString sessionModeFrom(const QJsonArray &permissions)
{
    QString mode;
    for (const QJsonValue &value : permissions) {
        const QJsonObject p = value.toObject();
        if (str(p, "type") == QLatin1String("setMode"))
            mode = str(p, "mode");
    }
    return mode;
}

QJsonObject allow(const QJsonObject &request, const QJsonObject &input)
{
    QJsonObject response{{QStringLiteral("behavior"), QStringLiteral("allow")},
                         {QStringLiteral("updatedInput"), input}};
    const QString toolUseId = str(request, "tool_use_id");
    if (!toolUseId.isEmpty())
        response.insert(QStringLiteral("toolUseID"), toolUseId);
    return response;
}

QJsonObject deny(const QJsonObject &request, const QString &message)
{
    QJsonObject response{{QStringLiteral("behavior"), QStringLiteral("deny")},
                         {QStringLiteral("message"), message}};
    const QString toolUseId = str(request, "tool_use_id");
    if (!toolUseId.isEmpty())
        response.insert(QStringLiteral("toolUseID"), toolUseId);
    return response;
}

QJsonObject setModePermission(const QString &mode)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("setMode")},
                       {QStringLiteral("mode"), mode},
                       {QStringLiteral("destination"), QStringLiteral("session")}};
}

QJsonObject requestBody(const QJsonObject &event)
{
    return event.value(QStringLiteral("request")).toObject();
}

Prompt permissionPrompt(const QString &requestId, const QJsonObject &body)
{
    Prompt prompt;
    prompt.kind = PromptKind::Permission;
    prompt.requestId = requestId;
    prompt.key = QStringLiteral("claude-approval-%1").arg(requestId);
    prompt.toolName = str(body, "tool_name");
    const QString shown = plain(str(body, "display_name").isEmpty()
                                    ? prompt.toolName
                                    : str(body, "display_name"),
                                80);
    const QJsonObject input = body.value(QStringLiteral("input")).toObject();
    QStringList lines;
    const QString title = plain(str(body, "title"), 300);
    lines << (title.isEmpty() ? QStringLiteral("Allow %1?").arg(shown) : title);
    QString detail = plain(toolDetail(prompt.toolName, input));
    if (detail.isEmpty())
        detail = plain(str(body, "description"));
    if (!detail.isEmpty())
        lines << detail;
    const QString blocked = plain(str(body, "blocked_path"), 300);
    if (!blocked.isEmpty())
        lines << QStringLiteral("Outside the working folders: %1").arg(blocked);
    const QString reason = plain(str(body, "decision_reason"), 400);
    if (!reason.isEmpty())
        lines << QStringLiteral("Why it asks: %1").arg(reason);

    QJsonArray options{option(kAllowOnce, QStringLiteral("Run this call only"))};
    const QJsonArray suggestions = body.value(QStringLiteral("permission_suggestions")).toArray();
    const QString remember = suggestionText(suggestions);
    if (!remember.isEmpty())
        options.append(option(kAllowAndRemember, remember));
    options.append(option(kDeny, QStringLiteral(
                                     "Refuse it. Type below instead to tell Claude what "
                                     "to do differently")));
    prompt.card = card(QStringLiteral("Claude Code asks to use %1").arg(shown),
                       QStringLiteral("Permission"), lines.join(QLatin1Char('\n')), options);
    return prompt;
}

Prompt planPrompt(const QString &requestId, const QJsonObject &body)
{
    Prompt prompt;
    prompt.kind = PromptKind::Plan;
    prompt.requestId = requestId;
    prompt.key = QStringLiteral("claude-approval-%1").arg(requestId);
    prompt.toolName = str(body, "tool_name");
    prompt.card = card(
        QStringLiteral("Claude Code finished planning"), QStringLiteral("Plan"),
        QStringLiteral("Approve the plan above and let Claude start working?"),
        QJsonArray{option(kApproveAcceptEdits,
                          QStringLiteral("Start, and apply file edits without asking")),
                   option(kApproveAskEdits,
                          QStringLiteral("Start, and ask before each change")),
                   option(kKeepPlanning,
                          QStringLiteral("Stay in plan mode. Type below to say what "
                                         "to change"))});
    return prompt;
}

QString money(double cents, const QString &currency)
{
    const QString code = currency.trimmed().isEmpty() ? QStringLiteral("USD") : currency;
    return QStringLiteral("%1 %2").arg(cents / 100.0, 0, 'f', 2).arg(code.toUpper());
}

Prompt dialogPrompt(const QString &requestId, const QJsonObject &body)
{
    Prompt prompt;
    prompt.requestId = requestId;
    prompt.key = QStringLiteral("claude-input-%1").arg(requestId);
    const QString kind = str(body, "dialog_kind");
    const QJsonObject payload = body.value(QStringLiteral("payload")).toObject();
    if (!supportedDialogKinds().contains(kind)) {
        prompt.kind = PromptKind::Unanswerable;
        return prompt;
    }
    prompt.kind = PromptKind::Dialog;
    if (kind == kRefusalFallback) {
        const QString original = plain(str(payload, "originalModel"), 80);
        const QString fallback = plain(str(payload, "fallbackModel"), 80);
        QStringList lines;
        lines << QStringLiteral("%1 declined this request.")
                     .arg(original.isEmpty() ? QStringLiteral("The model") : original);
        const QString guidance = plain(str(payload, "guidanceText"));
        if (!guidance.isEmpty())
            lines << guidance;
        lines << QStringLiteral("Retry it on %1, or change the prompt?")
                     .arg(fallback.isEmpty() ? QStringLiteral("the fallback model")
                                             : fallback);
        prompt.card = card(
            QStringLiteral("Claude Code needs a decision"), QStringLiteral("Refusal"),
            lines.join(QLatin1Char('\n')),
            QJsonArray{option(QStringLiteral("Retry on %1").arg(
                                  fallback.isEmpty() ? QStringLiteral("the fallback model")
                                                     : fallback),
                              QStringLiteral("Send the same request to the other model")),
                       option(QStringLiteral("Edit my prompt"),
                              QStringLiteral("Drop this request. Type below to send a "
                                             "new one"))});
    } else if (kind == kOverageConsent) {
        QString model = plain(str(payload, "modelName"), 80);
        if (model.isEmpty())
            model = plain(str(payload, "model"), 80);
        QStringList lines;
        lines << QStringLiteral("%1 needs extra usage to continue.")
                     .arg(model.isEmpty() ? QStringLiteral("This model") : model);
        const QJsonValue balance = payload.value(QStringLiteral("balanceCents"));
        if (balance.isDouble())
            lines << QStringLiteral("Extra usage balance: %1")
                         .arg(money(balance.toDouble(), str(payload, "currency")));
        if (!payload.value(QStringLiteral("overagesEnabled")).toBool())
            lines << QStringLiteral("Extra usage is off for this account.");
        prompt.card = card(
            QStringLiteral("Claude Code needs a decision"), QStringLiteral("Usage"),
            lines.join(QLatin1Char('\n')),
            QJsonArray{option(QStringLiteral("Use extra usage"),
                              QStringLiteral("Keep going on this model")),
                       option(QStringLiteral("Switch to the default model"),
                              QStringLiteral("Continue on the account's default model"))});
    } else if (kind == kAutoModeServerFallback) {
        QStringList lines;
        const QString title = plain(str(payload, "title"), 200);
        if (!title.isEmpty())
            lines << title;
        for (const QJsonValue &p : payload.value(QStringLiteral("paragraphs")).toArray()) {
            const QString text = plain(p.toString());
            if (!text.isEmpty())
                lines << text;
        }
        const QString help = plain(str(payload, "helpUrl"), 300);
        if (!help.isEmpty())
            lines << help;
        prompt.card = card(
            QStringLiteral("Claude Code needs a decision"), QStringLiteral("Auto mode"),
            lines.join(QLatin1Char('\n')),
            QJsonArray{option(QStringLiteral("Continue"),
                              QStringLiteral("Keep going in auto mode")),
                       option(QStringLiteral("Stop this turn"),
                              QStringLiteral("Interrupt the turn"))});
    }
    return prompt;
}

QString dialogResult(const QString &kind, const QString &answer)
{
    const QString a = answer.trimmed();
    if (kind == kRefusalFallback) {
        if (a.startsWith(QLatin1String("Retry on ")))
            return QStringLiteral("retry_fallback");
        if (a == QLatin1String("Edit my prompt"))
            return QStringLiteral("edit_prompt");
    } else if (kind == kOverageConsent) {
        if (a == QLatin1String("Use extra usage"))
            return QStringLiteral("consent");
        if (a == QLatin1String("Switch to the default model"))
            return QStringLiteral("switch_default");
    } else if (kind == kAutoModeServerFallback) {
        if (a == QLatin1String("Continue"))
            return QStringLiteral("continue");
        if (a == QLatin1String("Stop this turn"))
            return QStringLiteral("interrupt");
    }
    return QString();
}

QJsonObject questionAnswers(const QJsonObject &input, const QString &answer)
{
    const QJsonArray questions = input.value(QStringLiteral("questions")).toArray();
    QJsonObject answers;
    if (questions.size() <= 1) {
        const QString q = questions.isEmpty()
                              ? QString()
                              : questions.at(0).toObject().value(QStringLiteral("question")).toString();
        answers.insert(q, answer.trimmed());
        return answers;
    }
    const QString arrow = QStringLiteral(" → ");
    const QStringList lines = answer.split(QLatin1Char('\n'));
    for (int i = 0; i < questions.size(); ++i) {
        const QString q = questions.at(i).toObject().value(QStringLiteral("question")).toString();
        QString value;
        for (const QString &line : lines) {
            if (line.startsWith(q + arrow)) {
                value = line.mid(q.size() + arrow.size());
                break;
            }
        }
        if (value.isEmpty() && i < lines.size()) {
            value = lines.at(i);
            const int at = value.indexOf(arrow);
            if (at >= 0)
                value = value.mid(at + arrow.size());
        }
        answers.insert(q, value.trimmed());
    }
    return answers;
}

} // namespace

QStringList supportedDialogKinds()
{
    return {kRefusalFallback, kOverageConsent, kAutoModeServerFallback};
}

QJsonObject initializeRequest(const QString &title)
{
    QJsonArray kinds;
    for (const QString &kind : supportedDialogKinds())
        kinds.append(kind);
    QJsonObject body{{QStringLiteral("subtype"), QStringLiteral("initialize")},
                     {QStringLiteral("supportedDialogKinds"), kinds},
                     {QStringLiteral("agentProgressSummaries"), true},
                     {QStringLiteral("perTaskStopAffordance"), true}};
    const QString clean = cleanTitle(title);
    if (!clean.isEmpty())
        body.insert(QStringLiteral("title"), clean);
    return body;
}

QJsonObject request(const QString &requestId, const QJsonObject &body)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("control_request")},
                       {QStringLiteral("request_id"), requestId},
                       {QStringLiteral("request"), body}};
}

QJsonObject success(const QString &requestId, const QJsonObject &response)
{
    QJsonObject inner{{QStringLiteral("subtype"), QStringLiteral("success")},
                      {QStringLiteral("request_id"), requestId}};
    if (!response.isEmpty())
        inner.insert(QStringLiteral("response"), response);
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("control_response")},
                       {QStringLiteral("response"), inner}};
}

QJsonObject failure(const QString &requestId, const QString &message)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("control_response")},
        {QStringLiteral("response"),
         QJsonObject{{QStringLiteral("subtype"), QStringLiteral("error")},
                     {QStringLiteral("request_id"), requestId},
                     {QStringLiteral("error"), message}}}};
}

bool isControlRequest(const QJsonObject &event)
{
    return event.value(QStringLiteral("type")).toString() == QLatin1String("control_request");
}

bool isControlCancel(const QJsonObject &event)
{
    return event.value(QStringLiteral("type")).toString() ==
           QLatin1String("control_cancel_request");
}

QString requestIdOf(const QJsonObject &event)
{
    return event.value(QStringLiteral("request_id")).toString();
}

QString subtypeOf(const QJsonObject &event)
{
    return requestBody(event).value(QStringLiteral("subtype")).toString();
}

Prompt promptFor(const QJsonObject &event)
{
    Prompt prompt;
    if (!isControlRequest(event))
        return prompt;
    const QString requestId = requestIdOf(event);
    if (requestId.isEmpty())
        return prompt;
    const QJsonObject body = requestBody(event);
    const QString subtype = str(body, "subtype");
    if (subtype == QLatin1String("can_use_tool")) {
        const QString tool = str(body, "tool_name");
        if (tool == QLatin1String("AskUserQuestion")) {
            prompt.kind = PromptKind::Question;
            prompt.requestId = requestId;
            prompt.toolName = tool;
            prompt.key = str(body, "tool_use_id").isEmpty()
                             ? QStringLiteral("claude-input-%1").arg(requestId)
                             : str(body, "tool_use_id");
            if (str(body, "tool_use_id").isEmpty())
                prompt.card = body.value(QStringLiteral("input")).toObject();
            return prompt;
        }
        if (tool == QLatin1String("ExitPlanMode"))
            return planPrompt(requestId, body);
        return permissionPrompt(requestId, body);
    }
    if (subtype == QLatin1String("elicitation")) {
        prompt.kind = PromptKind::Elicitation;
        prompt.requestId = requestId;
        prompt.key = QStringLiteral("claude-input-%1").arg(requestId);
        return prompt;
    }
    if (subtype == QLatin1String("request_user_dialog"))
        return dialogPrompt(requestId, body);
    return prompt;
}

QJsonObject cardEvent(const Prompt &prompt)
{
    if (prompt.card.isEmpty() || prompt.key.isEmpty())
        return {};
    const QJsonObject block{{QStringLiteral("type"), QStringLiteral("tool_use")},
                            {QStringLiteral("id"), prompt.key},
                            {QStringLiteral("name"), QStringLiteral("AskUserQuestion")},
                            {QStringLiteral("input"), prompt.card}};
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("_forkmesh_control"), true},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                     {QStringLiteral("content"), QJsonArray{block}}}}};
}

Answer answerFor(const QJsonObject &event, const QString &answerText)
{
    Answer answer;
    const QString text = answerText.trimmed();
    if (text.isEmpty())
        return answer;
    const Prompt prompt = promptFor(event);
    const QJsonObject body = requestBody(event);
    const QJsonObject input = body.value(QStringLiteral("input")).toObject();
    switch (prompt.kind) {
    case PromptKind::Question: {
        QJsonObject updated = input;
        updated.insert(QStringLiteral("answers"), questionAnswers(input, text));
        answer.response = allow(body, updated);
        answer.valid = true;
        break;
    }
    case PromptKind::Permission:
        if (text == kAllowOnce) {
            answer.response = allow(body, input);
        } else if (text == kAllowAndRemember) {
            const QJsonArray suggestions =
                body.value(QStringLiteral("permission_suggestions")).toArray();
            answer.response = allow(body, input);
            answer.response.insert(QStringLiteral("updatedPermissions"), suggestions);
            answer.permissionMode = sessionModeFrom(suggestions);
        } else if (text == kDeny) {
            answer.response = deny(body, QStringLiteral("The user denied this tool call."));
        } else {
            answer.response = deny(
                body, QStringLiteral("The user denied this tool call and said: %1").arg(text));
        }
        answer.valid = true;
        break;
    case PromptKind::Plan:
        if (text == kApproveAcceptEdits || text == kApproveAskEdits) {
            const QString mode = text == kApproveAcceptEdits ? QStringLiteral("acceptEdits")
                                                             : QStringLiteral("default");
            answer.response = allow(body, input);
            answer.response.insert(QStringLiteral("updatedPermissions"),
                                   QJsonArray{setModePermission(mode)});
            answer.permissionMode = mode;
        } else if (text == kKeepPlanning) {
            answer.response = deny(body, QStringLiteral(
                                             "The user wants to keep planning before any "
                                             "changes are made."));
        } else {
            answer.response = deny(
                body,
                QStringLiteral("The user did not approve the plan yet and said: %1").arg(text));
        }
        answer.valid = true;
        break;
    case PromptKind::Dialog: {
        const QString kind = str(body, "dialog_kind");
        const QString result = dialogResult(kind, text);
        if (!result.isEmpty()) {
            answer.response = QJsonObject{{QStringLiteral("behavior"), QStringLiteral("completed")},
                                          {QStringLiteral("result"), result}};
        } else if (kind == kRefusalFallback) {
            answer.response = QJsonObject{{QStringLiteral("behavior"), QStringLiteral("completed")},
                                          {QStringLiteral("result"), QStringLiteral("edit_prompt")}};
            answer.followUp = text;
        } else {
            answer.response =
                QJsonObject{{QStringLiteral("behavior"), QStringLiteral("cancelled")}};
            answer.followUp = text;
        }
        answer.valid = true;
        break;
    }
    case PromptKind::Elicitation:
    case PromptKind::None:
    case PromptKind::Unanswerable:
        break;
    }
    return answer;
}

QList<ElicitationField> elicitationFields(const QJsonObject &requestEvent)
{
    const QJsonObject body = requestBody(requestEvent).isEmpty() ? requestEvent
                                                                 : requestBody(requestEvent);
    const QJsonObject schema = body.value(QStringLiteral("requested_schema")).toObject();
    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    QSet<QString> required;
    for (const QJsonValue &value : schema.value(QStringLiteral("required")).toArray())
        required.insert(value.toString());
    QStringList order;
    for (const QJsonValue &value : body.value(QStringLiteral("property_order")).toArray())
        if (properties.contains(value.toString()) && !order.contains(value.toString()))
            order << value.toString();
    for (auto it = properties.begin(); it != properties.end(); ++it)
        if (!order.contains(it.key()))
            order << it.key();

    QList<ElicitationField> fields;
    for (const QString &name : std::as_const(order)) {
        const QJsonObject p = properties.value(name).toObject();
        ElicitationField field;
        field.name = name;
        field.label = plain(str(p, "title").isEmpty() ? name : str(p, "title"), 120);
        field.description = plain(str(p, "description"), 400);
        field.type = str(p, "type").isEmpty() ? QStringLiteral("string") : str(p, "type");
        field.required = required.contains(name);
        field.secret = str(p, "format") == QLatin1String("password");
        field.defaultValue = p.value(QStringLiteral("default"));
        const QJsonArray enumValues = p.value(QStringLiteral("enum")).toArray();
        const QJsonArray enumNames = p.value(QStringLiteral("enumNames")).toArray();
        for (int i = 0; i < enumValues.size(); ++i) {
            const QString value = enumValues.at(i).toVariant().toString();
            field.choices << value;
            field.choiceLabels << plain(enumNames.at(i).toString().isEmpty()
                                            ? value
                                            : enumNames.at(i).toString(),
                                        120);
        }
        for (const QJsonValue &one : p.value(QStringLiteral("oneOf")).toArray()) {
            const QJsonObject o = one.toObject();
            const QString value = o.value(QStringLiteral("const")).toVariant().toString();
            if (value.isEmpty())
                continue;
            field.choices << value;
            field.choiceLabels << plain(str(o, "title").isEmpty() ? value : str(o, "title"), 120);
        }
        fields << field;
    }
    return fields;
}

QJsonObject elicitationContent(const QList<ElicitationField> &fields,
                               const QMap<QString, QString> &values, QString *error)
{
    QJsonObject content;
    for (const ElicitationField &field : fields) {
        const QString raw = values.value(field.name).trimmed();
        if (raw.isEmpty()) {
            if (field.required) {
                if (error)
                    *error = QStringLiteral("%1 is required.").arg(field.label);
                return {};
            }
            continue;
        }
        if (field.type == QLatin1String("boolean")) {
            content.insert(field.name, raw == QLatin1String("true") ||
                                           raw.compare(QLatin1String("yes"),
                                                       Qt::CaseInsensitive) == 0);
        } else if (field.type == QLatin1String("integer")) {
            bool ok = false;
            const qlonglong n = raw.toLongLong(&ok);
            if (!ok) {
                if (error)
                    *error = QStringLiteral("%1 must be a whole number.").arg(field.label);
                return {};
            }
            content.insert(field.name, n);
        } else if (field.type == QLatin1String("number")) {
            bool ok = false;
            const double n = raw.toDouble(&ok);
            if (!ok) {
                if (error)
                    *error = QStringLiteral("%1 must be a number.").arg(field.label);
                return {};
            }
            content.insert(field.name, n);
        } else {
            content.insert(field.name, raw);
        }
    }
    if (error)
        error->clear();
    return content;
}

QJsonObject elicitationResponse(const QString &action, const QJsonObject &content)
{
    QJsonObject response{{QStringLiteral("action"), action}};
    if (action == QLatin1String("accept") && !content.isEmpty())
        response.insert(QStringLiteral("content"), content);
    return response;
}

QString cleanTitle(const QString &title)
{
    static const QRegularExpression spaces(
        QStringLiteral("[\\x00-\\x1F\\x7F-\\x9F\\x{200B}-\\x{200F}\\x{202A}-\\x{202E}"
                       "\\x{2066}-\\x{2069}\\x{FEFF}\\s]+"));
    QString clean = title;
    clean.replace(spaces, QStringLiteral(" "));
    clean = clean.trimmed();
    if (clean.size() > kTitleLimit)
        clean = clean.left(kTitleLimit).trimmed();
    return clean;
}

QString titleFromEvent(const QJsonObject &event)
{
    if (event.value(QStringLiteral("type")).toString() != QLatin1String("system") ||
        event.value(QStringLiteral("subtype")).toString() !=
            QLatin1String("session_title_changed"))
        return QString();
    return cleanTitle(event.value(QStringLiteral("title")).toString());
}

QString permissionModeLabel(const QString &modeId)
{
    if (modeId == QLatin1String("bypassPermissions"))
        return QStringLiteral("Auto");
    if (modeId == QLatin1String("plan"))
        return QStringLiteral("Plan");
    if (modeId == QLatin1String("acceptEdits"))
        return QStringLiteral("Edit");
    if (modeId == QLatin1String("default"))
        return QStringLiteral("Ask");
    return QString();
}

} // namespace forkmesh::claudecontrol

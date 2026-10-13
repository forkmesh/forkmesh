#include "AgentLogText.h"

#include "ClaudePeerMessaging.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

namespace {

QString promptLine(const QString &text)
{
    const QString t = text.trimmed();
    return t.isEmpty() ? QString() : QStringLiteral("\n> ") + t + QLatin1Char('\n');
}

QString replyLine(const QString &text)
{
    const QString t = text.trimmed();
    return t.isEmpty() ? QString() : QLatin1Char('\n') + t + QLatin1Char('\n');
}

bool injectedPrompt(const QString &text)
{
    static const QStringList prefixes = {
        QStringLiteral("<command-"),       QStringLiteral("<local-command-"),
        QStringLiteral("<bash-"),          QStringLiteral("<system-reminder>"),
        QStringLiteral("<user_instructions>"), QStringLiteral("<environment_context>"),
        QStringLiteral("Caveat: "),
    };
    const QString t = text.trimmed();
    for (const QString &prefix : prefixes) {
        if (t.startsWith(prefix))
            return true;
    }
    return false;
}

constexpr int kTaskResultLimit = 1500;

bool isTaskNotification(const QString &text)
{
    return text.trimmed().startsWith(QLatin1String("<task-notification>"));
}

QString unescapeEntities(QString text)
{
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&apos;"), QLatin1String("'"));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return text;
}

QString tagText(const QString &xml, const QString &tag)
{
    const QString open = QLatin1Char('<') + tag + QLatin1Char('>');
    const QString close = QStringLiteral("</") + tag + QLatin1Char('>');
    const qsizetype start = xml.indexOf(open);
    if (start < 0)
        return {};
    const qsizetype from = start + open.size();
    const qsizetype end = xml.indexOf(close, from);
    return unescapeEntities(end < 0 ? xml.mid(from) : xml.mid(from, end - from));
}

QString clippedTaskResult(const QString &raw)
{
    QString text = raw;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    for (qsizetype newline = text.indexOf(QLatin1Char('\n'));
         newline >= 0 && text.left(newline).trimmed().isEmpty();
         newline = text.indexOf(QLatin1Char('\n')))
        text.remove(0, newline + 1);
    while (!text.isEmpty() && text.back().isSpace())
        text.chop(1);
    if (text.size() <= kTaskResultLimit)
        return text;
    qsizetype cut = text.lastIndexOf(QLatin1Char('\n'), kTaskResultLimit);
    if (cut < kTaskResultLimit * 2 / 3) {
        cut = kTaskResultLimit;
        if (text.at(cut - 1).isHighSurrogate())
            --cut;
    }
    QString head = text.left(cut);
    while (!head.isEmpty() && head.back().isSpace())
        head.chop(1);
    int fences = 0;
    for (const QString &line : head.split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(QLatin1String("```")))
            ++fences;
    }
    if (fences % 2 == 1)
        head += QStringLiteral("\n```");
    return head + QStringLiteral("\n\n") + QString::fromUtf8("\xE2\x80\xA6") +
           QStringLiteral("more (%1 characters not shown)").arg(text.size() - cut);
}

QString taskStatus(const QString &xml)
{
    static const QRegularExpression word(QStringLiteral("^[a-z_]{1,20}$"));
    const QString status = tagText(xml, QStringLiteral("status")).trimmed().toLower();
    if (status.isEmpty() || status == QLatin1String("cost") || !word.match(status).hasMatch())
        return QStringLiteral("update");
    return status;
}

QString taskBlock(const QString &xml)
{
    QString summary = tagText(xml, QStringLiteral("summary")).simplified();
    if (summary.isEmpty())
        summary = QStringLiteral("Background task");
    QString body = tagText(xml, QStringLiteral("result"));
    if (body.trimmed().isEmpty())
        body = tagText(xml, QStringLiteral("event"));
    QString out = QStringLiteral("\n==> Task ") + taskStatus(xml) + QStringLiteral(": ") + summary +
                  QLatin1Char('\n');
    if (body.trimmed().isEmpty())
        return out;
    for (const QString &line : clippedTaskResult(body).split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty())
            out += QLatin1Char('\n');
        else
            out += QStringLiteral("    ") + line + QLatin1Char('\n');
    }
    return out;
}

QString contentText(const QJsonValue &content)
{
    if (content.isString())
        return content.toString();
    QStringList texts;
    for (const QJsonValue &value : content.toArray()) {
        const QJsonObject block = value.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text") || type == QLatin1String("input_text"))
            texts.append(block.value(QStringLiteral("text")).toString());
    }
    return texts.join(QLatin1Char('\n'));
}

QString userContent(const QJsonValue &content)
{
    if (content.isString()) {
        const QString text = content.toString();
        if (isTaskNotification(text))
            return AgentLogText::taskNotification(text);
        return injectedPrompt(text) ? QString() : promptLine(text);
    }
    QStringList texts;
    QString tasks;
    for (const QJsonValue &value : content.toArray()) {
        const QJsonObject block = value.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type != QLatin1String("text") && type != QLatin1String("input_text"))
            continue;
        const QString text = block.value(QStringLiteral("text")).toString();
        if (isTaskNotification(text))
            tasks += AgentLogText::taskNotification(text);
        else if (!injectedPrompt(text))
            texts.append(text.trimmed());
    }
    texts.removeAll(QString());
    return promptLine(texts.join(QLatin1Char('\n'))) + tasks;
}

QString shellCommand(const QJsonValue &command)
{
    if (command.isString())
        return command.toString();
    QStringList parts;
    for (const QJsonValue &part : command.toArray())
        parts.append(part.toString());
    if (parts.size() == 3 && parts.at(1).startsWith(QLatin1Char('-')) &&
        parts.at(1).contains(QLatin1Char('c')))
        return parts.at(2);
    return parts.join(QLatin1Char(' '));
}

QString peerPrompt(const QJsonObject &entry)
{
    static const QRegularExpression envelope(
        QStringLiteral("<cross-session-message[^>]*>\\n([\\s\\S]*)\\n</cross-session-message>"));
    const QJsonObject origin = entry.value(QStringLiteral("origin")).toObject();
    QString body = origin.value(QStringLiteral("body")).toString();
    if (body.isEmpty()) {
        const QJsonValue content =
            entry.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
        const QRegularExpressionMatch match = envelope.match(content.toString());
        if (match.hasMatch())
            body = match.captured(1);
    }
    body = ClaudePeerMessaging::steerText(body.trimmed());
    if (body.isEmpty())
        return {};
    const QString name = origin.value(QStringLiteral("name")).toString().simplified();
    if (name.isEmpty())
        return promptLine(body);
    return promptLine(QString::fromUtf8("\xE2\x9F\xB5 ") + name + QStringLiteral(": ") + body);
}

} // namespace

namespace AgentLogText {

QString toolLine(const QString &name, const QJsonObject &input)
{
    QString arg = input.value(QStringLiteral("file_path")).toString();
    if (arg.isEmpty())
        arg = shellCommand(input.value(QStringLiteral("command")));
    if (arg.isEmpty())
        arg = input.value(QStringLiteral("path")).toString();
    if (arg.isEmpty())
        arg = input.value(QStringLiteral("pattern")).toString();
    if (arg.isEmpty() && !input.isEmpty())
        arg = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
    if (arg.size() > 200)
        arg = arg.left(200) + QString::fromUtf8("\xE2\x80\xA6");
    return QStringLiteral("\n[%1%2]\n")
        .arg(name, arg.isEmpty() ? QString() : QStringLiteral(" ") + arg);
}

QString assistantContent(const QJsonObject &message)
{
    QString log;
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text") || type == QLatin1String("output_text"))
            log += replyLine(block.value(QStringLiteral("text")).toString());
        else if (type == QLatin1String("tool_use"))
            log += toolLine(block.value(QStringLiteral("name")).toString(),
                            block.value(QStringLiteral("input")).toObject());
    }
    return log;
}

QString taskNotification(const QString &text)
{
    static const QRegularExpression block(
        QStringLiteral("<task-notification>([\\s\\S]*?)(?:</task-notification>|$)"));
    QString out;
    auto it = block.globalMatch(text);
    while (it.hasNext())
        out += taskBlock(it.next().captured(1));
    return out;
}

bool isTaskNotificationText(const QString &text)
{
    return isTaskNotification(text);
}

QString taskNotificationNotice(const QString &text)
{
    QStringList lines;
    for (const QString &line : taskNotification(text).split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("==> ")))
            lines << line.mid(4);
        else if (line.startsWith(QLatin1String("    ")))
            lines << line.mid(4);
        else
            lines << line;
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

bool isInjectedPrompt(const QString &text)
{
    return injectedPrompt(text) || isTaskNotification(text);
}

QString userEvent(const QJsonObject &event)
{
    if (event.value(QStringLiteral("isSidechain")).toBool())
        return {};
    const QJsonValue parent = event.value(QStringLiteral("parent_tool_use_id"));
    if (parent.isString() && !parent.toString().isEmpty())
        return {};
    const QString text =
        contentText(event.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")));
    return isTaskNotification(text) ? taskNotification(text) : QString();
}

QString claudeEntry(const QJsonObject &entry)
{
    if (!entry.value(QStringLiteral("isSidechain")).toBool() &&
        entry.value(QStringLiteral("type")).toString() == QLatin1String("user") &&
        entry.value(QStringLiteral("origin")).toObject().value(QStringLiteral("kind")).toString() ==
            QLatin1String("peer"))
        return peerPrompt(entry);
    if (entry.value(QStringLiteral("isSidechain")).toBool() ||
        entry.value(QStringLiteral("isMeta")).toBool())
        return {};
    const QString type = entry.value(QStringLiteral("type")).toString();
    const QJsonObject message = entry.value(QStringLiteral("message")).toObject();
    if (type == QLatin1String("user"))
        return userContent(message.value(QStringLiteral("content")));
    if (type == QLatin1String("assistant"))
        return assistantContent(message);
    if (type == QLatin1String("system") &&
        entry.value(QStringLiteral("subtype")).toString() == QLatin1String("compact_boundary"))
        return QStringLiteral("\n==> Conversation compacted\n");
    return {};
}

QString codexEntry(const QJsonObject &entry)
{
    const bool wrapped = entry.contains(QStringLiteral("payload"));
    const QJsonObject item = wrapped ? entry.value(QStringLiteral("payload")).toObject() : entry;
    const QString outer = entry.value(QStringLiteral("type")).toString();
    const QString type = item.value(QStringLiteral("type")).toString();
    if (wrapped && outer == QLatin1String("event_msg")) {
        if (type == QLatin1String("user_message"))
            return injectedPrompt(item.value(QStringLiteral("message")).toString())
                       ? QString()
                       : promptLine(item.value(QStringLiteral("message")).toString());
        return {};
    }
    if (wrapped && outer != QLatin1String("response_item"))
        return {};
    if (type == QLatin1String("message")) {
        const QString role = item.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("assistant"))
            return assistantContent(item);
        if (role == QLatin1String("user") && !wrapped)
            return userContent(item.value(QStringLiteral("content")));
        return {};
    }
    if (type == QLatin1String("function_call")) {
        const QString arguments = item.value(QStringLiteral("arguments")).toString();
        const QJsonObject input = QJsonDocument::fromJson(arguments.toUtf8()).object();
        return toolLine(item.value(QStringLiteral("name")).toString(), input);
    }
    if (type == QLatin1String("local_shell_call")) {
        const QJsonObject action = item.value(QStringLiteral("action")).toObject();
        return toolLine(QStringLiteral("shell"),
                        QJsonObject{{QStringLiteral("command"),
                                     action.value(QStringLiteral("command"))}});
    }
    if (type == QLatin1String("custom_tool_call")) {
        const QString input = item.value(QStringLiteral("input")).toString();
        const QString name = item.value(QStringLiteral("name")).toString();
        QString first = input.section(QLatin1Char('\n'), 0, 0).trimmed();
        for (const QString &line : input.split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("*** ")) && !line.startsWith(QLatin1String("*** Begin"))) {
                first = line.mid(4).trimmed();
                break;
            }
        }
        return toolLine(name, first.isEmpty() ? QJsonObject{}
                                              : QJsonObject{{QStringLiteral("path"), first}});
    }
    return {};
}

QString outsideEntry(const QString &kind, const QJsonObject &entry)
{
    if (kind == QLatin1String("claude"))
        return claudeEntry(entry);
    if (kind == QLatin1String("codex"))
        return codexEntry(entry);
    return {};
}

bool readsTranscript(const QString &kind)
{
    return kind == QLatin1String("claude") || kind == QLatin1String("codex");
}

} // namespace AgentLogText

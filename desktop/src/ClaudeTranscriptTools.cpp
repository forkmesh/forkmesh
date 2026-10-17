#include "ClaudeTranscriptTools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace forkmesh::transcripttools {

namespace {

const QString kMcpPrefix = QStringLiteral("mcp__");

QString str(const QJsonObject &input, const char *key)
{
    return input.value(QLatin1String(key)).toString().trimmed();
}

QString firstLine(const QString &text, int limit = 160)
{
    QString line = text.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
    if (line.size() > limit)
        line = line.left(limit - 1) + QStringLiteral("…");
    return line;
}

QString humanTool(QString name)
{
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    return name.trimmed();
}

QString humanServer(QString server)
{
    if (server.startsWith(QLatin1String("claude_ai_")))
        server = QStringLiteral("claude.ai ") + server.mid(10);
    server.replace(QLatin1Char('_'), QLatin1Char(' '));
    return server.trimmed();
}

QString count(qint64 value)
{
    return QLocale().toString(value);
}

}

bool isSubagentTool(const QString &name)
{
    return name == QLatin1String("Task") || name == QLatin1String("Agent");
}

bool isMcpTool(const QString &name)
{
    return name.startsWith(kMcpPrefix) && name.indexOf(QStringLiteral("__"), 5) > 5;
}

QString mcpServer(const QString &name)
{
    if (!isMcpTool(name))
        return QString();
    return name.mid(5, name.indexOf(QStringLiteral("__"), 5) - 5);
}

QString mcpToolName(const QString &name)
{
    if (!isMcpTool(name))
        return QString();
    return name.mid(name.indexOf(QStringLiteral("__"), 5) + 2);
}

bool isWindowTool(const QString &name)
{
    if (mcpServer(name) != QLatin1String("forkmesh"))
        return false;
    const QString tool = mcpToolName(name);
    return tool == QLatin1String("show_window") || tool == QLatin1String("read_window") ||
           tool == QLatin1String("close_window") ||
           tool == QLatin1String("open_browser_window") ||
           tool == QLatin1String("read_browser_page");
}

bool isShowWindowTool(const QString &name)
{
    return isWindowTool(name) && mcpToolName(name) == QLatin1String("show_window");
}

QString displayName(const QString &name, const QJsonObject &input)
{
    if (isSubagentTool(name)) {
        const QString sub = str(input, "subagent_type");
        return sub.isEmpty() ? QStringLiteral("Agent") : sub;
    }
    if (isWindowTool(name)) {
        const QString tool = mcpToolName(name);
        if (tool == QLatin1String("show_window"))
            return QStringLiteral("Window");
        if (tool == QLatin1String("open_browser_window"))
            return QStringLiteral("Browser");
        if (tool == QLatin1String("read_browser_page"))
            return QStringLiteral("Read page");
        if (tool == QLatin1String("read_window"))
            return QStringLiteral("Read window");
        return QStringLiteral("Close window");
    }
    if (isMcpTool(name))
        return QStringLiteral("%1 · %2").arg(humanServer(mcpServer(name)),
                                             humanTool(mcpToolName(name)));
    static const QHash<QString, QString> names{
        {QStringLiteral("TodoWrite"), QStringLiteral("Update Todos")},
        {QStringLiteral("ExitPlanMode"), QStringLiteral("Plan")},
        {QStringLiteral("EnterPlanMode"), QStringLiteral("Enter plan mode")},
        {QStringLiteral("BashOutput"), QStringLiteral("Read output")},
        {QStringLiteral("TaskOutput"), QStringLiteral("Read output")},
        {QStringLiteral("KillShell"), QStringLiteral("Stop task")},
        {QStringLiteral("KillBash"), QStringLiteral("Stop task")},
        {QStringLiteral("TaskStop"), QStringLiteral("Stop task")},
        {QStringLiteral("ToolSearch"), QStringLiteral("Load tools")},
        {QStringLiteral("WebFetch"), QStringLiteral("Fetch")},
        {QStringLiteral("web_fetch"), QStringLiteral("Fetch")},
        {QStringLiteral("WebSearch"), QStringLiteral("Web search")},
        {QStringLiteral("web_search"), QStringLiteral("Web search")},
        {QStringLiteral("ScheduleWakeup"), QStringLiteral("Schedule wakeup")},
        {QStringLiteral("SendMessage"), QStringLiteral("Message")},
        {QStringLiteral("NotebookEdit"), QStringLiteral("Edit notebook")},
        {QStringLiteral("SlashCommand"), QStringLiteral("Command")},
        {QStringLiteral("TaskCreate"), QStringLiteral("New task")},
        {QStringLiteral("TaskUpdate"), QStringLiteral("Update task")},
        {QStringLiteral("TaskList"), QStringLiteral("List tasks")},
        {QStringLiteral("TaskGet"), QStringLiteral("Read task")},
        {QStringLiteral("CronCreate"), QStringLiteral("Schedule")},
        {QStringLiteral("CronDelete"), QStringLiteral("Unschedule")},
        {QStringLiteral("CronList"), QStringLiteral("List schedules")},
        {QStringLiteral("code_execution"), QStringLiteral("Run code")},
        {QStringLiteral("bash_code_execution"), QStringLiteral("Run code")},
    };
    return names.value(name, name);
}

QString subtitle(const QString &name, const QJsonObject &input)
{
    if (name == QLatin1String("Bash")) {
        QString text = input.value(QStringLiteral("description")).toString();
        if (input.value(QStringLiteral("run_in_background")).toBool())
            text = text.isEmpty() ? QStringLiteral("in background")
                                  : text + QStringLiteral(" · in background");
        return text;
    }
    if (name == QLatin1String("Read")) {
        const QString fp = input.value(QStringLiteral("file_path")).toString();
        const int off = input.value(QStringLiteral("offset")).toInt();
        const int lim = input.value(QStringLiteral("limit")).toInt();
        if (off > 0 && lim > 0)
            return QStringLiteral("%1:%2 (lines %2-%3)").arg(fp).arg(off).arg(off + lim - 1);
        const QString pages = str(input, "pages");
        if (!pages.isEmpty())
            return QStringLiteral("%1 (pages %2)").arg(fp, pages);
        return fp;
    }
    if (name == QLatin1String("Edit") || name == QLatin1String("Write") ||
        name == QLatin1String("MultiEdit"))
        return input.value(QStringLiteral("file_path")).toString();
    if (name == QLatin1String("NotebookEdit")) {
        const QString path = str(input, "notebook_path");
        return path.isEmpty() ? str(input, "file_path") : path;
    }
    if (isSubagentTool(name)) {
        QString text = input.value(QStringLiteral("description")).toString();
        if (input.value(QStringLiteral("run_in_background")).toBool())
            text = text.isEmpty() ? QStringLiteral("in background")
                                  : text + QStringLiteral(" · in background");
        return text;
    }
    if (name == QLatin1String("Grep")) {
        const QString pattern = input.value(QStringLiteral("pattern")).toString();
        const QString path = str(input, "path");
        const QString glob = str(input, "glob");
        QString text = pattern;
        if (!glob.isEmpty())
            text += QStringLiteral(" in %1").arg(glob);
        else if (!path.isEmpty())
            text += QStringLiteral(" in %1").arg(path);
        return text;
    }
    if (name == QLatin1String("Glob")) {
        const QString pattern = input.value(QStringLiteral("pattern")).toString();
        const QString path = str(input, "path");
        return path.isEmpty() ? pattern : QStringLiteral("%1 in %2").arg(pattern, path);
    }
    if (name == QLatin1String("WebSearch") || name == QLatin1String("web_search") ||
        name == QLatin1String("ToolSearch"))
        return input.value(QStringLiteral("query")).toString();
    if (name == QLatin1String("WebFetch") || name == QLatin1String("web_fetch"))
        return str(input, "url");
    if (name == QLatin1String("FileChange")) {
        QStringList paths;
        for (const QJsonValue &value : input.value(QStringLiteral("changes")).toArray()) {
            const QString path = value.toObject().value(QStringLiteral("path")).toString();
            if (!path.isEmpty())
                paths << path;
        }
        return paths.join(QStringLiteral(", "));
    }
    if (name == QLatin1String("BashOutput"))
        return str(input, "bash_id");
    if (name == QLatin1String("TaskOutput") || name == QLatin1String("TaskStop") ||
        name == QLatin1String("TaskGet"))
        return str(input, "task_id").isEmpty() ? str(input, "taskId") : str(input, "task_id");
    if (name == QLatin1String("KillShell") || name == QLatin1String("KillBash"))
        return str(input, "shell_id");
    if (name == QLatin1String("Skill")) {
        const QString skill = str(input, "skill");
        return skill.isEmpty() ? skill : QLatin1Char('/') + skill;
    }
    if (name == QLatin1String("SlashCommand"))
        return str(input, "command");
    if (name == QLatin1String("SendMessage")) {
        const QString to = str(input, "to");
        const QString summary = str(input, "summary");
        if (to.isEmpty())
            return summary;
        return summary.isEmpty() ? QStringLiteral("to %1").arg(to)
                                 : QStringLiteral("to %1 · %2").arg(to, summary);
    }
    if (name == QLatin1String("ScheduleWakeup")) {
        const int seconds = input.value(QStringLiteral("delaySeconds")).toInt();
        const QString reason = str(input, "reason");
        if (input.value(QStringLiteral("stop")).toBool())
            return QStringLiteral("stop the loop");
        const QString when = seconds >= 60 ? QStringLiteral("in %1 min").arg(seconds / 60)
                                           : QStringLiteral("in %1 s").arg(seconds);
        return reason.isEmpty() ? when : QStringLiteral("%1 · %2").arg(when, reason);
    }
    if (name == QLatin1String("Workflow")) {
        const QString named = str(input, "name");
        if (!named.isEmpty())
            return named;
        const QString path = str(input, "scriptPath");
        if (!path.isEmpty())
            return path.section(QLatin1Char('/'), -1);
        static const QRegularExpression metaName(
            QStringLiteral("name\\s*:\\s*['\"]([^'\"\\n]{1,80})['\"]"));
        return metaName.match(input.value(QStringLiteral("script")).toString()).captured(1);
    }
    if (name == QLatin1String("TaskCreate"))
        return str(input, "subject");
    if (name == QLatin1String("TaskUpdate")) {
        const QString id = str(input, "taskId");
        const QString status = str(input, "status");
        return status.isEmpty() ? id : QStringLiteral("%1 → %2").arg(id, status);
    }
    if (name == QLatin1String("CronCreate"))
        return str(input, "cron").isEmpty() ? str(input, "schedule") : str(input, "cron");
    if (isShowWindowTool(name))
        return str(input, "title");
    if (isWindowTool(name) && mcpToolName(name) == QLatin1String("open_browser_window"))
        return str(input, "url");
    if (isWindowTool(name))
        return str(input, "window_id");
    for (const char *key : {"description", "title", "name", "query", "pattern", "url",
                            "file_path", "path", "repository", "subject", "command"}) {
        const QString value = firstLine(str(input, key));
        if (!value.isEmpty())
            return value;
    }
    return QString();
}

bool bodyHidden(const QString &name)
{
    static const QStringList hidden{
        QStringLiteral("Read"),        QStringLiteral("Grep"),
        QStringLiteral("Glob"),        QStringLiteral("WebSearch"),
        QStringLiteral("web_search"),  QStringLiteral("web_fetch"),
        QStringLiteral("ToolSearch"),  QStringLiteral("BashOutput"),
        QStringLiteral("TaskOutput"),  QStringLiteral("KillShell"),
        QStringLiteral("KillBash"),    QStringLiteral("TaskStop"),
        QStringLiteral("EnterPlanMode"), QStringLiteral("ScheduleWakeup"),
        QStringLiteral("SlashCommand"), QStringLiteral("TaskList"),
        QStringLiteral("TaskGet"),     QStringLiteral("TaskUpdate"),
        QStringLiteral("Workflow"),    QStringLiteral("CronList"),
        QStringLiteral("CronDelete"),  QStringLiteral("ExitPlanMode"),
        QStringLiteral("Skill"),
    };
    return hidden.contains(name) || isWindowTool(name);
}

QString bodyMarkdown(const QString &name, const QJsonObject &input)
{
    if (name == QLatin1String("ExitPlanMode"))
        return input.value(QStringLiteral("plan")).toString().trimmed();
    if (name == QLatin1String("WebFetch") || name == QLatin1String("web_fetch"))
        return input.value(QStringLiteral("prompt")).toString().trimmed();
    if (name == QLatin1String("SendMessage"))
        return input.value(QStringLiteral("message")).toString().trimmed();
    if (name == QLatin1String("Skill"))
        return input.value(QStringLiteral("args")).toString().trimmed();
    if (name == QLatin1String("TaskCreate"))
        return input.value(QStringLiteral("description")).toString().trimmed();
    if (name == QLatin1String("CronCreate"))
        return input.value(QStringLiteral("prompt")).toString().trimmed();
    if (isShowWindowTool(name)) {
        const QString format = str(input, "format").toLower();
        const QString body = input.value(QStringLiteral("body")).toString().trimmed();
        if (format == QLatin1String("html"))
            return QString();
        if (format == QLatin1String("text"))
            return body.isEmpty() ? body : QStringLiteral("```\n%1\n```").arg(body);
        return body;
    }
    return QString();
}

bool summarizeResult(const QString &name)
{
    return name == QLatin1String("Read") || name == QLatin1String("Grep") ||
           name == QLatin1String("Glob") || name == QLatin1String("ToolSearch") ||
           name == QLatin1String("Skill");
}

bool suppressResult(const QString &name)
{
    return name == QLatin1String("TodoWrite") || name == QLatin1String("EnterPlanMode");
}

bool isFileEditTool(const QString &name)
{
    return name == QLatin1String("Edit") || name == QLatin1String("MultiEdit") ||
           name == QLatin1String("Write");
}

QVector<DiffLine> lineDiff(const QString &oldText, const QString &newText)
{
    const QStringList a = oldText.isEmpty() ? QStringList() : oldText.split(QLatin1Char('\n'));
    const QStringList b = newText.isEmpty() ? QStringList() : newText.split(QLatin1Char('\n'));
    int pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre])
        ++pre;
    int suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre &&
           a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
        ++suf;

    QVector<DiffLine> out;
    for (int i = 0; i < pre; ++i)
        out.append({QLatin1Char(' '), a[i]});
    const int n = int(a.size()) - pre - suf;
    const int m = int(b.size()) - pre - suf;
    if (n > 0 && m > 0 && qint64(n) * m <= 250000) {
        QVector<int> lcs((n + 1) * (m + 1), 0);
        const auto at = [&](int i, int j) -> int & { return lcs[i * (m + 1) + j]; };
        for (int i = n - 1; i >= 0; --i)
            for (int j = m - 1; j >= 0; --j)
                at(i, j) = a[pre + i] == b[pre + j]
                               ? at(i + 1, j + 1) + 1
                               : std::max(at(i + 1, j), at(i, j + 1));
        int i = 0, j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && a[pre + i] == b[pre + j]) {
                out.append({QLatin1Char(' '), a[pre + i]});
                ++i;
                ++j;
            } else if (i < n && (j >= m || at(i + 1, j) >= at(i, j + 1))) {
                out.append({QLatin1Char('-'), a[pre + i]});
                ++i;
            } else {
                out.append({QLatin1Char('+'), b[pre + j]});
                ++j;
            }
        }
    } else {
        for (int i = 0; i < n; ++i)
            out.append({QLatin1Char('-'), a[pre + i]});
        for (int j = 0; j < m; ++j)
            out.append({QLatin1Char('+'), b[pre + j]});
    }
    for (int i = int(a.size()) - suf; i < a.size(); ++i)
        out.append({QLatin1Char(' '), a[i]});
    return out;
}

EditStats editStats(const QString &name, const QJsonObject &input)
{
    EditStats stats;
    const auto addPair = [&stats](const QJsonObject &edit) {
        const QJsonValue oldValue = edit.value(QStringLiteral("old_string"));
        const QJsonValue newValue = edit.value(QStringLiteral("new_string"));
        if (!oldValue.isString() && !newValue.isString())
            return;
        stats.known = true;
        for (const DiffLine &line : lineDiff(oldValue.toString(), newValue.toString())) {
            if (line.kind == QLatin1Char('+'))
                ++stats.added;
            else if (line.kind == QLatin1Char('-'))
                ++stats.removed;
        }
    };
    if (name == QLatin1String("Edit")) {
        addPair(input);
    } else if (name == QLatin1String("MultiEdit")) {
        for (const QJsonValue &edit : input.value(QStringLiteral("edits")).toArray())
            addPair(edit.toObject());
    } else if (name == QLatin1String("Write")) {
        const QJsonValue content = input.value(QStringLiteral("content"));
        if (content.isString()) {
            stats.known = true;
            QString text = content.toString();
            if (text.endsWith(QLatin1Char('\n')))
                text.chop(1);
            stats.added = text.isEmpty() ? 0 : int(text.count(QLatin1Char('\n'))) + 1;
        }
    } else if (name == QLatin1String("FileChange")) {
        for (const QJsonValue &value : input.value(QStringLiteral("changes")).toArray()) {
            const QJsonObject change = value.toObject();
            const QJsonValue diff = change.value(QStringLiteral("diff"));
            if (!diff.isString())
                continue;
            stats.known = true;
            QString text = diff.toString();
            if (text.endsWith(QLatin1Char('\n')))
                text.chop(1);
            if (text.isEmpty())
                continue;
            const QStringList lines = text.split(QLatin1Char('\n'));
            const bool hunks = text.startsWith(QLatin1String("@@")) ||
                               text.contains(QLatin1String("\n@@"));
            const QString type = change.value(QStringLiteral("kind"))
                                     .toObject()
                                     .value(QStringLiteral("type"))
                                     .toString();
            if (!hunks && type == QLatin1String("add")) {
                stats.added += int(lines.size());
                continue;
            }
            if (!hunks && type == QLatin1String("delete")) {
                stats.removed += int(lines.size());
                continue;
            }
            for (const QString &line : lines) {
                if (line.startsWith(QLatin1String("+++ ")) ||
                    line.startsWith(QLatin1String("--- ")))
                    continue;
                if (line.startsWith(QLatin1Char('+')))
                    ++stats.added;
                else if (line.startsWith(QLatin1Char('-')))
                    ++stats.removed;
            }
        }
    }
    return stats;
}

QString toolResultText(const QJsonValue &content, int *imageCount)
{
    int images = 0;
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else {
        QStringList parts;
        QStringList tools;
        for (const QJsonValue &value : content.toArray()) {
            const QJsonObject block = value.toObject();
            const QString type = block.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("image"))
                ++images;
            else if (type == QLatin1String("tool_reference"))
                tools << block.value(QStringLiteral("tool_name")).toString();
            else if (block.contains(QStringLiteral("text")))
                parts << block.value(QStringLiteral("text")).toString();
        }
        tools.removeAll(QString());
        if (!tools.isEmpty())
            parts << QStringLiteral("Loaded %1").arg(tools.join(QStringLiteral(", ")));
        if (images > 0)
            parts << (images == 1 ? QStringLiteral("[image]")
                                  : QStringLiteral("[%1 images]").arg(images));
        text = parts.join(QLatin1Char('\n'));
    }
    if (imageCount)
        *imageCount = images;
    return text;
}

bool isServerToolResultBlock(const QString &blockType)
{
    return blockType != QLatin1String("tool_result") &&
           blockType.endsWith(QLatin1String("_tool_result"));
}

QString serverToolResultText(const QJsonObject &block, bool *isError)
{
    bool error = block.value(QStringLiteral("is_error")).toBool();
    const QJsonValue content = block.value(QStringLiteral("content"));
    QStringList lines;
    if (content.isString()) {
        lines << content.toString();
    } else if (content.isArray()) {
        for (const QJsonValue &value : content.toArray()) {
            const QJsonObject item = value.toObject();
            const QString title = item.value(QStringLiteral("title")).toString().trimmed();
            const QString url = item.value(QStringLiteral("url")).toString().trimmed();
            const QString text = item.value(QStringLiteral("text")).toString();
            if (!title.isEmpty() || !url.isEmpty())
                lines << (title.isEmpty() ? url
                                          : url.isEmpty() ? title
                                                          : QStringLiteral("%1 — %2").arg(title, url));
            else if (!text.isEmpty())
                lines << text;
        }
    } else if (content.isObject()) {
        const QJsonObject item = content.toObject();
        const QString code = item.value(QStringLiteral("error_code")).toString();
        if (!code.isEmpty()) {
            error = true;
            lines << QStringLiteral("error: %1").arg(code);
        }
        const QString url = item.value(QStringLiteral("url")).toString();
        if (!url.isEmpty())
            lines << url;
        const QString out = item.value(QStringLiteral("stdout")).toString();
        const QString err = item.value(QStringLiteral("stderr")).toString();
        if (!out.isEmpty())
            lines << out;
        if (!err.isEmpty())
            lines << err;
        if (item.contains(QStringLiteral("return_code")) &&
            item.value(QStringLiteral("return_code")).toInt() != 0)
            error = true;
    }
    if (isError)
        *isError = error;
    return lines.join(QLatin1Char('\n'));
}

QString parentToolUseId(const QJsonObject &event)
{
    const QJsonValue parent = event.value(QStringLiteral("parent_tool_use_id"));
    return parent.isString() ? parent.toString() : QString();
}

QString systemNotice(const QJsonObject &event)
{
    if (event.value(QStringLiteral("type")).toString() != QLatin1String("system"))
        return QString();
    const QString sub = event.value(QStringLiteral("subtype")).toString();
    if (sub == QLatin1String("compact_boundary")) {
        const QJsonObject meta = event.value(QStringLiteral("compact_metadata")).toObject();
        const QString trigger = meta.value(QStringLiteral("trigger")).toString();
        const qint64 before = qint64(meta.value(QStringLiteral("pre_tokens")).toDouble());
        QStringList details;
        if (!trigger.isEmpty())
            details << trigger;
        if (before > 0)
            details << QStringLiteral("%1 tokens before").arg(count(before));
        return details.isEmpty()
                   ? QStringLiteral("Conversation compacted")
                   : QStringLiteral("Conversation compacted (%1)")
                         .arg(details.join(QStringLiteral(", ")));
    }
    if (sub == QLatin1String("status")) {
        if (event.value(QStringLiteral("status")).toString() == QLatin1String("compacting"))
            return QStringLiteral("Compacting conversation…");
        return QString();
    }
    if (sub == QLatin1String("api_retry")) {
        const int attempt = event.value(QStringLiteral("attempt")).toInt();
        const int max = event.value(QStringLiteral("max_retries")).toInt();
        const int status = event.value(QStringLiteral("error_status")).toInt();
        const double delay = event.value(QStringLiteral("retry_delay_ms")).toDouble();
        QString text = status > 0
                           ? QStringLiteral("API request failed (%1); retrying").arg(status)
                           : QStringLiteral("API request failed; retrying");
        if (delay > 0)
            text += QStringLiteral(" in %1 s").arg(delay / 1000.0, 0, 'f', 1);
        if (attempt > 0)
            text += max > 0 ? QStringLiteral(" (attempt %1 of %2)").arg(attempt).arg(max)
                            : QStringLiteral(" (attempt %1)").arg(attempt);
        return text;
    }
    if (sub == QLatin1String("local_command_output")) {
        const QString content = event.value(QStringLiteral("content")).toString().trimmed();
        return content;
    }
    if (sub == QLatin1String("task_notification")) {
        const QString status = event.value(QStringLiteral("status")).toString().trimmed();
        const QString raw = event.value(QStringLiteral("summary")).toString();
        const QString summary = raw.contains(QLatin1Char('\n')) || raw.size() > 300
                                    ? plainSummary(reportSummary(agentReport(raw)), 300)
                                    : raw.simplified();
        if (summary.isEmpty() && status.isEmpty())
            return QString();
        return QStringLiteral("Background task %1%2")
            .arg(status.isEmpty() ? QStringLiteral("update") : status,
                 summary.isEmpty() ? QString() : QStringLiteral(": ") + summary);
    }
    return QString();
}

namespace {

QString unescapedTag(const QString &xml, const QString &tag)
{
    const QString open = QLatin1Char('<') + tag + QLatin1Char('>');
    const QString close = QStringLiteral("</") + tag + QLatin1Char('>');
    const qsizetype start = xml.indexOf(open);
    if (start < 0)
        return QString();
    const qsizetype from = start + open.size();
    const qsizetype end = xml.indexOf(close, from);
    QString text = end < 0 ? xml.mid(from) : xml.mid(from, end - from);
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&apos;"), QLatin1String("'"));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return text;
}

QString clipAtSentence(const QString &text, int limit)
{
    if (text.size() <= limit)
        return text;
    static const QRegularExpression end(QStringLiteral("[.!?](?=\\s)"));
    qsizetype cut = -1;
    auto it = end.globalMatch(text);
    while (it.hasNext()) {
        const qsizetype at = it.next().capturedStart();
        if (at >= limit)
            break;
        cut = at;
    }
    if (cut >= 8)
        return text.left(cut + 1);
    qsizetype space = text.lastIndexOf(QLatin1Char(' '), limit - 1);
    if (space < limit / 2)
        space = limit - 1;
    QString head = text.left(space);
    while (!head.isEmpty() && (head.back().isSpace() || head.back() == QLatin1Char(',')))
        head.chop(1);
    if (head.count(QLatin1Char('`')) % 2 == 1)
        head += QLatin1Char('`');
    if (head.count(QStringLiteral("**")) % 2 == 1)
        head += QStringLiteral("**");
    return head + QStringLiteral("…");
}

bool isProseParagraph(const QStringList &lines)
{
    const QString first = lines.value(0).trimmed();
    if (first.startsWith(QLatin1Char('#')) || first.startsWith(QLatin1Char('|')) ||
        first.startsWith(QLatin1String("```")) || first.startsWith(QLatin1String("---")) ||
        first.startsWith(QLatin1String("- ")) || first.startsWith(QLatin1String("* ")) ||
        first.startsWith(QLatin1Char('>')))
        return false;
    static const QRegularExpression numbered(QStringLiteral("^\\d+[.)]\\s"));
    if (numbered.match(first).hasMatch())
        return false;
    static const QRegularExpression boldHeading(QStringLiteral("^\\*\\*[^*]+\\*\\*:?$"));
    return !(lines.size() == 1 && boldHeading.match(first).hasMatch());
}

}

QVector<TaskNotification> taskNotifications(const QString &text)
{
    static const QRegularExpression block(
        QStringLiteral("<task-notification>([\\s\\S]*?)(?:</task-notification>|$)"));
    QVector<TaskNotification> out;
    auto it = block.globalMatch(text);
    while (it.hasNext()) {
        const QString xml = it.next().captured(1);
        TaskNotification n;
        n.toolUseId = unescapedTag(xml, QStringLiteral("tool-use-id")).trimmed();
        n.status = unescapedTag(xml, QStringLiteral("status")).trimmed().toLower();
        n.summary = unescapedTag(xml, QStringLiteral("summary")).simplified();
        n.result = unescapedTag(xml, QStringLiteral("result"));
        n.event = unescapedTag(xml, QStringLiteral("event"));
        out.append(n);
    }
    return out;
}

QString agentReport(const QString &resultText)
{
    QString text = resultText;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    static const QRegularExpression usage(QStringLiteral("<usage>[\\s\\S]*?(?:</usage>|$)"));
    text.remove(usage);
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
        lines.removeFirst();
    if (!lines.isEmpty() && lines.first().startsWith(QLatin1String("[Subagent hand-back]"))) {
        qsizetype header = 0;
        while (header < lines.size() &&
               !lines.at(header).trimmed().endsWith(QLatin1String("follows:")))
            ++header;
        if (header == lines.size())
            header = 0;
        lines = lines.mid(header + 1);
        for (QString &line : lines) {
            if (line.startsWith(QLatin1String("  ")))
                line.remove(0, 2);
        }
    }
    while (!lines.isEmpty() && (lines.first().trimmed().isEmpty() ||
                                lines.first().startsWith(QLatin1String("[harness:"))))
        lines.removeFirst();
    while (!lines.isEmpty() && (lines.last().trimmed().isEmpty() ||
                                lines.last().trimmed().startsWith(QLatin1String("agentId:"))))
        lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

bool isAgentLaunchResult(const QString &resultText)
{
    return resultText.trimmed().startsWith(QLatin1String("Async agent launched"));
}

QString reportSummary(const QString &report, int limit)
{
    QVector<QStringList> paragraphs;
    QStringList current;
    bool fenced = false;
    for (const QString &line : report.split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(QLatin1String("```")))
            fenced = !fenced;
        if (!fenced && line.trimmed().isEmpty()) {
            if (!current.isEmpty())
                paragraphs.append(current);
            current.clear();
            continue;
        }
        current.append(line);
    }
    if (!current.isEmpty())
        paragraphs.append(current);
    if (paragraphs.isEmpty())
        return QString();
    QStringList lead = paragraphs.first();
    for (const QStringList &paragraph : paragraphs) {
        if (isProseParagraph(paragraph)) {
            lead = paragraph;
            break;
        }
    }
    if (!isProseParagraph(lead)) {
        QStringList kept;
        int size = 0;
        for (const QString &line : lead) {
            if (size > 0 && size + line.size() > limit)
                break;
            kept.append(line);
            size += int(line.size()) + 1;
        }
        return kept.join(QLatin1Char('\n')).trimmed();
    }
    for (QString &line : lead)
        line = line.trimmed();
    return clipAtSentence(lead.join(QLatin1Char(' ')).simplified(), limit);
}

QString plainSummary(const QString &markdown, int limit)
{
    QString text = markdown;
    text.remove(QStringLiteral("**"));
    text.remove(QLatin1Char('`'));
    text = text.simplified();
    if (text.startsWith(QLatin1String("- ")) || text.startsWith(QLatin1String("* ")))
        text = text.mid(2);
    return clipAtSentence(text, limit);
}

QString resultTokenSummary(const QJsonObject &resultEvent)
{
    const qint64 total = qint64(resultEvent.value(QStringLiteral("total_tokens")).toDouble());
    if (total > 0)
        return QStringLiteral("%1 tokens").arg(count(total));
    const QJsonObject usage = resultEvent.value(QStringLiteral("usage")).toObject();
    const qint64 in = qint64(usage.value(QStringLiteral("input_tokens")).toDouble() +
                             usage.value(QStringLiteral("cache_creation_input_tokens")).toDouble() +
                             usage.value(QStringLiteral("cache_read_input_tokens")).toDouble());
    const qint64 out = qint64(usage.value(QStringLiteral("output_tokens")).toDouble());
    if (in <= 0 && out <= 0)
        return QString();
    return QStringLiteral("%1 in · %2 out tokens").arg(count(in), count(out));
}

WindowResult parseWindowResult(const QString &text)
{
    WindowResult result;
    QJsonDocument doc = QJsonDocument::fromJson(text.trimmed().toUtf8());
    if (!doc.isObject())
        doc = QJsonDocument::fromJson(text.trimmed().section(QLatin1Char('\n'), 0, 0).toUtf8());
    if (!doc.isObject())
        return result;
    const QJsonObject object = doc.object();
    result.windowId = object.value(QStringLiteral("window_id")).toString();
    result.browser = object.value(QStringLiteral("kind")).toString() == QLatin1String("browser");
    result.external = object.value(QStringLiteral("external")).toBool();
    result.url = object.value(QStringLiteral("url")).toString();
    result.pageTitle = object.value(QStringLiteral("page_title")).toString();
    result.page = object.value(QStringLiteral("kind")).toString() == QLatin1String("page");
    if (result.page) {
        result.pageTitle = object.value(QStringLiteral("title")).toString();
        result.totalChars = qint64(object.value(QStringLiteral("total_chars")).toDouble());
        result.screenshot = object.value(QStringLiteral("has_screenshot")).toBool();
        for (const QJsonValue &message : object.value(QStringLiteral("console")).toArray())
            if (message.toObject().value(QStringLiteral("level")).toString() ==
                QLatin1String("error"))
                ++result.consoleErrors;
    }
    if (result.windowId.isEmpty() && !result.external)
        return result;
    result.title = object.value(QStringLiteral("title")).toString();
    result.open = object.value(QStringLiteral("open")).toBool();
    result.answered = object.value(QStringLiteral("answered")).toBool();
    result.button = object.value(QStringLiteral("button")).toString();
    result.input = object.value(QStringLiteral("input")).toString();
    result.valid = true;
    return result;
}

QString windowResultSummary(const WindowResult &result)
{
    if (!result.valid)
        return QString();
    if (result.external)
        return QStringLiteral("Opened in the user's browser");
    if (result.page) {
        QString text = QStringLiteral("Read %1 characters").arg(count(result.totalChars));
        const QString page = firstLine(result.pageTitle.isEmpty() ? result.url
                                                                  : result.pageTitle,
                                       100);
        if (!page.isEmpty())
            text += QStringLiteral(" of %1").arg(page);
        if (result.consoleErrors > 0)
            text += QStringLiteral(" · %1 console error%2")
                        .arg(result.consoleErrors)
                        .arg(result.consoleErrors == 1 ? QString() : QStringLiteral("s"));
        if (result.screenshot)
            text += QStringLiteral(" · screenshot");
        return text;
    }
    if (result.browser) {
        const QString page = firstLine(result.pageTitle.isEmpty() ? result.url
                                                                  : result.pageTitle,
                                       100);
        const QString state = result.open ? QStringLiteral("Browser open")
                                          : QStringLiteral("Browser closed");
        return page.isEmpty() ? state : QStringLiteral("%1 · %2").arg(state, page);
    }
    if (result.answered) {
        QString text = result.button.isEmpty()
                           ? QStringLiteral("The user answered")
                           : QStringLiteral("The user chose “%1”").arg(result.button);
        const QString input = firstLine(result.input, 120);
        if (!input.isEmpty())
            text += QStringLiteral(": %1").arg(input);
        return text;
    }
    return result.open ? QStringLiteral("Window open") : QStringLiteral("Window closed");
}

qint64 OutputTokenCounter::add(const QJsonObject &message)
{
    const qint64 out = qint64(message.value(QStringLiteral("usage")).toObject()
                                  .value(QStringLiteral("output_tokens")).toDouble());
    const QString id = message.value(QStringLiteral("id")).toString();
    if (out <= 0)
        return 0;
    if (id.isEmpty())
        return out;
    const qint64 seen = m_seen.value(id, 0);
    if (out <= seen)
        return 0;
    if (m_seen.size() >= 512 && !m_seen.contains(id))
        m_seen.clear();
    m_seen.insert(id, out);
    return out - seen;
}

namespace {

struct ShellWord {
    QString text;
    bool quoted = false;
};

bool isShellAssignment(const QString &word)
{
    static const QRegularExpression assignment(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\[[^\\]]*\\])?\\+?="));
    return assignment.match(word).hasMatch();
}

bool isShellNumber(const QString &word)
{
    static const QRegularExpression number(
        QStringLiteral("^\\d+(\\.\\d+)?[smhd]?$"));
    return number.match(word).hasMatch();
}

qsizetype closingParen(const QString &text, qsizetype open)
{
    int depth = 0;
    for (qsizetype i = open; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\')) {
            ++i;
        } else if (c == QLatin1Char('\'')) {
            const qsizetype close = text.indexOf(QLatin1Char('\''), i + 1);
            if (close < 0)
                return -1;
            i = close;
        } else if (c == QLatin1Char('"')) {
            qsizetype j = i + 1;
            while (j < text.size() && text.at(j) != QLatin1Char('"'))
                j += text.at(j) == QLatin1Char('\\') ? 2 : 1;
            if (j >= text.size())
                return -1;
            i = j;
        } else if (c == QLatin1Char('(')) {
            ++depth;
        } else if (c == QLatin1Char(')')) {
            if (--depth == 0)
                return i;
        }
    }
    return -1;
}

qsizetype closingBacktick(const QString &text, qsizetype open)
{
    for (qsizetype i = open + 1; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\\'))
            ++i;
        else if (text.at(i) == QLatin1Char('`'))
            return i;
    }
    return -1;
}

void shellSegments(const QString &command, int depth,
                   QVector<QVector<ShellWord>> &segments)
{
    QVector<ShellWord> words;
    QString word;
    bool inWord = false;
    bool quotedWord = false;
    bool skipNext = false;
    bool casePattern = false;
    int caseDepth = 0;
    QStringList heredocs;
    const auto nested = [&](qsizetype from, qsizetype to) {
        if (depth < 4 && to > from)
            shellSegments(command.mid(from, to - from), depth + 1, segments);
    };
    const auto endWord = [&] {
        if (inWord) {
            if (!quotedWord && words.isEmpty() && word == QLatin1String("case"))
                ++caseDepth;
            else if (!quotedWord && words.isEmpty() && word == QLatin1String("esac")) {
                caseDepth = qMax(0, caseDepth - 1);
                casePattern = false;
            }
            if (!quotedWord && word == QLatin1String("in") && !words.isEmpty()
                && words.first().text == QLatin1String("case"))
                casePattern = true;
            if (skipNext)
                skipNext = false;
            else
                words.push_back({word, quotedWord});
        }
        word.clear();
        inWord = false;
        quotedWord = false;
    };
    const auto endSegment = [&] {
        endWord();
        if (!words.isEmpty())
            segments.push_back(words);
        words.clear();
        skipNext = false;
    };
    static const QString wordStops = QStringLiteral(";|&()<>");
    const qsizetype n = command.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = command.at(i);
        const QChar next = i + 1 < n ? command.at(i + 1) : QChar();
        if (c == QLatin1Char('\\') && i + 1 < n) {
            if (next != QLatin1Char('\n')) {
                word += next;
                inWord = true;
            }
            ++i;
            continue;
        }
        if (c == QLatin1Char('\'')) {
            qsizetype close = command.indexOf(QLatin1Char('\''), i + 1);
            if (close < 0)
                close = n;
            quotedWord = quotedWord || !inWord;
            word += command.mid(i + 1, close - i - 1);
            inWord = true;
            i = close;
            continue;
        }
        if (c == QLatin1Char('"')) {
            qsizetype j = i + 1;
            while (j < n && command.at(j) != QLatin1Char('"')) {
                if (command.at(j) == QLatin1Char('\\')) {
                    j += 2;
                } else if (command.at(j) == QLatin1Char('$') && j + 1 < n
                           && command.at(j + 1) == QLatin1Char('(')) {
                    const qsizetype close = closingParen(command, j + 1);
                    if (close < 0) {
                        j = n;
                    } else {
                        if (j + 2 < n && command.at(j + 2) != QLatin1Char('('))
                            nested(j + 2, close);
                        j = close + 1;
                    }
                } else if (command.at(j) == QLatin1Char('`')) {
                    const qsizetype close = closingBacktick(command, j);
                    if (close < 0) {
                        j = n;
                    } else {
                        nested(j + 1, close);
                        j = close + 1;
                    }
                } else {
                    ++j;
                }
            }
            j = qMin(j, n);
            quotedWord = quotedWord || !inWord;
            word += command.mid(i + 1, j - i - 1);
            inWord = true;
            i = j;
            continue;
        }
        if ((c == QLatin1Char('$') || c == QLatin1Char('<') || c == QLatin1Char('>'))
            && next == QLatin1Char('(')) {
            if (c == QLatin1Char('$') && i + 2 < n && command.at(i + 2) == QLatin1Char('(')) {
                const qsizetype close = command.indexOf(QStringLiteral("))"), i + 3);
                i = close < 0 ? n : close + 1;
            } else {
                const qsizetype close = closingParen(command, i + 1);
                nested(i + 2, close < 0 ? n : close);
                i = close < 0 ? n : close;
            }
            word += QLatin1Char('$');
            inWord = true;
            continue;
        }
        if (c == QLatin1Char('`')) {
            const qsizetype close = closingBacktick(command, i);
            nested(i + 1, close < 0 ? n : close);
            i = close < 0 ? n : close;
            word += QLatin1Char('$');
            inWord = true;
            continue;
        }
        if (c == QLatin1Char('\n')) {
            endSegment();
            while (!heredocs.isEmpty() && i < n) {
                const QString delimiter = heredocs.takeFirst();
                while (i < n) {
                    qsizetype lineEnd = command.indexOf(QLatin1Char('\n'), i + 1);
                    if (lineEnd < 0)
                        lineEnd = n;
                    const bool closed =
                        command.mid(i + 1, lineEnd - i - 1).trimmed() == delimiter;
                    i = lineEnd;
                    if (closed)
                        break;
                }
            }
            heredocs.clear();
            continue;
        }
        if (c == QLatin1Char('<') || c == QLatin1Char('>')) {
            bool digits = inWord && !quotedWord && !word.isEmpty();
            for (const QChar d : std::as_const(word))
                digits = digits && d.isDigit();
            if (digits) {
                word.clear();
                inWord = false;
            } else {
                endWord();
            }
            if (c == QLatin1Char('<') && command.mid(i, 2) == QLatin1String("<<")
                && command.mid(i, 3) != QLatin1String("<<<")) {
                qsizetype j = i + 2;
                if (j < n && command.at(j) == QLatin1Char('-'))
                    ++j;
                while (j < n && command.at(j) == QLatin1Char(' '))
                    ++j;
                QString delimiter;
                while (j < n && !command.at(j).isSpace()
                       && !wordStops.contains(command.at(j))) {
                    const QChar d = command.at(j);
                    if (d != QLatin1Char('\'') && d != QLatin1Char('"')
                        && d != QLatin1Char('\\'))
                        delimiter += d;
                    ++j;
                }
                if (!delimiter.isEmpty())
                    heredocs << delimiter;
                i = j - 1;
                continue;
            }
            qsizetype j = i;
            while (j < n && (command.at(j) == QLatin1Char('<')
                             || command.at(j) == QLatin1Char('>')))
                ++j;
            bool target = true;
            if (j < n && command.at(j) == QLatin1Char('&')) {
                ++j;
                while (j < n && (command.at(j).isDigit()
                                 || command.at(j) == QLatin1Char('-'))) {
                    ++j;
                    target = false;
                }
            }
            i = j - 1;
            skipNext = target;
            continue;
        }
        if (c == QLatin1Char('(') && next == QLatin1Char('(')) {
            const qsizetype close = command.indexOf(QStringLiteral("))"), i + 2);
            i = close < 0 ? n : close + 1;
            inWord = true;
            continue;
        }
        if (casePattern && (c == QLatin1Char('|') || c == QLatin1Char(')'))) {
            if (c == QLatin1Char(')')) {
                words.clear();
                word.clear();
                inWord = false;
                quotedWord = false;
                skipNext = false;
                casePattern = false;
            } else {
                endWord();
            }
            continue;
        }
        if (c == QLatin1Char(';') && next == QLatin1Char(';') && caseDepth > 0) {
            endSegment();
            casePattern = true;
            ++i;
            continue;
        }
        if (c == QLatin1Char(';') || c == QLatin1Char('|') || c == QLatin1Char('&')
            || c == QLatin1Char('(') || c == QLatin1Char(')')) {
            endSegment();
            continue;
        }
        if (c.isSpace()) {
            endWord();
            continue;
        }
        if (c == QLatin1Char('#') && !inWord) {
            const qsizetype eol = command.indexOf(QLatin1Char('\n'), i);
            i = (eol < 0 ? n : eol) - 1;
            continue;
        }
        word += c;
        inWord = true;
    }
    endSegment();
}

QStringList commandProgramsAt(const QString &command, int depth);

QStringList segmentPrograms(const QVector<ShellWord> &words, int depth)
{
    static const QSet<QString> keywords = {
        QStringLiteral("if"),    QStringLiteral("then"),  QStringLiteral("else"),
        QStringLiteral("elif"),  QStringLiteral("fi"),    QStringLiteral("do"),
        QStringLiteral("done"),  QStringLiteral("while"), QStringLiteral("until"),
        QStringLiteral("!"),     QStringLiteral("{"),     QStringLiteral("}"),
        QStringLiteral("time"),  QStringLiteral("coproc"),
    };
    static const QSet<QString> notCommands = {
        QStringLiteral("for"),    QStringLiteral("case"),   QStringLiteral("esac"),
        QStringLiteral("select"), QStringLiteral("function"), QStringLiteral("in"),
    };
    static const QHash<QString, QSet<QString>> wrappers = {
        {QStringLiteral("sudo"),
         {QStringLiteral("-u"), QStringLiteral("-g"), QStringLiteral("-U"),
          QStringLiteral("-C"), QStringLiteral("-D"), QStringLiteral("-h"),
          QStringLiteral("-p"), QStringLiteral("-r"), QStringLiteral("-t")}},
        {QStringLiteral("doas"), {QStringLiteral("-u"), QStringLiteral("-C")}},
        {QStringLiteral("env"),
         {QStringLiteral("-u"), QStringLiteral("-C"), QStringLiteral("-S")}},
        {QStringLiteral("nohup"), {}},
        {QStringLiteral("nice"), {QStringLiteral("-n")}},
        {QStringLiteral("exec"), {QStringLiteral("-a")}},
        {QStringLiteral("command"), {}},
        {QStringLiteral("builtin"), {}},
        {QStringLiteral("setsid"), {}},
        {QStringLiteral("stdbuf"), {}},
        {QStringLiteral("unbuffer"), {}},
        {QStringLiteral("ionice"),
         {QStringLiteral("-c"), QStringLiteral("-n"), QStringLiteral("-p")}},
        {QStringLiteral("taskset"), {}},
        {QStringLiteral("timeout"),
         {QStringLiteral("-s"), QStringLiteral("-k"), QStringLiteral("--signal"),
          QStringLiteral("--kill-after")}},
        {QStringLiteral("xargs"),
         {QStringLiteral("-I"), QStringLiteral("-n"), QStringLiteral("-P"),
          QStringLiteral("-d"), QStringLiteral("-L"), QStringLiteral("-s"),
          QStringLiteral("-a"), QStringLiteral("-E")}},
    };
    static const QHash<QString, QSet<QString>> runners = {
        {QStringLiteral("systemd-run"),
         {QStringLiteral("-p"), QStringLiteral("--property"), QStringLiteral("-u"),
          QStringLiteral("--unit"), QStringLiteral("-E"), QStringLiteral("--setenv"),
          QStringLiteral("--slice"), QStringLiteral("-M"), QStringLiteral("--machine"),
          QStringLiteral("--uid"), QStringLiteral("--gid"), QStringLiteral("--nice"),
          QStringLiteral("--description"), QStringLiteral("--working-directory")}},
        {QStringLiteral("watch"), {QStringLiteral("-n"), QStringLiteral("--interval")}},
    };

    QStringList out;
    qsizetype i = 0;
    while (i < words.size()) {
        const ShellWord &w = words.at(i);
        if (!w.quoted && keywords.contains(w.text)) {
            ++i;
            continue;
        }
        if (!w.quoted && notCommands.contains(w.text))
            return out;
        if (!w.quoted && isShellAssignment(w.text)) {
            ++i;
            continue;
        }
        const auto wrapper = wrappers.constFind(w.text);
        if (w.quoted || wrapper == wrappers.constEnd())
            break;
        ++i;
        while (i < words.size()) {
            const QString &arg = words.at(i).text;
            if (wrapper.value().contains(arg)) {
                i += 2;
            } else if (arg.startsWith(QLatin1Char('-')) || isShellNumber(arg)
                       || (w.text == QLatin1String("env") && isShellAssignment(arg))) {
                ++i;
            } else {
                break;
            }
        }
    }
    if (i >= words.size())
        return out;

    const QString &raw = words.at(i).text;
    if (raw.isEmpty() || raw.startsWith(QLatin1Char('$'))
        || raw.startsWith(QLatin1Char('-')))
        return out;
    const QString program = raw.section(QLatin1Char('/'), -1);
    if (program.isEmpty())
        return out;

    if (program == QLatin1String("bash") || program == QLatin1String("sh")
        || program == QLatin1String("zsh") || program == QLatin1String("dash")) {
        for (qsizetype j = i + 1; j < words.size(); ++j) {
            const QString &arg = words.at(j).text;
            if (!arg.startsWith(QLatin1Char('-')))
                break;
            if (!arg.startsWith(QLatin1String("--")) && arg.contains(QLatin1Char('c'))
                && j + 1 < words.size() && depth < 3)
                return commandProgramsAt(words.at(j + 1).text, depth + 1);
        }
    }

    out << program;

    if (program.startsWith(QLatin1String("python"))) {
        for (qsizetype j = i + 1; j < words.size(); ++j) {
            const QString &arg = words.at(j).text;
            if (arg == QLatin1String("-m") && j + 1 < words.size()) {
                out << words.at(j + 1).text.section(QLatin1Char('.'), 0, 0);
                break;
            }
            if (!arg.startsWith(QLatin1Char('-')))
                break;
        }
    }

    const auto runner = runners.constFind(program);
    if (runner != runners.constEnd() && depth < 3) {
        qsizetype j = i + 1;
        while (j < words.size()) {
            const QString &arg = words.at(j).text;
            if (runner.value().contains(arg))
                j += 2;
            else if (arg.startsWith(QLatin1Char('-')))
                ++j;
            else
                break;
        }
        if (j + 1 == words.size() && words.at(j).text.contains(QLatin1Char(' ')))
            out << commandProgramsAt(words.at(j).text, depth + 1);
        else if (j < words.size())
            out << segmentPrograms(words.mid(j), depth + 1);
    }
    return out;
}

QStringList commandProgramsAt(const QString &command, int depth)
{
    QVector<QVector<ShellWord>> segments;
    shellSegments(command, 0, segments);
    QStringList out;
    for (const QVector<ShellWord> &segment : std::as_const(segments)) {
        for (const QString &program : segmentPrograms(segment, depth)) {
            if (!program.isEmpty() && !out.contains(program))
                out << program;
        }
    }
    return out;
}

QHash<QString, QString> commandIconTable()
{
    static const char *const rows[][2] = {
        {"search", "grep egrep fgrep zgrep rg ag ack"},
        {"read", "cat bat less more nl view strings xxd hexdump od zcat"},
        {"head", "head"},
        {"tail", "tail"},
        {"replace", "sed awk gawk mawk perl tr iconv envsubst"},
        {"cut", "cut paste column join split csplit"},
        {"count", "wc seq expr bc"},
        {"sort", "sort uniq comm shuf tac rev"},
        {"folder", "ls tree stat realpath readlink basename dirname file pwd exa eza"},
        {"find", "find fd locate"},
        {"cd", "cd pushd popd"},
        {"mkdir", "mkdir mktemp"},
        {"copy", "cp install"},
        {"move", "mv rename"},
        {"link", "ln"},
        {"delete", "rm rmdir unlink shred"},
        {"chmod", "chmod chown chgrp umask setfacl getfacl"},
        {"write", "touch tee truncate dd"},
        {"diff", "diff cmp patch colordiff delta"},
        {"echo", "echo printf yes"},
        {"git", "git tig"},
        {"gh", "gh hub glab"},
        {"python", "python python3 python2 pip pip3 uv poetry pipx ipython venv"},
        {"test", "pytest ctest tox jest vitest playwright py_compile pyflakes ruff mypy "
                 "flake8 shellcheck eslint"},
        {"node", "node deno bun tsc tsx"},
        {"npm", "npm npx pnpm yarn"},
        {"cmake", "cmake cpack"},
        {"ninja", "ninja"},
        {"build", "make gmake ccache ld strip ar ranlib objdump nm ldd pkg-config "
                  "meson autoreconf moc rcc"},
        {"cpp", "g++ clang++ c++"},
        {"c", "gcc clang cc"},
        {"dart", "dart"},
        {"swift", "swift swiftc"},
        {"xcode", "xcodebuild xcrun codesign pod plutil"},
        {"go", "go gofmt"},
        {"rust", "rustc rustup"},
        {"cargo", "cargo"},
        {"gradle", "gradle gradlew"},
        {"java", "java javac jar keytool"},
        {"kotlin", "kotlin kotlinc"},
        {"ruby", "ruby gem bundle rake"},
        {"docker", "docker podman docker-compose kubectl"},
        {"sqlite", "sqlite3"},
        {"database", "psql mysql redis-cli mongosh"},
        {"web", "curl wget http"},
        {"network", "ping ip ss netstat dig nslookup host traceroute nc ncat ufw "
                    "iptables nmap tcpdump resolvectl"},
        {"ssh", "ssh scp rsync sftp ssh-add ssh-keyscan mosh sshd"},
        {"key", "ssh-keygen openssl gpg age sha256sum sha1sum md5sum shasum b2sum"},
        {"service", "systemctl systemd-run service launchctl loginctl busctl gsettings "
                    "dconf"},
        {"logs", "journalctl dmesg logger"},
        {"process", "ps pgrep top htop pidof lsof fuser uptime nproc"},
        {"disk", "df du free vmstat iostat findmnt lsblk mount"},
        {"kill", "kill pkill killall"},
        {"clock", "sleep watch"},
        {"date", "date cal"},
        {"archive", "tar zip unzip gzip gunzip xz unxz bzip2 7z zstd"},
        {"package", "apt apt-get dpkg brew snap flatpak dnf yum pacman"},
        {"json", "jq yq base64"},
        {"image", "convert magick ffmpeg ffprobe pdftoppm gs grim scrot import identify"},
        {"phone", "adb fastboot"},
        {"claude", "claude"},
        {"ai", "codex ollama llama-cli llama-server"},
        {"cloud", "wrangler pywrangler vultr-cli aws gcloud doctl fly flyctl az"},
        {"open", "open xdg-open"},
        {"terminal", "bash sh zsh fish dash source . which type whereis man"},
    };
    QHash<QString, QString> table;
    for (const auto &row : rows) {
        const QString key = QString::fromLatin1(row[0]);
        for (const QString &program :
             QString::fromLatin1(row[1]).split(QLatin1Char(' '), Qt::SkipEmptyParts))
            table.insert(program, key);
    }
    return table;
}

}

QStringList commandPrograms(const QString &command)
{
    return commandProgramsAt(command, 0);
}

QString commandIconKey(const QString &program)
{
    static const QHash<QString, QString> table = commandIconTable();
    static const QSet<QString> silent = {
        QStringLiteral("true"),     QStringLiteral("false"),  QStringLiteral(":"),
        QStringLiteral("exit"),     QStringLiteral("return"), QStringLiteral("break"),
        QStringLiteral("continue"), QStringLiteral("shift"),  QStringLiteral("local"),
        QStringLiteral("declare"),  QStringLiteral("typeset"), QStringLiteral("readonly"),
        QStringLiteral("unset"),    QStringLiteral("trap"),   QStringLiteral("read"),
        QStringLiteral("let"),      QStringLiteral("test"),   QStringLiteral("["),
        QStringLiteral("[["),       QStringLiteral("]"),      QStringLiteral("]]"),
        QStringLiteral("shopt"),    QStringLiteral("alias"),  QStringLiteral("set"),
        QStringLiteral("export"),   QStringLiteral("wait"),   QStringLiteral("eval"),
        QStringLiteral("disown"),
    };
    const QString name = program.trimmed();
    if (name.isEmpty() || silent.contains(name))
        return QString();
    const auto hit = table.constFind(name);
    if (hit != table.constEnd())
        return hit.value();
    static const QRegularExpression testBinary(
        QStringLiteral("(^tests?$|^tests?[_-]|[_-]tests?$)"));
    if (testBinary.match(name).hasMatch())
        return QStringLiteral("test");
    if (name.endsWith(QLatin1String(".py")))
        return QStringLiteral("python");
    if (name.endsWith(QLatin1String(".js")) || name.endsWith(QLatin1String(".mjs")))
        return QStringLiteral("node");
    return QStringLiteral("terminal");
}

QString commandIconLabel(const QString &key)
{
    static const QHash<QString, QString> labels = [] {
        static const char *const rows[][2] = {
            {"search", "Search text"},          {"read", "Read a file"},
            {"head", "Start of a file"},        {"tail", "End of a file"},
            {"replace", "Transform text"},      {"cut", "Split columns"},
            {"count", "Count"},                 {"sort", "Sort lines"},
            {"folder", "List files"},           {"find", "Find files"},
            {"cd", "Change folder"},            {"mkdir", "Make a folder"},
            {"copy", "Copy files"},             {"move", "Move or rename"},
            {"link", "Make a link"},            {"delete", "Delete files"},
            {"chmod", "Permissions"},           {"write", "Write a file"},
            {"diff", "Compare files"},          {"echo", "Print text"},
            {"git", "Git"},                     {"gh", "GitHub CLI"},
            {"python", "Python"},               {"test", "Tests and checks"},
            {"node", "JavaScript runtime"},     {"npm", "JavaScript packages"},
            {"cmake", "CMake"},                 {"ninja", "Ninja build"},
            {"build", "Build tools"},           {"cpp", "C++ compiler"},
            {"c", "C compiler"},                {"dart", "Dart"},
            {"swift", "Swift"},                 {"xcode", "Xcode tools"},
            {"go", "Go"},                       {"rust", "Rust"},
            {"cargo", "Cargo"},                 {"gradle", "Gradle"},
            {"java", "Java"},                   {"kotlin", "Kotlin"},
            {"ruby", "Ruby"},                   {"docker", "Containers"},
            {"sqlite", "SQLite"},               {"database", "Database client"},
            {"web", "Web request"},             {"network", "Network"},
            {"ssh", "Remote shell"},            {"key", "Keys and checksums"},
            {"service", "Services"},            {"logs", "System logs"},
            {"process", "Processes"},           {"disk", "Disk and memory"},
            {"kill", "Stop a process"},         {"clock", "Wait"},
            {"date", "Date"},                   {"archive", "Archive"},
            {"package", "System packages"},     {"json", "JSON and encoding"},
            {"image", "Images and media"},      {"phone", "Android device"},
            {"claude", "Claude Code"},          {"ai", "AI model"},
            {"cloud", "Cloud CLI"},             {"open", "Open"},
            {"terminal", "Shell"},
        };
        QHash<QString, QString> h;
        for (const auto &row : rows)
            h.insert(QString::fromLatin1(row[0]), QString::fromLatin1(row[1]));
        return h;
    }();
    return labels.value(key, QStringLiteral("Shell"));
}

QVector<CommandIcon> commandIcons(const QString &command)
{
    QVector<CommandIcon> icons;
    for (const QString &program : commandPrograms(command)) {
        const QString key = commandIconKey(program);
        if (key.isEmpty())
            continue;
        auto it = std::find_if(icons.begin(), icons.end(),
                               [&](const CommandIcon &icon) { return icon.key == key; });
        if (it == icons.end())
            icons.push_back({key, {program}});
        else if (!it->programs.contains(program))
            it->programs << program;
    }
    return icons;
}

namespace {

void collectRawLogIds(const QJsonValue &value, QStringList &ids)
{
    static const QStringList kKeys{QStringLiteral("id"), QStringLiteral("tool_use_id"),
                                   QStringLiteral("call_id"), QStringLiteral("callId"),
                                   QStringLiteral("item_id"), QStringLiteral("itemId")};
    if (value.isArray()) {
        for (const QJsonValue &child : value.toArray())
            collectRawLogIds(child, ids);
        return;
    }
    if (!value.isObject())
        return;
    const QJsonObject object = value.toObject();
    for (const QString &key : kKeys) {
        const QString id = object.value(key).toString();
        if (id.size() >= 4 && !ids.contains(id))
            ids.append(id);
    }
    for (auto it = object.begin(); it != object.end(); ++it)
        if (it->isObject() || it->isArray())
            collectRawLogIds(*it, ids);
}

}

int rawLogLineStart(const QString &raw, const QJsonObject &event)
{
    if (raw.isEmpty() || event.isEmpty())
        return -1;
    qsizetype at = raw.indexOf(
        QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
    if (at < 0) {
        QStringList ids;
        const QString uuid = event.value(QStringLiteral("uuid")).toString();
        if (!uuid.isEmpty())
            ids.append(uuid);
        collectRawLogIds(event, ids);
        for (const QString &id : std::as_const(ids)) {
            at = raw.indexOf(QLatin1Char('"') + id + QLatin1Char('"'));
            if (at >= 0)
                break;
        }
    }
    if (at < 0)
        return -1;
    return int(raw.lastIndexOf(QLatin1Char('\n'), at) + 1);
}

}

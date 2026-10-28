#include "SessionInsights.h"

#include "ClaudeTranscriptTools.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace forkmesh::sessioninsights {

namespace tt = forkmesh::transcripttools;

namespace {

constexpr int kEntryLimit = 400;

QString text(const QJsonObject &input, const char *key)
{
    return input.value(QLatin1String(key)).toString().trimmed();
}

QString firstLine(const QString &value, int limit = 120)
{
    QString line = value.section(QLatin1Char('\n'), 0, 0).simplified();
    if (line.size() > limit)
        line = line.left(limit - 1) + QChar(0x2026);
    return line;
}

template <typename T>
QList<T> newestFirst(const QHash<QString, T> &items)
{
    QList<T> list = items.values();
    std::sort(list.begin(), list.end(),
              [](const T &a, const T &b) { return a.touched > b.touched; });
    return list;
}

template <typename T>
void trim(QHash<QString, T> &items)
{
    if (items.size() <= kEntryLimit)
        return;
    QList<T> list = newestFirst(items);
    for (int index = kEntryLimit; index < list.size(); ++index)
        items.remove(list.at(index).key);
}

template <>
void trim(QHash<QString, Background> &items)
{
    if (items.size() <= kEntryLimit)
        return;
    QList<Background> list = newestFirst(items);
    for (int index = kEntryLimit; index < list.size(); ++index)
        if (!list.at(index).running())
            items.remove(list.at(index).id);
}

QString contentText(const QJsonValue &content)
{
    if (content.isString())
        return content.toString();
    QStringList parts;
    for (const QJsonValue &value : content.toArray()) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            parts << block.value(QStringLiteral("text")).toString();
    }
    return parts.join(QLatin1Char('\n'));
}

}

QString fileName(const QString &path)
{
    QString clean = path.trimmed();
    while (clean.endsWith(QLatin1Char('/')) && clean.size() > 1)
        clean.chop(1);
    const qsizetype slash = clean.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? clean : clean.mid(slash + 1);
}

QString hostOf(const QString &url)
{
    const QUrl parsed(url.trimmed());
    QString host = parsed.host();
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    return host.isEmpty() ? url.trimmed() : host;
}

void Collector::clear()
{
    m_tasks.clear();
    m_outputs.clear();
    m_sources.clear();
    m_background.clear();
    m_tools.clear();
    m_pending.clear();
    m_shells.clear();
    m_clock = 0;
    m_calls = 0;
    m_errors = 0;
}

bool Collector::ingest(const QJsonObject &event)
{
    if (event.value(QStringLiteral("isSidechain")).toBool() ||
        event.value(QStringLiteral("isMeta")).toBool())
        return false;
    bool changed = m_tasks.ingest(event);
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("_local_user"))
        return applyNotifications(event.value(QStringLiteral("text")).toString()) || changed;
    if (type != QLatin1String("assistant") && type != QLatin1String("user"))
        return changed;
    const QJsonValue content =
        event.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    if (type == QLatin1String("user"))
        changed |= applyNotifications(contentText(content));
    for (const QJsonValue &value : content.toArray()) {
        const QJsonObject block = value.toObject();
        const QString blockType = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("assistant") &&
            (blockType == QLatin1String("tool_use") ||
             blockType == QLatin1String("server_tool_use"))) {
            changed |= applyToolUse(block.value(QStringLiteral("id")).toString(),
                                    block.value(QStringLiteral("name")).toString(),
                                    block.value(QStringLiteral("input")).toObject());
        } else if (type == QLatin1String("user") &&
                   blockType == QLatin1String("tool_result")) {
            changed |= applyToolResult(block.value(QStringLiteral("tool_use_id")).toString(),
                                       tt::toolResultText(block.value(QStringLiteral("content"))),
                                       block.value(QStringLiteral("is_error")).toBool());
        }
    }
    return changed;
}

void Collector::touchOutput(const QString &id, const QString &path, const QString &name,
                            int added, int removed, bool created)
{
    if (path.isEmpty())
        return;
    Entry &entry = m_outputs[path];
    if (entry.key.isEmpty()) {
        entry.key = path;
        entry.label = fileName(path);
        entry.created = created;
    }
    entry.detail = path;
    entry.icon = entry.created ? QStringLiteral("file") : QStringLiteral("pencil");
    ++entry.count;
    entry.added += added;
    entry.removed += removed;
    entry.touched = ++m_clock;
    if (!id.isEmpty())
        m_pending.insert(id + QLatin1Char('|') + path,
                         {name, QStringLiteral("outputs"), path, added, removed, false});
    trim(m_outputs);
}

void Collector::touchSource(const QString &key, const QString &label, const QString &detail,
                            const QString &icon)
{
    if (key.isEmpty())
        return;
    Entry &entry = m_sources[key];
    if (entry.key.isEmpty()) {
        entry.key = key;
        entry.label = label;
        entry.icon = icon;
    }
    entry.detail = detail;
    ++entry.count;
    entry.touched = ++m_clock;
    trim(m_sources);
}

bool Collector::applyToolUse(const QString &id, const QString &name, const QJsonObject &input)
{
    if (name.isEmpty())
        return false;
    ++m_calls;
    const QString shown = tt::displayName(name, input);
    ToolCount &count = m_tools[shown];
    count.name = shown;
    ++count.calls;
    if (!id.isEmpty())
        m_pending.insert(id, {shown, QString(), QString(), 0, 0, false});

    if (name == QLatin1String("Write") || name == QLatin1String("Edit") ||
        name == QLatin1String("MultiEdit")) {
        const tt::EditStats stats = tt::editStats(name, input);
        touchOutput(id, text(input, "file_path"), shown, stats.added, stats.removed,
                    name == QLatin1String("Write") &&
                        !m_outputs.contains(text(input, "file_path")));
        return true;
    }
    if (name == QLatin1String("NotebookEdit")) {
        QString path = text(input, "notebook_path");
        if (path.isEmpty())
            path = text(input, "file_path");
        touchOutput(id, path, shown, 0, 0, false);
        return true;
    }
    if (name == QLatin1String("FileChange")) {
        for (const QJsonValue &value : input.value(QStringLiteral("changes")).toArray()) {
            const QJsonObject change = value.toObject();
            const tt::EditStats stats = tt::editStats(
                name, QJsonObject{{QStringLiteral("changes"), QJsonArray{change}}});
            const QString kind = text(change, "kind").toLower();
            touchOutput(id, text(change, "path"), shown, stats.added, stats.removed,
                        kind == QLatin1String("add") || kind == QLatin1String("create"));
        }
        return true;
    }
    if (name == QLatin1String("Read")) {
        const QString path = text(input, "file_path");
        touchSource(path, fileName(path), path, QStringLiteral("file"));
        return true;
    }
    if (name == QLatin1String("WebFetch") || name == QLatin1String("web_fetch")) {
        const QString url = text(input, "url");
        touchSource(url, hostOf(url), url, QStringLiteral("link"));
        return true;
    }
    if (name == QLatin1String("WebSearch") || name == QLatin1String("web_search")) {
        const QString query = text(input, "query");
        touchSource(QStringLiteral("search:") + query, firstLine(query),
                    QStringLiteral("Web search: %1").arg(query), QStringLiteral("search"));
        return true;
    }
    if (name == QLatin1String("Skill")) {
        const QString skill = text(input, "skill");
        touchSource(QStringLiteral("skill:") + skill, QLatin1Char('/') + skill,
                    QStringLiteral("Skill"), QStringLiteral("sparkle"));
        return true;
    }
    if (tt::isMcpTool(name)) {
        const QString server = tt::mcpServer(name);
        const QString label = shown.section(QStringLiteral(" · "), 0, 0);
        touchSource(QStringLiteral("mcp:") + server, label.isEmpty() ? server : label,
                    QStringLiteral("Connector: %1").arg(server), QStringLiteral("package"));
        return true;
    }
    const bool subagent = tt::isSubagentTool(name);
    const bool shell = name == QLatin1String("Bash") &&
                       input.value(QStringLiteral("run_in_background")).toBool();
    if ((subagent || shell) && !id.isEmpty()) {
        Background item;
        item.id = id;
        item.kind = subagent ? QStringLiteral("agent") : QStringLiteral("shell");
        item.label = text(input, "description");
        if (item.label.isEmpty())
            item.label = subagent ? text(input, "subagent_type")
                                  : firstLine(text(input, "command"), 80);
        if (item.label.isEmpty())
            item.label = subagent ? QStringLiteral("Subagent") : QStringLiteral("Shell");
        item.status = QStringLiteral("running");
        item.touched = ++m_clock;
        m_background.insert(id, item);
        m_pending[id].background = input.value(QStringLiteral("run_in_background")).toBool();
        trim(m_background);
        return true;
    }
    if (name == QLatin1String("KillShell") || name == QLatin1String("KillBash") ||
        name == QLatin1String("TaskStop")) {
        QString target = text(input, "shell_id");
        if (target.isEmpty())
            target = text(input, "task_id");
        if (target.isEmpty())
            target = text(input, "taskId");
        const QString owner = m_shells.value(target, target);
        auto it = m_background.find(owner);
        if (it != m_background.end() && it->running()) {
            it->status = QStringLiteral("stopped");
            it->touched = ++m_clock;
        }
        return true;
    }
    return true;
}

bool Collector::applyToolResult(const QString &id, const QString &resultText, bool isError)
{
    const auto pending = m_pending.constFind(id);
    if (pending == m_pending.constEnd())
        return false;
    const Pending call = pending.value();
    m_pending.erase(pending);
    if (isError) {
        ++m_errors;
        auto tool = m_tools.find(call.name);
        if (tool != m_tools.end())
            ++tool->errors;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (!it.key().startsWith(id + QLatin1Char('|'))) {
                ++it;
                continue;
            }
            auto out = m_outputs.find(it->key);
            if (out != m_outputs.end()) {
                out->added -= it->added;
                out->removed -= it->removed;
                if (--out->count <= 0)
                    m_outputs.erase(out);
            }
            it = m_pending.erase(it);
        }
    } else {
        m_pending.removeIf([&id](const QHash<QString, Pending>::iterator &it) {
            return it.key().startsWith(id + QLatin1Char('|'));
        });
    }
    auto task = m_background.find(id);
    if (task == m_background.end())
        return true;
    if (isError) {
        task->status = QStringLiteral("failed");
    } else if (call.background) {
        static const QRegularExpression shellId(
            QStringLiteral("(?:with ID|shell id|task id|agentId)[:\\s]+([A-Za-z0-9_-]+)"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = shellId.match(resultText);
        if (match.hasMatch())
            m_shells.insert(match.captured(1), id);
    } else {
        task->status = QStringLiteral("completed");
    }
    task->touched = ++m_clock;
    return true;
}

bool Collector::applyNotifications(const QString &body)
{
    if (!body.contains(QLatin1String("<task-notification>")))
        return false;
    bool changed = false;
    for (const tt::TaskNotification &note : tt::taskNotifications(body)) {
        auto it = m_background.find(note.toolUseId);
        if (it == m_background.end())
            it = m_background.find(m_shells.value(note.toolUseId));
        if (it == m_background.end())
            continue;
        QString status = note.status;
        if (status == QLatin1String("completed") || status == QLatin1String("done") ||
            status == QLatin1String("success"))
            status = QStringLiteral("completed");
        else if (status == QLatin1String("killed") || status == QLatin1String("stopped"))
            status = QStringLiteral("stopped");
        else if (status.isEmpty() || status == QLatin1String("running"))
            continue;
        else if (status != QLatin1String("failed"))
            status = QStringLiteral("failed");
        it->status = status;
        it->touched = ++m_clock;
        changed = true;
    }
    return changed;
}

QList<Entry> Collector::outputs() const
{
    return newestFirst(m_outputs);
}

QList<Entry> Collector::sources() const
{
    return newestFirst(m_sources);
}

QList<Background> Collector::background() const
{
    QList<Background> list = newestFirst(m_background);
    std::stable_sort(list.begin(), list.end(), [](const Background &a, const Background &b) {
        return a.running() && !b.running();
    });
    return list;
}

QList<ToolCount> Collector::tools() const
{
    QList<ToolCount> list = m_tools.values();
    std::sort(list.begin(), list.end(), [](const ToolCount &a, const ToolCount &b) {
        return a.calls != b.calls ? a.calls > b.calls : a.name < b.name;
    });
    return list;
}

int Collector::runningBackground() const
{
    int running = 0;
    for (const Background &item : m_background)
        running += item.running() ? 1 : 0;
    return running;
}

QStringList defaultCardOrder()
{
    return {QStringLiteral("progress"),   QStringLiteral("outputs"),
            QStringLiteral("sources"),    QStringLiteral("background"),
            QStringLiteral("changes"),    QStringLiteral("context"),
            QStringLiteral("environment"), QStringLiteral("tools")};
}

QStringList normalizedCardOrder(const QStringList &saved)
{
    const QStringList known = defaultCardOrder();
    QStringList order;
    for (const QString &key : saved)
        if (known.contains(key) && !order.contains(key))
            order << key;
    for (int index = 0; index < known.size(); ++index) {
        const QString &key = known.at(index);
        if (order.contains(key))
            continue;
        int at = 0;
        for (int before = index - 1; before >= 0; --before) {
            const qsizetype found = order.indexOf(known.at(before));
            if (found >= 0) {
                at = int(found) + 1;
                break;
            }
        }
        order.insert(at, key);
    }
    return order;
}

QStringList movedCard(const QStringList &order, const QString &key, int toIndex)
{
    QStringList moved = order;
    const qsizetype from = moved.indexOf(key);
    if (from < 0)
        return moved;
    moved.removeAt(from);
    moved.insert(qBound(0, toIndex, int(moved.size())), key);
    return moved;
}

}

#include "TeammateWorkspace.h"

#include "TeammateStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace forkmesh::teammates {

namespace {

QString capped(QString text, int limit)
{
    text = text.trimmed();
    return text.size() > limit ? text.left(limit) : text;
}

QString checklistLine(const Widget &widget)
{
    QStringList items;
    for (const WidgetItem &item : widget.items)
        items.append(QStringLiteral("[%1] %2")
                         .arg(item.done ? QStringLiteral("x") : QStringLiteral(" "),
                              item.text));
    return items.join(QStringLiteral("; "));
}

QString number(double value)
{
    return QString::number(value, 'g', 10);
}

} // namespace

QStringList widgetKinds()
{
    return {kWidgetChecklist, kWidgetGoal, kWidgetNote, kWidgetPrompt,
            kWidgetLinks};
}

QString widgetKindLabel(const QString &kind)
{
    if (kind == kWidgetChecklist)
        return QStringLiteral("Checklist");
    if (kind == kWidgetGoal)
        return QStringLiteral("Goal tracker");
    if (kind == kWidgetNote)
        return QStringLiteral("Pinned note");
    if (kind == kWidgetPrompt)
        return QStringLiteral("Quick request");
    if (kind == kWidgetLinks)
        return QStringLiteral("Links");
    return QStringLiteral("Widget");
}

QString widgetKindIcon(const QString &kind)
{
    if (kind == kWidgetChecklist)
        return QStringLiteral("check-circle");
    if (kind == kWidgetGoal)
        return QStringLiteral("graph");
    if (kind == kWidgetNote)
        return QStringLiteral("note");
    if (kind == kWidgetPrompt)
        return QStringLiteral("paper-airplane");
    if (kind == kWidgetLinks)
        return QStringLiteral("link");
    return QStringLiteral("package");
}

QString newWidgetId()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(12);
}

Widget newWidget(const QString &kind, const QString &title)
{
    Widget widget;
    widget.id = newWidgetId();
    widget.kind = kind;
    widget.title = title.trimmed().isEmpty() ? widgetKindLabel(kind) : title;
    widget.createdBy = kWorkspaceFromYou;
    widget.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (kind == kWidgetGoal)
        widget.target = 10.0;
    normalizeWidget(&widget);
    return widget;
}

QString safeUrl(const QString &url)
{
    const QUrl parsed = QUrl::fromUserInput(url.trimmed());
    if (!parsed.isValid())
        return {};
    const QString scheme = parsed.scheme().toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return {};
    return parsed.toString();
}

bool normalizeWidget(Widget *widget)
{
    if (!widget)
        return false;
    widget->kind = widget->kind.trimmed().toLower();
    if (!widgetKinds().contains(widget->kind))
        return false;
    widget->id = widget->id.trimmed();
    widget->id.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")));
    widget->id = widget->id.left(40);
    widget->title = capped(widget->title, 120);
    if (widget->title.isEmpty())
        widget->title = widgetKindLabel(widget->kind);
    widget->text = capped(widget->text, 8 * 1024);
    widget->unit = capped(widget->unit, 24);
    widget->createdBy = capped(widget->createdBy, 80);
    QList<WidgetItem> items;
    for (WidgetItem item : std::as_const(widget->items)) {
        item.text = capped(item.text, 400);
        item.url = widget->kind == kWidgetLinks ? safeUrl(item.url) : QString();
        if (widget->kind == kWidgetLinks && item.url.isEmpty())
            continue;
        if (item.text.isEmpty()) {
            if (item.url.isEmpty())
                continue;
            item.text = item.url;
        }
        items.append(item);
        if (items.size() >= kWorkspaceMaxItems)
            break;
    }
    if (widget->kind != kWidgetChecklist && widget->kind != kWidgetLinks)
        items.clear();
    widget->items = items;
    if (widget->kind == kWidgetGoal) {
        if (!std::isfinite(widget->value))
            widget->value = 0.0;
        if (!std::isfinite(widget->target) || widget->target <= 0.0)
            widget->target = 1.0;
    } else {
        widget->value = 0.0;
        widget->target = 0.0;
        widget->unit.clear();
    }
    return true;
}

double widgetProgress(const Widget &widget)
{
    if (widget.kind == kWidgetGoal)
        return widget.target > 0.0
                   ? qBound(0.0, widget.value / widget.target, 1.0)
                   : 0.0;
    if (widget.kind == kWidgetChecklist && !widget.items.isEmpty())
        return double(widget.items.size() - openChecklistItems(widget)) /
               double(widget.items.size());
    return 0.0;
}

int openChecklistItems(const Widget &widget)
{
    return int(std::count_if(widget.items.cbegin(), widget.items.cend(),
                             [](const WidgetItem &item) { return !item.done; }));
}

QJsonObject widgetToJson(const Widget &widget)
{
    QJsonObject object{{QStringLiteral("id"), widget.id},
                       {QStringLiteral("kind"), widget.kind},
                       {QStringLiteral("title"), widget.title}};
    if (!widget.text.isEmpty())
        object.insert(QStringLiteral("text"), widget.text);
    if (!widget.items.isEmpty()) {
        QJsonArray items;
        for (const WidgetItem &item : widget.items) {
            QJsonObject entry{{QStringLiteral("text"), item.text}};
            if (!item.url.isEmpty())
                entry.insert(QStringLiteral("url"), item.url);
            if (item.done)
                entry.insert(QStringLiteral("done"), true);
            items.append(entry);
        }
        object.insert(QStringLiteral("items"), items);
    }
    if (widget.kind == kWidgetGoal) {
        object.insert(QStringLiteral("value"), widget.value);
        object.insert(QStringLiteral("target"), widget.target);
        if (!widget.unit.isEmpty())
            object.insert(QStringLiteral("unit"), widget.unit);
    }
    if (!widget.createdBy.isEmpty())
        object.insert(QStringLiteral("createdBy"), widget.createdBy);
    if (widget.updatedAtMs > 0)
        object.insert(QStringLiteral("updatedAtMs"),
                      double(widget.updatedAtMs));
    return object;
}

Widget widgetFromJson(const QJsonObject &object)
{
    Widget widget;
    widget.id = object.value(QStringLiteral("id")).toString();
    widget.kind = object.value(QStringLiteral("kind")).toString();
    widget.title = object.value(QStringLiteral("title")).toString();
    widget.text = object.value(QStringLiteral("text")).toString();
    const QJsonValue items = object.value(QStringLiteral("items"));
    for (const QJsonValue &value : items.toArray()) {
        WidgetItem item;
        if (value.isString()) {
            item.text = value.toString();
        } else {
            const QJsonObject entry = value.toObject();
            item.text = entry.value(QStringLiteral("text")).toString();
            item.url = entry.value(QStringLiteral("url")).toString();
            item.done = entry.value(QStringLiteral("done")).toBool();
        }
        widget.items.append(item);
    }
    widget.value = object.value(QStringLiteral("value")).toDouble();
    widget.target = object.value(QStringLiteral("target")).toDouble();
    widget.unit = object.value(QStringLiteral("unit")).toString();
    widget.createdBy = object.value(QStringLiteral("createdBy")).toString();
    widget.updatedAtMs =
        qint64(object.value(QStringLiteral("updatedAtMs")).toDouble());
    return widget;
}

QJsonObject workspaceToJson(const Workspace &workspace)
{
    QJsonArray widgets;
    for (const Widget &widget : workspace.widgets)
        widgets.append(widgetToJson(widget));
    QJsonArray messages;
    for (const ChatEntry &entry : workspace.messages) {
        QJsonObject message{{QStringLiteral("from"), entry.from},
                            {QStringLiteral("text"), entry.text},
                            {QStringLiteral("atMs"), double(entry.atMs)}};
        if (entry.sessionId > 0)
            message.insert(QStringLiteral("sessionId"), entry.sessionId);
        messages.append(message);
    }
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("notes"), workspace.notes},
            {QStringLiteral("notesUpdatedAtMs"), double(workspace.notesUpdatedAtMs)},
            {QStringLiteral("widgets"), widgets},
            {QStringLiteral("messages"), messages},
            {QStringLiteral("handledProposals"),
             QJsonArray::fromStringList(workspace.handledProposals)}};
}

Workspace workspaceFromJson(const QJsonObject &object)
{
    Workspace workspace;
    workspace.notes =
        object.value(QStringLiteral("notes")).toString().left(kWorkspaceMaxNotes);
    workspace.notesUpdatedAtMs =
        qint64(object.value(QStringLiteral("notesUpdatedAtMs")).toDouble());
    QStringList seen;
    for (const QJsonValue &value :
         object.value(QStringLiteral("widgets")).toArray()) {
        Widget widget = widgetFromJson(value.toObject());
        if (!normalizeWidget(&widget))
            continue;
        if (widget.id.isEmpty() || seen.contains(widget.id))
            widget.id = newWidgetId();
        seen.append(widget.id);
        workspace.widgets.append(widget);
        if (workspace.widgets.size() >= kWorkspaceMaxWidgets)
            break;
    }
    for (const QJsonValue &value :
         object.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject message = value.toObject();
        ChatEntry entry;
        entry.from = message.value(QStringLiteral("from")).toString();
        entry.text = message.value(QStringLiteral("text")).toString();
        entry.atMs = qint64(message.value(QStringLiteral("atMs")).toDouble());
        entry.sessionId = message.value(QStringLiteral("sessionId")).toInt();
        if (!entry.text.trimmed().isEmpty())
            workspace.messages.append(entry);
    }
    while (workspace.messages.size() > kWorkspaceMaxMessages)
        workspace.messages.removeFirst();
    for (const QJsonValue &value :
         object.value(QStringLiteral("handledProposals")).toArray()) {
        const QString fingerprint = value.toString();
        if (!fingerprint.isEmpty())
            workspace.handledProposals.append(fingerprint);
    }
    return workspace;
}

void appendMessage(Workspace *workspace, ChatEntry entry)
{
    if (!workspace)
        return;
    entry.text = capped(entry.text, 16 * 1024);
    if (entry.text.isEmpty())
        return;
    if (entry.atMs <= 0)
        entry.atMs = QDateTime::currentMSecsSinceEpoch();
    workspace->messages.append(entry);
    while (workspace->messages.size() > kWorkspaceMaxMessages)
        workspace->messages.removeFirst();
}

bool upsertWidget(Workspace *workspace, Widget widget)
{
    if (!workspace || !normalizeWidget(&widget))
        return false;
    widget.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (!widget.id.isEmpty()) {
        for (Widget &existing : workspace->widgets) {
            if (existing.id == widget.id) {
                if (widget.createdBy.isEmpty())
                    widget.createdBy = existing.createdBy;
                existing = widget;
                return true;
            }
        }
    } else {
        widget.id = newWidgetId();
    }
    if (workspace->widgets.size() >= kWorkspaceMaxWidgets)
        return false;
    workspace->widgets.append(widget);
    return true;
}

bool removeWidget(Workspace *workspace, const QString &widgetId)
{
    if (!workspace)
        return false;
    const auto before = workspace->widgets.size();
    workspace->widgets.erase(
        std::remove_if(workspace->widgets.begin(), workspace->widgets.end(),
                       [&widgetId](const Widget &widget) {
                           return widget.id == widgetId;
                       }),
        workspace->widgets.end());
    return workspace->widgets.size() != before;
}

void markProposalHandled(Workspace *workspace, const QString &fingerprint)
{
    if (!workspace || fingerprint.isEmpty() ||
        workspace->handledProposals.contains(fingerprint))
        return;
    workspace->handledProposals.append(fingerprint);
    while (workspace->handledProposals.size() > kWorkspaceMaxHandled)
        workspace->handledProposals.removeFirst();
}

QList<Widget> widgetBlocksFromOutput(const QString &output)
{
    static const QRegularExpression fence(
        QStringLiteral("```[ \\t]*forkmesh-widget[^\\n]*\\n(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);
    QList<Widget> widgets;
    auto matches = fence.globalMatch(output);
    while (matches.hasNext() && widgets.size() < 12) {
        const QByteArray body = matches.next().captured(1).trimmed().toUtf8();
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError)
            continue;
        QList<QJsonObject> objects;
        if (document.isObject())
            objects.append(document.object());
        else
            for (const QJsonValue &value : document.array())
                objects.append(value.toObject());
        for (const QJsonObject &object : std::as_const(objects)) {
            Widget widget = widgetFromJson(object);
            widget.updatedAtMs = 0;
            if (normalizeWidget(&widget))
                widgets.append(widget);
        }
    }
    return widgets;
}

QString proposalFingerprint(const Widget &widget)
{
    Widget stable = widget;
    stable.updatedAtMs = 0;
    stable.createdBy.clear();
    const QByteArray bytes =
        QJsonDocument(widgetToJson(stable)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16));
}

QList<WidgetProposal> pendingProposals(const Workspace &workspace,
                                       const QString &output, int sessionId)
{
    QList<WidgetProposal> proposals;
    for (const Widget &widget : widgetBlocksFromOutput(output)) {
        WidgetProposal proposal;
        proposal.widget = widget;
        proposal.fingerprint = proposalFingerprint(widget);
        proposal.sessionId = sessionId;
        if (workspace.handledProposals.contains(proposal.fingerprint))
            continue;
        proposal.updatesExisting =
            !widget.id.isEmpty() &&
            std::any_of(workspace.widgets.cbegin(), workspace.widgets.cend(),
                        [&widget](const Widget &existing) {
                            return existing.id == widget.id;
                        });
        proposals.append(proposal);
    }
    return proposals;
}

QString widgetContract()
{
    return QStringLiteral(
        "To propose a new widget for the operator's page, or an update to one, "
        "put a fenced block tagged forkmesh-widget in your closing summary "
        "holding one JSON object: {\"id\": \"<existing id to update, or omit>\", "
        "\"kind\": \"checklist|goal|note|prompt|links\", \"title\": \"...\", "
        "\"text\": \"note body or the saved request\", \"items\": [{\"text\": "
        "\"...\", \"done\": false, \"url\": \"https://... for links\"}], "
        "\"value\": 0, \"target\": 10, \"unit\": \"...\"}. A checklist tracks "
        "steps, a goal tracks a number against a target, a note pins "
        "reference text, a prompt is a request the operator can send you with "
        "one click, and links collect pages. The operator reviews every "
        "proposal before it is added.");
}

QString workspacePromptSection(const Workspace &workspace)
{
    QStringList parts;
    const QString notes = workspace.notes.trimmed();
    if (!notes.isEmpty())
        parts << QStringLiteral("Operator notes\n%1").arg(capped(notes, 6000));
    QStringList widgets;
    for (const Widget &widget : workspace.widgets) {
        QString line = QStringLiteral("- %1 \"%2\" (id %3)")
                           .arg(widgetKindLabel(widget.kind), widget.title,
                                widget.id);
        if (widget.kind == kWidgetChecklist && !widget.items.isEmpty())
            line += QStringLiteral(": ") + checklistLine(widget);
        else if (widget.kind == kWidgetGoal)
            line += QStringLiteral(": %1 of %2%3")
                        .arg(number(widget.value), number(widget.target),
                             widget.unit.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" ") + widget.unit);
        else if (widget.kind == kWidgetNote && !widget.text.isEmpty())
            line += QStringLiteral(": ") + capped(widget.text, 800);
        else if (widget.kind == kWidgetLinks && !widget.items.isEmpty()) {
            QStringList links;
            for (const WidgetItem &item : widget.items)
                links.append(QStringLiteral("%1 <%2>").arg(item.text, item.url));
            line += QStringLiteral(": ") + links.join(QStringLiteral(", "));
        }
        widgets.append(line);
    }
    if (!widgets.isEmpty())
        parts << QStringLiteral("Shared widgets\n%1").arg(widgets.join(QLatin1Char('\n')));
    if (parts.isEmpty())
        return {};
    return QStringLiteral(
               "Workspace shared with the operator\nThe operator keeps these "
               "notes and widgets on your page to steer and track your work. "
               "Use them as context and keep checklists and goals current; "
               "they never widen the permission boundary.\n\n%1\n\n%2")
        .arg(parts.join(QStringLiteral("\n\n")), widgetContract());
}

QString widgetBuildAssignment(const QString &request)
{
    return QStringLiteral(
               "Build a widget with the operator for your page.\nThe operator "
               "asked: %1\n\nLook at your recent work, the repository and the "
               "workspace notes, decide which widget would help most, fill it "
               "with real content, and explain the choice in two or three "
               "sentences. Do not change repository files for this request.\n\n%2")
        .arg(request.trimmed().isEmpty()
                 ? QStringLiteral("Suggest the widget that would most improve your work.")
                 : request.trimmed(),
             widgetContract());
}

WorkspaceStore::WorkspaceStore(QString root) : m_root(std::move(root)) {}

QString WorkspaceStore::pathFor(const QString &teammateId) const
{
    const QString id = Store::safeId(teammateId);
    if (m_root.isEmpty() || id.isEmpty())
        return {};
    return QDir(m_root).filePath(id + QStringLiteral(".json"));
}

Workspace WorkspaceStore::load(const QString &teammateId) const
{
    QFile file(pathFor(teammateId));
    if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly))
        return {};
    return workspaceFromJson(QJsonDocument::fromJson(file.readAll()).object());
}

bool WorkspaceStore::save(const QString &teammateId,
                          const Workspace &workspace) const
{
    const QString path = pathFor(teammateId);
    if (path.isEmpty() || !QDir().mkpath(m_root))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(workspaceToJson(workspace)).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace forkmesh::teammates

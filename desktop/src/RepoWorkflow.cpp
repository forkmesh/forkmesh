#include "RepoWorkflow.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTime>

#include <algorithm>

namespace forkmesh::repoworkflows {
namespace {

using forkmesh::teammates::Config;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

QJsonObject option(const QString &value, const QString &label)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
}

QJsonObject field(const QString &key, const QString &label, const QString &type,
                  const QJsonObject &extra = {})
{
    QJsonObject result{{QStringLiteral("key"), key},
                       {QStringLiteral("label"), label},
                       {QStringLiteral("type"), type}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        result.insert(it.key(), it.value());
    return result;
}

struct DesktopStep {
    const char *type;
    const char *step;
    const char *name;
    const char *icon;
    const char *description;
};

const DesktopStep kDesktopSteps[] = {
    {"desktop.diskUsage", "usage", "Measure the disk", "hard-drive",
     "Reads the free space and the agent worktrees the cleanup keeps."},
    {"desktop.cleanAgents", "agents", "Remove agent leftovers", "bot",
     "Deletes scratch folders and builds of agents that no longer run."},
    {"desktop.cleanTests", "tests", "Remove test leftovers", "flask-conical",
     "Deletes pytest, Actions sandbox and Qt test folders nothing uses."},
    {"desktop.cleanVersions", "versions", "Prune old versions and Trash", "archive",
     "Removes old Claude versions, backups past the newest three and ForkMesh cleanup items in the Trash."},
    {"desktop.cleanCaches", "caches", "Clear pip and npm caches", "package",
     "Runs pip3 cache purge and npm cache clean."},
    {"desktop.diskReport", "report", "Report free space", "gauge",
     "Reads the free space after the cleanup and lists big folders it leaves for you."},
};

QJsonArray permissionOptions()
{
    QJsonArray options;
    for (const QString &permission : forkmesh::teammates::knownPermissions())
        options.append(option(permission, forkmesh::teammates::permissionLabel(permission)));
    return options;
}

QJsonArray taskTypeOptions()
{
    QJsonArray options;
    for (const QString &type : forkmesh::teammates::knownTaskTypes())
        options.append(option(type, forkmesh::teammates::taskTypeLabel(type)));
    return options;
}

QJsonArray scheduleModeOptions()
{
    return {option(forkmesh::teammates::kScheduleOnDemand, QStringLiteral("Only when asked")),
            option(forkmesh::teammates::kScheduleContinuous, QStringLiteral("Continuously")),
            option(forkmesh::teammates::kScheduleHourly, QStringLiteral("Every hour")),
            option(forkmesh::teammates::kScheduleDaily, QStringLiteral("Every day")),
            option(forkmesh::teammates::kScheduleWeekly, QStringLiteral("Every week")),
            option(forkmesh::teammates::kScheduleWeekdays, QStringLiteral("Weekdays")),
            option(forkmesh::teammates::kScheduleCustom, QStringLiteral("Chosen days")),
            option(forkmesh::teammates::kScheduleEvents, QStringLiteral("On events"))};
}

QJsonArray dayOptions()
{
    const QStringList names{QStringLiteral("Monday"),   QStringLiteral("Tuesday"),
                            QStringLiteral("Wednesday"), QStringLiteral("Thursday"),
                            QStringLiteral("Friday"),   QStringLiteral("Saturday"),
                            QStringLiteral("Sunday")};
    QJsonArray options;
    for (int day = 1; day <= 7; ++day)
        options.append(option(QString::number(day), names.at(day - 1)));
    return options;
}

QJsonObject spec(const QString &name, const QString &group, const QString &icon,
                 const QString &color, const QString &description, const QJsonArray &fields,
                 bool trigger = false, bool single = false)
{
    QJsonObject result{{QStringLiteral("name"), name},
                       {QStringLiteral("group"), group},
                       {QStringLiteral("icon"), icon},
                       {QStringLiteral("color"), color},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("fields"), fields},
                       {QStringLiteral("outputs"), QJsonArray{QStringLiteral("main")}},
                       {QStringLiteral("runsOn"), QStringLiteral("desktop")}};
    if (trigger)
        result.insert(QStringLiteral("trigger"), true);
    if (single)
        result.insert(QStringLiteral("single"), true);
    return result;
}

const QHash<QString, QJsonObject> &specs()
{
    static const QHash<QString, QJsonObject> table = [] {
        QHash<QString, QJsonObject> result;
        result.insert(kManual, spec(QStringLiteral("Run by hand"), QStringLiteral("Triggers"),
                                    QStringLiteral("mouse-pointer-click"), QStringLiteral("#64748b"),
                                    QStringLiteral("Start the workflow with its Run button, a pinned rail "
                                                   "button or a phone."),
                                    {}, true, true));
        result.insert(kSchedule,
                      spec(QStringLiteral("Schedule"), QStringLiteral("Triggers"),
                           QStringLiteral("calendar-clock"), QStringLiteral("#0891b2"),
                           QStringLiteral("When the workflow runs by itself: on a cadence, continuously, "
                                          "or on repository events."),
                           {field(QStringLiteral("mode"), QStringLiteral("How often"), QStringLiteral("select"),
                                  {{QStringLiteral("options"), scheduleModeOptions()},
                                   {QStringLiteral("default"), forkmesh::teammates::kScheduleOnDemand}}),
                            field(QStringLiteral("time"), QStringLiteral("Time (HH:mm)"), QStringLiteral("string"),
                                  {{QStringLiteral("placeholder"), QStringLiteral("09:00")},
                                   {QStringLiteral("default"), QStringLiteral("09:00")}}),
                            field(QStringLiteral("days"), QStringLiteral("Days"), QStringLiteral("checks"),
                                  {{QStringLiteral("options"), dayOptions()}}),
                            field(QStringLiteral("events"), QStringLiteral("Run on events"), QStringLiteral("checks"),
                                  {{QStringLiteral("options"), taskTypeOptions()}}),
                            field(QStringLiteral("active"), QStringLiteral("Schedule is on"), QStringLiteral("boolean"),
                                  {{QStringLiteral("default"), QStringLiteral("false")}})},
                           true, true));
        result.insert(kMonitor,
                      spec(QStringLiteral("Log monitor"), QStringLiteral("Triggers"), QStringLiteral("activity"),
                           QStringLiteral("#155e75"),
                           QStringLiteral("Starts a run when an app-log badge passes a threshold."),
                           {field(QStringLiteral("badge"), QStringLiteral("Badge"), QStringLiteral("string"),
                                  {{QStringLiteral("required"), true},
                                   {QStringLiteral("placeholder"), QStringLiteral("STALL")}}),
                            field(QStringLiteral("threshold"), QStringLiteral("Threshold"), QStringLiteral("number"),
                                  {{QStringLiteral("default"), QStringLiteral("10")}}),
                            field(QStringLiteral("taskType"), QStringLiteral("Task type"), QStringLiteral("select"),
                                  {{QStringLiteral("options"), taskTypeOptions()},
                                   {QStringLiteral("default"), forkmesh::teammates::kTaskTypeErrorLogs}}),
                            field(QStringLiteral("active"), QStringLiteral("Monitor is on"), QStringLiteral("boolean"),
                                  {{QStringLiteral("default"), QStringLiteral("true")}}),
                            field(QStringLiteral("inclusive"), QStringLiteral("Include the threshold"),
                                  QStringLiteral("boolean"), {{QStringLiteral("default"), QStringLiteral("false")}})},
                           true));
        result.insert(
            kAgent,
            spec(QStringLiteral("Special agent"), QStringLiteral("Agents"), QStringLiteral("bot"),
                 QStringLiteral("#db2777"),
                 QStringLiteral("Runs a coding agent with a role, instructions, skills, a playbook, a learning "
                                "journal and only the permissions you tick. Its reply is the step's output."),
                 {field(QStringLiteral("avatar"), QStringLiteral("Portrait"), QStringLiteral("avatar"),
                        {{QStringLiteral("default"), QStringLiteral("sol")}}),
                  field(QStringLiteral("specialty"), QStringLiteral("Specialty"), QStringLiteral("string"),
                        {{QStringLiteral("required"), true},
                         {QStringLiteral("placeholder"), QStringLiteral("Security review")}}),
                  field(QStringLiteral("focus"), QStringLiteral("Focus"), QStringLiteral("string")),
                  field(QStringLiteral("description"), QStringLiteral("Responsibility"), QStringLiteral("text")),
                  field(QStringLiteral("instructions"), QStringLiteral("Operating instructions"),
                        QStringLiteral("code")),
                  field(QStringLiteral("shortcut"), QStringLiteral("Playbook shortcut"), QStringLiteral("shortcut")),
                  field(QStringLiteral("skills"), QStringLiteral("Skill names (one per line)"),
                        QStringLiteral("list")),
                  field(QStringLiteral("skillRefs"), QStringLiteral("Skill files (scope:file, one per line)"),
                        QStringLiteral("list"),
                        {{QStringLiteral("placeholder"), QStringLiteral("repository:security-review.skill")}}),
                  field(QStringLiteral("permissions"), QStringLiteral("Permissions"), QStringLiteral("checks"),
                        {{QStringLiteral("options"), permissionOptions()}}),
                  field(QStringLiteral("selfImproving"), QStringLiteral("Keeps a learning journal"),
                        QStringLiteral("boolean"), {{QStringLiteral("default"), QStringLiteral("true")}}),
                  field(QStringLiteral("improvementNotes"), QStringLiteral("Learning journal (one lesson per line)"),
                        QStringLiteral("list")),
                  field(QStringLiteral("lowCostModel"), QStringLiteral("Use the low-cost model"),
                        QStringLiteral("boolean"), {{QStringLiteral("default"), QStringLiteral("false")}}),
                  field(QStringLiteral("runWindowHours"), QStringLiteral("Expected run window (hours)"),
                        QStringLiteral("number"), {{QStringLiteral("default"), QStringLiteral("0")}}),
                  field(QStringLiteral("relayChannel"), QStringLiteral("Relay channel"), QStringLiteral("string"),
                        {{QStringLiteral("placeholder"), QStringLiteral("#team")}}),
                  field(QStringLiteral("marketingChannels"), QStringLiteral("Marketing channels (one per line)"),
                        QStringLiteral("list")),
                  field(QStringLiteral("workflow"), QStringLiteral("Native workflow"), QStringLiteral("select"),
                        {{QStringLiteral("options"),
                          QJsonArray{option(QString(), QStringLiteral("None")),
                                     option(forkmesh::teammates::kWorkflowContributorPayouts,
                                            QStringLiteral("Contributor payouts"))}}})}));
        result.insert(kShortcut,
                      spec(QStringLiteral("Run shortcut"), QStringLiteral("Shortcuts"), QStringLiteral("command"),
                           QStringLiteral("#2563eb"),
                           QStringLiteral("Runs a script or prompt from .forkmesh/shortcuts. Scripts get the earlier "
                                          "steps' output in FORKMESH_WORKFLOW_INPUT."),
                           {field(QStringLiteral("file"), QStringLiteral("Shortcut file"), QStringLiteral("shortcut"),
                                  {{QStringLiteral("required"), true}}),
                            field(QStringLiteral("wait"), QStringLiteral("Wait for it to finish"),
                                  QStringLiteral("boolean"), {{QStringLiteral("default"), QStringLiteral("true")}}),
                            field(QStringLiteral("allowEdits"), QStringLiteral("Prompt shortcuts may edit files"),
                                  QStringLiteral("boolean"), {{QStringLiteral("default"), QStringLiteral("false")}})}));
        result.insert(kOutput,
                      spec(QStringLiteral("Save output"), QStringLiteral("Output"), QStringLiteral("file-text"),
                           QStringLiteral("#16a34a"),
                           QStringLiteral("Writes the run so far as Markdown. Every run keeps its own output file on "
                                          "this desktop; a path also saves a copy in the repository."),
                           {field(QStringLiteral("title"), QStringLiteral("Heading"), QStringLiteral("string")),
                            field(QStringLiteral("path"), QStringLiteral("Also save to (repository path)"),
                                  QStringLiteral("string"),
                                  {{QStringLiteral("placeholder"), QStringLiteral("docs/reports/latest.md")}})}));
        result.insert(kNote, spec(QStringLiteral("Note"), QStringLiteral("Output"), QStringLiteral("sticky-note"),
                                  QStringLiteral("#a16207"),
                                  QStringLiteral("A note on the canvas. It passes its input on unchanged."),
                                  {field(QStringLiteral("text"), QStringLiteral("Note"), QStringLiteral("text"))}));
        for (const DesktopStep &step : kDesktopSteps) {
            QJsonObject desktop = spec(QString::fromLatin1(step.name), QStringLiteral("Desktop"),
                                       QString::fromLatin1(step.icon), QStringLiteral("#0f766e"),
                                       QString::fromLatin1(step.description), {});
            desktop.insert(QStringLiteral("desktopStep"), QString::fromLatin1(step.step));
            result.insert(QString::fromLatin1(step.type), desktop);
        }
        return result;
    }();
    return table;
}

QStringList specOrder()
{
    QStringList order{kManual, kSchedule, kMonitor, kAgent, kShortcut, kOutput, kNote};
    for (const DesktopStep &step : kDesktopSteps)
        order.append(QString::fromLatin1(step.type));
    return order;
}

QJsonArray strings(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.append(value);
    return result;
}

QStringList lines(const QJsonValue &value)
{
    QStringList result;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            const QString clean = entry.toVariant().toString().trimmed();
            if (!clean.isEmpty())
                result.append(clean);
        }
        return result;
    }
    for (const QString &line : value.toString().split(QLatin1Char('\n'))) {
        const QString clean = line.trimmed();
        if (!clean.isEmpty())
            result.append(clean);
    }
    return result;
}

bool flag(const QJsonValue &value, bool fallback)
{
    if (value.isBool())
        return value.toBool();
    if (value.isString()) {
        const QString clean = value.toString().trimmed().toLower();
        if (clean == QLatin1String("true"))
            return true;
        if (clean == QLatin1String("false"))
            return false;
    }
    if (value.isDouble())
        return value.toDouble() != 0;
    return fallback;
}

int number(const QJsonValue &value, int fallback)
{
    if (value.isDouble())
        return int(value.toDouble());
    bool ok = false;
    const int parsed = value.toString().trimmed().toInt(&ok);
    return ok ? parsed : fallback;
}

QJsonObject scheduleJson(const QJsonObject &params)
{
    QJsonArray days;
    for (const QString &day : lines(params.value(QStringLiteral("days")))) {
        const int value = day.toInt();
        if (value >= 1 && value <= 7 && !days.contains(value))
            days.append(value);
    }
    return {{QStringLiteral("mode"), params.value(QStringLiteral("mode")).toString()},
            {QStringLiteral("time"), params.value(QStringLiteral("time")).toString()},
            {QStringLiteral("days"), days},
            {QStringLiteral("events"), strings(lines(params.value(QStringLiteral("events"))))},
            {QStringLiteral("active"), flag(params.value(QStringLiteral("active")), false)}};
}

QJsonObject monitorJson(const QJsonObject &params)
{
    return {{QStringLiteral("badge"), params.value(QStringLiteral("badge")).toString().trimmed()},
            {QStringLiteral("threshold"), qMax(0, number(params.value(QStringLiteral("threshold")), 0))},
            {QStringLiteral("taskType"), params.value(QStringLiteral("taskType"))
                                             .toString(forkmesh::teammates::kTaskTypeErrorLogs)},
            {QStringLiteral("active"), flag(params.value(QStringLiteral("active")), true)},
            {QStringLiteral("inclusive"), flag(params.value(QStringLiteral("inclusive")), false)}};
}

QJsonObject scheduleParams(const forkmesh::teammates::Schedule &schedule)
{
    const QJsonObject encoded = schedule.toJson();
    QJsonArray days;
    for (const QJsonValue &day : encoded.value(QStringLiteral("days")).toArray())
        days.append(QString::number(day.toInt()));
    return {{QStringLiteral("mode"), encoded.value(QStringLiteral("mode"))},
            {QStringLiteral("time"), encoded.value(QStringLiteral("time"))},
            {QStringLiteral("days"), days},
            {QStringLiteral("events"), encoded.value(QStringLiteral("events"))},
            {QStringLiteral("active"), encoded.value(QStringLiteral("active"))}};
}

QJsonObject monitorParams(const forkmesh::teammates::LogMonitor &monitor)
{
    return {{QStringLiteral("badge"), monitor.badge},
            {QStringLiteral("threshold"), monitor.threshold},
            {QStringLiteral("taskType"), monitor.taskType},
            {QStringLiteral("active"), monitor.active},
            {QStringLiteral("inclusive"), monitor.inclusive}};
}

QJsonObject makeNode(const QString &id, const QString &type, const QString &name, int column, int row,
                     const QJsonObject &params)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("type"), type},
            {QStringLiteral("name"), name},
            {QStringLiteral("x"), column * kColumn},
            {QStringLiteral("y"), row * kRow},
            {QStringLiteral("params"), params}};
}

QJsonObject connection(const QString &from, const QString &to)
{
    return {{QStringLiteral("from"), from},
            {QStringLiteral("output"), QStringLiteral("main")},
            {QStringLiteral("to"), to}};
}

bool connected(const QJsonArray &connections, const QString &from, const QString &to)
{
    for (const QJsonValue &value : connections) {
        const QJsonObject edge = value.toObject();
        if (text(edge, "from") == from && text(edge, "to") == to)
            return true;
    }
    return false;
}

void removeNode(Workflow *workflow, const QString &nodeId)
{
    const int index = workflow->indexOf(nodeId);
    if (index < 0)
        return;
    workflow->nodes.removeAt(index);
    QJsonArray kept;
    for (const QJsonValue &value : std::as_const(workflow->connections)) {
        const QJsonObject edge = value.toObject();
        if (text(edge, "from") != nodeId && text(edge, "to") != nodeId)
            kept.append(edge);
    }
    workflow->connections = kept;
}

void setNode(Workflow *workflow, const QJsonObject &node)
{
    const int index = workflow->indexOf(text(node, "id"));
    if (index >= 0)
        workflow->nodes.replace(index, node);
    else
        workflow->nodes.append(node);
}

QString clip(const QString &value, int limit)
{
    return value.size() > limit ? value.left(limit) + QStringLiteral("\n…") : value;
}

QString isoTime(qint64 ms)
{
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toString(Qt::ISODate) : QString();
}

QString durationText(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(qMax<qint64>(0, ms));
    const qint64 seconds = ms / 1000;
    if (seconds < 90)
        return QStringLiteral("%1 s").arg(seconds);
    const qint64 minutes = seconds / 60;
    if (minutes < 90)
        return QStringLiteral("%1 min").arg(minutes);
    return QStringLiteral("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

QString statusWord(const QString &status)
{
    if (status == kStatusSuccess)
        return QStringLiteral("succeeded");
    if (status == kStatusError)
        return QStringLiteral("failed");
    if (status == kStatusStopped)
        return QStringLiteral("stopped");
    if (status == kStatusSkipped)
        return QStringLiteral("skipped");
    if (status == kStatusQueued)
        return QStringLiteral("waiting");
    return QStringLiteral("running");
}

} // namespace

QJsonObject Workflow::toJson() const
{
    return {{QStringLiteral("kind"), kFileKind},
            {QStringLiteral("version"), version},
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("icon"), icon},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("createdAtMs"), double(createdAtMs)},
            {QStringLiteral("updatedAtMs"), double(updatedAtMs)},
            {QStringLiteral("nodes"), nodes},
            {QStringLiteral("connections"), connections}};
}

Workflow Workflow::fromJson(const QJsonObject &object, QString *error)
{
    Workflow result;
    result.version = qMax(1, object.value(QStringLiteral("version")).toInt(1));
    result.id = forkmesh::teammates::Store::safeId(text(object, "id"));
    result.name = text(object, "name").trimmed().left(80);
    result.description = text(object, "description").trimmed().left(1000);
    result.icon = text(object, "icon").trimmed().left(80);
    result.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    result.createdAtMs = qint64(object.value(QStringLiteral("createdAtMs")).toDouble());
    result.updatedAtMs = qint64(object.value(QStringLiteral("updatedAtMs")).toDouble());
    for (const QJsonValue &value : object.value(QStringLiteral("nodes")).toArray()) {
        QJsonObject node = value.toObject();
        if (!node.value(QStringLiteral("params")).isObject())
            node.insert(QStringLiteral("params"), QJsonObject());
        result.nodes.append(node);
    }
    for (const QJsonValue &value : object.value(QStringLiteral("connections")).toArray()) {
        QJsonObject edge = value.toObject();
        if (text(edge, "output").isEmpty())
            edge.insert(QStringLiteral("output"), QStringLiteral("main"));
        result.connections.append(edge);
    }
    const QString problem = validate(result);
    if (error)
        *error = problem;
    return result;
}

int Workflow::indexOf(const QString &nodeId) const
{
    for (int index = 0; index < nodes.size(); ++index)
        if (text(nodes.at(index).toObject(), "id") == nodeId)
            return index;
    return -1;
}

QJsonObject Workflow::node(const QString &nodeId) const
{
    const int index = indexOf(nodeId);
    return index >= 0 ? nodes.at(index).toObject() : QJsonObject();
}

QStringList Workflow::ordered() const
{
    QStringList ids;
    QHash<QString, int> incoming;
    QHash<QString, QStringList> outgoing;
    for (const QJsonValue &value : nodes) {
        const QString id = text(value.toObject(), "id");
        ids.append(id);
        incoming.insert(id, 0);
    }
    for (const QJsonValue &value : connections) {
        const QJsonObject edge = value.toObject();
        const QString from = text(edge, "from");
        const QString to = text(edge, "to");
        if (!incoming.contains(from) || !incoming.contains(to))
            continue;
        outgoing[from].append(to);
        incoming[to] += 1;
    }
    QStringList order;
    QStringList ready;
    for (const QString &id : ids)
        if (incoming.value(id) == 0)
            ready.append(id);
    while (!ready.isEmpty()) {
        const QString id = ready.takeFirst();
        order.append(id);
        for (const QString &next : outgoing.value(id)) {
            incoming[next] -= 1;
            if (incoming.value(next) == 0) {
                int position = 0;
                while (position < ready.size() && ids.indexOf(ready.at(position)) < ids.indexOf(next))
                    ++position;
                ready.insert(position, next);
            }
        }
    }
    return order.size() == ids.size() ? order : QStringList();
}

QStringList Workflow::inputsOf(const QString &nodeId) const
{
    QStringList result;
    for (const QJsonValue &value : connections) {
        const QJsonObject edge = value.toObject();
        if (text(edge, "to") == nodeId && !result.contains(text(edge, "from")))
            result.append(text(edge, "from"));
    }
    return result;
}

QStringList Workflow::nodesOfType(const QString &type) const
{
    QStringList result;
    for (const QJsonValue &value : nodes)
        if (text(value.toObject(), "type") == type)
            result.append(text(value.toObject(), "id"));
    return result;
}

QString Workflow::primaryAgent() const
{
    const QStringList order = ordered();
    for (const QString &id : order.isEmpty() ? nodesOfType(kAgent) : order)
        if (text(node(id), "type") == kAgent)
            return id;
    return {};
}

QJsonObject Workflow::scheduleNode() const
{
    const QStringList ids = nodesOfType(kSchedule);
    return ids.isEmpty() ? QJsonObject() : node(ids.first());
}

QString Workflow::triggerSummary() const
{
    const QJsonObject schedule = scheduleNode();
    QStringList parts;
    if (!schedule.isEmpty() && !schedule.value(QStringLiteral("disabled")).toBool()) {
        const forkmesh::teammates::Schedule parsed = forkmesh::teammates::Schedule::fromJson(
            scheduleJson(schedule.value(QStringLiteral("params")).toObject()));
        if (parsed.mode != forkmesh::teammates::kScheduleOnDemand)
            parts.append(forkmesh::teammates::humanSchedule(parsed) +
                         (parsed.active ? QString() : QStringLiteral(" (off)")));
    }
    const int monitors = int(nodesOfType(kMonitor).size());
    if (monitors > 0)
        parts.append(monitors == 1 ? QStringLiteral("1 log monitor")
                                   : QStringLiteral("%1 log monitors").arg(monitors));
    if (parts.isEmpty())
        parts.append(QStringLiteral("By hand"));
    return parts.join(QStringLiteral(" · "));
}

bool looksLikeWorkflowFile(const QJsonObject &object)
{
    return object.value(QStringLiteral("nodes")).isArray() || text(object, "kind") == kFileKind;
}

bool isTriggerType(const QString &type)
{
    return type == kManual || type == kSchedule || type == kMonitor;
}

bool isKnownType(const QString &type)
{
    return specs().contains(type);
}

bool isDesktopStepType(const QString &type)
{
    return !desktopStepName(type).isEmpty();
}

QString desktopStepName(const QString &type)
{
    for (const DesktopStep &step : kDesktopSteps)
        if (type == QLatin1String(step.type))
            return QString::fromLatin1(step.step);
    return {};
}

QString validate(const Workflow &workflow)
{
    static const QRegularExpression nodeIdPattern(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    if (workflow.id.isEmpty())
        return QStringLiteral("invalid_workflow_id");
    if (workflow.name.trimmed().isEmpty())
        return QStringLiteral("workflow_needs_name");
    if (workflow.nodes.size() > kMaxNodes)
        return QStringLiteral("too_many_nodes");
    if (workflow.connections.size() > kMaxConnections)
        return QStringLiteral("too_many_connections");
    QSet<QString> ids;
    QSet<QString> names;
    QHash<QString, QString> types;
    int schedules = 0;
    int manuals = 0;
    int monitors = 0;
    for (const QJsonValue &value : workflow.nodes) {
        const QJsonObject node = value.toObject();
        const QString id = text(node, "id");
        const QString type = text(node, "type");
        const QString name = text(node, "name").trimmed();
        if (!nodeIdPattern.match(id).hasMatch() || ids.contains(id))
            return QStringLiteral("invalid_node");
        if (!isKnownType(type))
            return QStringLiteral("unknown_node_type");
        if (name.isEmpty() || name.size() > kMaxNodeName || names.contains(name))
            return QStringLiteral("duplicate_node_name");
        ids.insert(id);
        names.insert(name);
        types.insert(id, type);
        const QJsonObject params = node.value(QStringLiteral("params")).toObject();
        if (type == kSchedule) {
            ++schedules;
            const QString mode = params.value(QStringLiteral("mode")).toString();
            const QJsonArray modes = scheduleModeOptions();
            bool known = mode.isEmpty();
            for (const QJsonValue &entry : modes)
                known = known || text(entry.toObject(), "value") == mode;
            if (!known)
                return QStringLiteral("invalid_schedule_mode");
            const QString time = params.value(QStringLiteral("time")).toString();
            if (!time.isEmpty() && !QTime::fromString(time, QStringLiteral("HH:mm")).isValid())
                return QStringLiteral("invalid_schedule_time");
        } else if (type == kManual) {
            ++manuals;
        } else if (type == kMonitor) {
            ++monitors;
            if (params.value(QStringLiteral("badge")).toString().trimmed().isEmpty())
                return QStringLiteral("monitor_needs_badge");
        } else if (type == kAgent) {
            if (params.value(QStringLiteral("specialty")).toString().trimmed().isEmpty())
                return QStringLiteral("agent_needs_specialty");
            const QString native = params.value(QStringLiteral("workflow")).toString();
            if (!native.isEmpty() && native != forkmesh::teammates::kWorkflowContributorPayouts)
                return QStringLiteral("invalid_native_workflow");
        } else if (type == kShortcut) {
            const QString file = params.value(QStringLiteral("file")).toString().trimmed();
            if (file.isEmpty() || file.contains(QLatin1Char('/')) || file.contains(QLatin1Char('\\')) ||
                file.startsWith(QLatin1Char('.')))
                return QStringLiteral("shortcut_needs_file");
        } else if (type == kOutput) {
            const QString path = params.value(QStringLiteral("path")).toString().trimmed();
            if (!path.isEmpty() && safeRepositoryPath(path).isEmpty())
                return QStringLiteral("invalid_output_path");
        }
    }
    if (schedules > 1)
        return QStringLiteral("too_many_schedules");
    if (manuals > 1)
        return QStringLiteral("too_many_triggers");
    if (monitors > kMaxMonitors)
        return QStringLiteral("too_many_monitors");
    QSet<QString> edges;
    for (const QJsonValue &value : workflow.connections) {
        const QJsonObject edge = value.toObject();
        const QString from = text(edge, "from");
        const QString to = text(edge, "to");
        if (!ids.contains(from) || !ids.contains(to) || from == to ||
            text(edge, "output") != QLatin1String("main") || isTriggerType(types.value(to)))
            return QStringLiteral("invalid_connection");
        const QString key = from + QLatin1Char('>') + to;
        if (edges.contains(key))
            return QStringLiteral("invalid_connection");
        edges.insert(key);
    }
    if (!workflow.nodes.isEmpty() && workflow.ordered().isEmpty())
        return QStringLiteral("cycle");
    return {};
}

QString errorText(const QString &code)
{
    static const QHash<QString, QString> texts{
        {QStringLiteral("invalid_workflow_id"),
         QStringLiteral("The workflow needs an id made of lowercase letters, numbers and dashes.")},
        {QStringLiteral("workflow_needs_name"), QStringLiteral("Give the workflow a name.")},
        {QStringLiteral("too_many_nodes"), QStringLiteral("A workflow holds at most 60 steps.")},
        {QStringLiteral("too_many_connections"), QStringLiteral("A workflow holds at most 120 connections.")},
        {QStringLiteral("invalid_node"), QStringLiteral("Every step needs its own id.")},
        {QStringLiteral("unknown_node_type"),
         QStringLiteral("A step uses a type this desktop does not know. Update ForkMesh.")},
        {QStringLiteral("duplicate_node_name"),
         QStringLiteral("Every step needs a name of at most 60 characters that no other step uses.")},
        {QStringLiteral("invalid_schedule_mode"), QStringLiteral("The schedule uses an unknown cadence.")},
        {QStringLiteral("invalid_schedule_time"), QStringLiteral("Write the schedule time as HH:mm.")},
        {QStringLiteral("monitor_needs_badge"), QStringLiteral("A log monitor needs the badge it counts.")},
        {QStringLiteral("agent_needs_specialty"), QStringLiteral("A special agent step needs a specialty.")},
        {QStringLiteral("invalid_native_workflow"),
         QStringLiteral("A special agent step names a native workflow this desktop does not have.")},
        {QStringLiteral("shortcut_needs_file"),
         QStringLiteral("A Run shortcut step needs a file name from .forkmesh/shortcuts.")},
        {QStringLiteral("invalid_output_path"),
         QStringLiteral("Save output can only write inside the repository, outside .git and .forkmesh.")},
        {QStringLiteral("too_many_schedules"), QStringLiteral("A workflow has at most one schedule.")},
        {QStringLiteral("too_many_triggers"), QStringLiteral("A workflow has at most one Run by hand trigger.")},
        {QStringLiteral("too_many_monitors"), QStringLiteral("A workflow has at most 16 log monitors.")},
        {QStringLiteral("invalid_connection"),
         QStringLiteral("A connection points at a missing step, at itself, or into a trigger.")},
        {QStringLiteral("cycle"), QStringLiteral("The steps form a loop. Remove a connection to break it.")},
    };
    return texts.value(code, code);
}

QJsonObject nodeSpec(const QString &type)
{
    QJsonObject result = specs().value(type);
    if (!result.isEmpty())
        result.insert(QStringLiteral("type"), type);
    return result;
}

QJsonObject catalog()
{
    QJsonArray nodes;
    for (const QString &type : specOrder())
        nodes.append(nodeSpec(type));
    return {{QStringLiteral("core"), nodes},
            {QStringLiteral("integrations"), QJsonArray()},
            {QStringLiteral("operators"), QJsonArray()},
            {QStringLiteral("rails"), strings(rails())}};
}

QJsonObject agentParams(const Config &config)
{
    QJsonObject params = config.toJson();
    for (const QString &key : {QStringLiteral("version"), QStringLiteral("id"), QStringLiteral("name"),
                               QStringLiteral("enabled"), QStringLiteral("schedule"),
                               QStringLiteral("logMonitors"), QStringLiteral("createdAtMs"),
                               QStringLiteral("updatedAtMs"), QStringLiteral("lastRunAtMs"),
                               QStringLiteral("lastSessionId"), QStringLiteral("lastTrigger")})
        params.remove(key);
    return params;
}

QString agentStepId(const Workflow &workflow, const QString &nodeId)
{
    return nodeId == workflow.primaryAgent()
               ? workflow.id
               : forkmesh::teammates::Store::safeId(workflow.id + QLatin1Char('-') + nodeId);
}

bool runsAgent(const Workflow &workflow, const QString &agentId)
{
    if (agentId.isEmpty())
        return false;
    for (const QString &nodeId : workflow.nodesOfType(kAgent))
        if (agentStepId(workflow, nodeId) == agentId)
            return true;
    return false;
}

Config agentConfig(const Workflow &workflow, const QString &nodeId, QString *error)
{
    const QJsonObject node = workflow.node(nodeId);
    QJsonObject object = node.value(QStringLiteral("params")).toObject();
    const bool primary = nodeId == workflow.primaryAgent();
    object.insert(QStringLiteral("id"), agentStepId(workflow, nodeId));
    object.insert(QStringLiteral("name"), primary && !workflow.name.isEmpty() ? workflow.name : text(node, "name"));
    if (text(object, "avatar").isEmpty())
        object.insert(QStringLiteral("avatar"), workflow.icon.isEmpty() ? QStringLiteral("sol") : workflow.icon);
    if (text(object, "description").isEmpty())
        object.insert(QStringLiteral("description"), workflow.description);
    object.insert(QStringLiteral("enabled"), workflow.enabled && !node.value(QStringLiteral("disabled")).toBool());
    object.insert(QStringLiteral("skills"), strings(lines(object.value(QStringLiteral("skills")))));
    object.insert(QStringLiteral("skillRefs"), strings(lines(object.value(QStringLiteral("skillRefs")))));
    object.insert(QStringLiteral("improvementNotes"),
                  strings(lines(object.value(QStringLiteral("improvementNotes")))));
    object.insert(QStringLiteral("marketingChannels"),
                  strings(lines(object.value(QStringLiteral("marketingChannels")))));
    object.insert(QStringLiteral("permissions"), strings(lines(object.value(QStringLiteral("permissions")))));
    object.insert(QStringLiteral("selfImproving"), flag(object.value(QStringLiteral("selfImproving")), true));
    object.insert(QStringLiteral("lowCostModel"), flag(object.value(QStringLiteral("lowCostModel")), false));
    object.insert(QStringLiteral("runWindowHours"), number(object.value(QStringLiteral("runWindowHours")), 0));
    object.insert(QStringLiteral("improvementCycles"),
                  number(object.value(QStringLiteral("improvementCycles")), 0));
    if (primary) {
        const QJsonObject schedule = workflow.scheduleNode();
        if (!schedule.isEmpty()) {
            QJsonObject encoded = scheduleJson(schedule.value(QStringLiteral("params")).toObject());
            if (schedule.value(QStringLiteral("disabled")).toBool())
                encoded.insert(QStringLiteral("active"), false);
            object.insert(QStringLiteral("schedule"), encoded);
        }
        QJsonArray monitors;
        for (const QString &id : workflow.nodesOfType(kMonitor)) {
            const QJsonObject monitor = workflow.node(id);
            QJsonObject encoded = monitorJson(monitor.value(QStringLiteral("params")).toObject());
            if (monitor.value(QStringLiteral("disabled")).toBool())
                encoded.insert(QStringLiteral("active"), false);
            monitors.append(encoded);
        }
        object.insert(QStringLiteral("logMonitors"), monitors);
        object.insert(QStringLiteral("createdAtMs"), double(workflow.createdAtMs));
        object.insert(QStringLiteral("updatedAtMs"), double(workflow.updatedAtMs));
    }
    return Config::fromJson(object, error);
}

Config scheduleConfig(const Workflow &workflow, qint64 lastRunAtMs)
{
    Config config;
    config.id = workflow.id;
    config.name = workflow.name;
    config.specialty = QStringLiteral("Workflow");
    config.enabled = workflow.enabled;
    config.lastRunAtMs = lastRunAtMs;
    const QJsonObject schedule = workflow.scheduleNode();
    if (!schedule.isEmpty()) {
        config.schedule =
            forkmesh::teammates::Schedule::fromJson(scheduleJson(schedule.value(QStringLiteral("params")).toObject()));
        if (schedule.value(QStringLiteral("disabled")).toBool())
            config.schedule.active = false;
    }
    return config;
}

bool toTeammate(const Workflow &workflow, Config *config, QString *error)
{
    const QString agent = workflow.primaryAgent();
    if (agent.isEmpty()) {
        if (error)
            error->clear();
        return false;
    }
    QString problem;
    const Config parsed = agentConfig(workflow, agent, &problem);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    if (config)
        *config = parsed;
    if (error)
        error->clear();
    return true;
}

void applyTeammate(Workflow *workflow, const Config &config)
{
    if (!workflow)
        return;
    workflow->id = config.id;
    workflow->name = config.name;
    workflow->icon = config.avatar;
    workflow->description = config.description;
    workflow->enabled = config.enabled;
    workflow->createdAtMs = config.createdAtMs;
    workflow->updatedAtMs = config.updatedAtMs;
    const bool fresh = workflow->nodes.isEmpty();
    QString agent = workflow->primaryAgent();
    if (agent.isEmpty()) {
        agent = uniqueNodeId(*workflow, QStringLiteral("agent"));
        workflow->nodes.append(makeNode(agent, kAgent, uniqueNodeName(*workflow, config.name), 1, 0, {}));
        for (const QJsonValue &value : std::as_const(workflow->nodes)) {
            const QJsonObject node = value.toObject();
            if (isTriggerType(text(node, "type")))
                workflow->connections.append(connection(text(node, "id"), agent));
        }
    }
    QJsonObject agentNode = workflow->node(agent);
    const QString name = config.name.left(kMaxNodeName);
    agentNode.insert(QStringLiteral("name"),
                     text(agentNode, "name") == name ? name : uniqueNodeName(*workflow, name));
    agentNode.insert(QStringLiteral("params"), agentParams(config));
    setNode(workflow, agentNode);

    QJsonObject schedule = workflow->scheduleNode();
    if (schedule.isEmpty()) {
        schedule = makeNode(uniqueNodeId(*workflow, QStringLiteral("schedule")), kSchedule,
                            uniqueNodeName(*workflow, QStringLiteral("Schedule")), 0, 0, {});
    }
    schedule.insert(QStringLiteral("params"), scheduleParams(config.schedule));
    schedule.remove(QStringLiteral("disabled"));
    setNode(workflow, schedule);
    if (!connected(workflow->connections, text(schedule, "id"), agent))
        workflow->connections.append(connection(text(schedule, "id"), agent));

    for (const QString &id : workflow->nodesOfType(kMonitor))
        removeNode(workflow, id);
    int row = 1;
    for (const forkmesh::teammates::LogMonitor &monitor : config.logMonitors) {
        const QString id = uniqueNodeId(*workflow, QStringLiteral("monitor%1").arg(row));
        workflow->nodes.append(makeNode(id, kMonitor,
                                        uniqueNodeName(*workflow, QStringLiteral("Log monitor %1").arg(row)), 0,
                                        row, monitorParams(monitor)));
        workflow->connections.append(connection(id, agent));
        ++row;
    }
    if (fresh) {
        const QString output = uniqueNodeId(*workflow, QStringLiteral("output"));
        workflow->nodes.append(makeNode(output, kOutput, uniqueNodeName(*workflow, QStringLiteral("Save the report")),
                                        2, 0, {{QStringLiteral("title"), QString()},
                                               {QStringLiteral("path"), QString()}}));
        workflow->connections.append(connection(agent, output));
    }
}

Workflow fromTeammate(const Config &config)
{
    Workflow workflow;
    applyTeammate(&workflow, config);
    return workflow;
}

QJsonObject withoutBookkeeping(QJsonObject object)
{
    for (const QString &key : {QStringLiteral("improvementCycles"), QStringLiteral("updatedAtMs")})
        object.remove(key);
    if (!object.value(QStringLiteral("nodes")).isArray())
        return object;
    QJsonArray nodes;
    for (const QJsonValue &value : object.value(QStringLiteral("nodes")).toArray()) {
        QJsonObject node = value.toObject();
        QJsonObject params = node.value(QStringLiteral("params")).toObject();
        params.remove(QStringLiteral("improvementCycles"));
        node.insert(QStringLiteral("params"), params);
        nodes.append(node);
    }
    object.insert(QStringLiteral("nodes"), nodes);
    return object;
}

QList<Listing> listRepositoryWorkflows(const QString &checkout)
{
    QList<Listing> listings;
    if (checkout.isEmpty())
        return listings;
    QSet<QString> ids;
    const QDir root(checkout);
    const QFileInfoList files = QDir(root.filePath(kFolder))
                                    .entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::Readable,
                                                   QDir::Name);
    for (const QFileInfo &info : files) {
        Listing listing;
        listing.path = info.absoluteFilePath();
        QFile file(listing.path);
        QJsonParseError parseError;
        const QJsonDocument document = file.open(QIODevice::ReadOnly)
                                           ? QJsonDocument::fromJson(file.readAll(), &parseError)
                                           : QJsonDocument();
        if (!document.isObject()) {
            listing.workflow.id = forkmesh::teammates::Store::safeId(info.completeBaseName());
            listing.workflow.name = info.completeBaseName();
            listing.error = QStringLiteral("%1 is not valid JSON.").arg(info.fileName());
        } else {
            QString problem;
            listing.workflow = Workflow::fromJson(document.object(), &problem);
            if (listing.workflow.id.isEmpty())
                listing.workflow.id = forkmesh::teammates::Store::safeId(info.completeBaseName());
            if (listing.workflow.name.isEmpty())
                listing.workflow.name = info.completeBaseName();
            const QString again = problem.isEmpty() ? QString() : validate(listing.workflow);
            listing.error = again.isEmpty() ? QString() : errorText(again);
        }
        if (ids.contains(listing.workflow.id))
            continue;
        ids.insert(listing.workflow.id);
        listings.append(listing);
    }
    const QFileInfoList legacy = QDir(root.filePath(kLegacyFolder))
                                     .entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::Readable,
                                                    QDir::Name);
    for (const QFileInfo &info : legacy) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        Listing listing;
        listing.path = info.absoluteFilePath();
        listing.legacy = true;
        if (looksLikeWorkflowFile(object)) {
            QString problem;
            listing.workflow = Workflow::fromJson(object, &problem);
            listing.error = problem.isEmpty() ? QString() : errorText(problem);
        } else {
            QString problem;
            const Config config = Config::fromJson(object, &problem);
            if (!problem.isEmpty())
                continue;
            listing.workflow = fromTeammate(config);
        }
        if (listing.workflow.id.isEmpty() || ids.contains(listing.workflow.id))
            continue;
        ids.insert(listing.workflow.id);
        listings.append(listing);
    }
    return listings;
}

QString approvalDigest(const QString &checkout, const QString &path)
{
    QFile file(path);
    if (checkout.isEmpty() || !file.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return {};
    const QJsonObject object = withoutBookkeeping(document.object());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QJsonDocument(object).toJson(QJsonDocument::Compact));
    const QDir shortcuts(QDir(checkout).filePath(QStringLiteral(".forkmesh/shortcuts")));
    for (const QJsonValue &value : object.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("type")).toString() != kShortcut)
            continue;
        const QString name =
            QFileInfo(node.value(QStringLiteral("params")).toObject().value(QStringLiteral("file")).toString())
                .fileName();
        hash.addData(QByteArray(1, '\0'));
        hash.addData(name.toUtf8());
        hash.addData(QByteArray(1, '\0'));
        QFile script(shortcuts.filePath(name));
        if (!name.isEmpty() && script.open(QIODevice::ReadOnly))
            hash.addData(script.readAll());
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString approvalSettingsKey(const QString &repository, const QString &checkout, const QString &path)
{
    const QString relative = QDir(checkout).relativeFilePath(QFileInfo(path).absoluteFilePath());
    const QByteArray identity = (repository.trimmed().toLower() + QLatin1Char('\n') + relative).toUtf8();
    return QStringLiteral("repoWorkflows/approved/%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(32)));
}

bool isApproved(const QSettings &settings, const QString &repository, const QString &checkout,
                const QString &path)
{
    const QString digest = approvalDigest(checkout, path);
    return !digest.isEmpty() &&
           settings.value(approvalSettingsKey(repository, checkout, path)).toString() == digest;
}

bool approve(QSettings &settings, const QString &repository, const QString &checkout, const QString &path,
             const QString &expectedDigest)
{
    const QString digest = approvalDigest(checkout, path);
    if (digest.isEmpty() || (!expectedDigest.isEmpty() && digest != expectedDigest))
        return false;
    settings.setValue(approvalSettingsKey(repository, checkout, path), digest);
    return true;
}

QString writeWorkflow(const QString &checkout, Workflow workflow, qint64 nowMs, QString *path)
{
    if (checkout.isEmpty())
        return QStringLiteral("This repository has no local checkout.");
    if (workflow.createdAtMs <= 0)
        workflow.createdAtMs = nowMs;
    workflow.updatedAtMs = nowMs;
    const QString problem = validate(workflow);
    if (!problem.isEmpty())
        return errorText(problem);
    const QString folder = QDir(checkout).filePath(kFolder);
    if (!QDir().mkpath(folder))
        return QStringLiteral("Could not create %1.").arg(kFolder);
    const QString target = QDir(folder).filePath(workflow.id + QStringLiteral(".json"));
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QStringLiteral("Could not write %1.").arg(QFileInfo(target).fileName());
    file.write(QJsonDocument(workflow.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return QStringLiteral("Could not finish saving %1.").arg(QFileInfo(target).fileName());
    const QString legacy = QDir(QDir(checkout).filePath(kLegacyFolder)).filePath(workflow.id + QStringLiteral(".json"));
    if (QFileInfo(legacy).isFile())
        QFile::remove(legacy);
    if (path)
        *path = target;
    return {};
}

bool removeWorkflowFiles(const QString &checkout, const QString &workflowId, QString *error)
{
    const QString id = forkmesh::teammates::Store::safeId(workflowId);
    bool found = false;
    for (const QString &folder : {kFolder, kLegacyFolder}) {
        if (checkout.isEmpty() || id.isEmpty())
            break;
        const QString target = QDir(QDir(checkout).filePath(folder)).filePath(id + QStringLiteral(".json"));
        if (!QFileInfo(target).isFile())
            continue;
        found = true;
        if (!QFile::remove(target)) {
            if (error)
                *error = QStringLiteral("Could not delete %1.").arg(QFileInfo(target).fileName());
            return false;
        }
    }
    if (!found && error)
        *error = QStringLiteral("That workflow no longer exists.");
    return found;
}

QList<QPair<QString, QString>> avatarChoices()
{
    return {{QStringLiteral("cfo"), QStringLiteral("CFO · graphite & emerald")},
            {QStringLiteral("code-steward"), QStringLiteral("Steward · indigo & silver")},
            {QStringLiteral("marketing"), QStringLiteral("Marketing · plum & coral")},
            {QStringLiteral("mobile-developer"), QStringLiteral("Mobile · teal & lime")},
            {QStringLiteral("quality"), QStringLiteral("Quality · violet & cyan")},
            {QStringLiteral("ux"), QStringLiteral("UX · midnight & teal")},
            {QStringLiteral("ceo"), QStringLiteral("CEO · gold & onyx")},
            {QStringLiteral("sage"), QStringLiteral("Sage · emerald")},
            {QStringLiteral("coo"), QStringLiteral("COO · crimson")},
            {QStringLiteral("cpa"), QStringLiteral("CPA · champagne")},
            {QStringLiteral("fixer"), QStringLiteral("Fixer · copper")},
            {QStringLiteral("tempo"), QStringLiteral("Tempo · chartreuse")},
            {QStringLiteral("reaper"), QStringLiteral("Reaper · scarlet")},
            {QStringLiteral("sales"), QStringLiteral("Sales · orchid")},
            {QStringLiteral("yc-news"), QStringLiteral("YC News · amber")},
            {QStringLiteral("sentinel"), QStringLiteral("Sentinel · cobalt")},
            {QStringLiteral("hearth"), QStringLiteral("Hearth · ember & rose")},
            {QStringLiteral("reddit"), QStringLiteral("Reddit · orange & ivory")},
            {QStringLiteral("blogger"), QStringLiteral("Blogger · teal & ivory")},
            {QStringLiteral("herald"), QStringLiteral("Herald · midnight & silver")},
            {QStringLiteral("crawler"), QStringLiteral("Crawler · lime & graphite")},
            {QStringLiteral("genie"), QStringLiteral("Genie · spider")},
            {QStringLiteral("sol"), QStringLiteral("Sol · gold")},
            {QStringLiteral("terra"), QStringLiteral("Terra · cyan")},
            {QStringLiteral("luna"), QStringLiteral("Luna · blue")},
            {QStringLiteral("opus"), QStringLiteral("Opus · magenta")},
            {QStringLiteral("fable"), QStringLiteral("Fable · violet")}};
}

QList<QPair<QString, QString>> iconChoices()
{
    return {{QStringLiteral("workflow"), QStringLiteral("Workflow")},
            {QStringLiteral("terminal"), QStringLiteral("Terminal")},
            {QStringLiteral("rocket"), QStringLiteral("Rocket")},
            {QStringLiteral("server"), QStringLiteral("Server")},
            {QStringLiteral("package"), QStringLiteral("Package")},
            {QStringLiteral("graph"), QStringLiteral("Graph")},
            {QStringLiteral("pulse"), QStringLiteral("Pulse")},
            {QStringLiteral("calendar"), QStringLiteral("Calendar")},
            {QStringLiteral("clock"), QStringLiteral("Clock")},
            {QStringLiteral("sparkle"), QStringLiteral("Sparkle")},
            {QStringLiteral("database"), QStringLiteral("Database")},
            {QStringLiteral("file"), QStringLiteral("File")}};
}

bool isIconChoice(const QString &key)
{
    for (const auto &choice : iconChoices())
        if (choice.first == key)
            return true;
    return key == QLatin1String("command");
}

Workflow blank(const QString &id, const QString &name)
{
    Workflow workflow;
    workflow.id = forkmesh::teammates::Store::safeId(id.isEmpty() ? name : id);
    workflow.name = name.trimmed().left(80);
    workflow.icon = QStringLiteral("workflow");
    workflow.nodes.append(makeNode(QStringLiteral("run"), kManual, QStringLiteral("Run by hand"), 0, 0, {}));
    return workflow;
}

Workflow shortcutWorkflow(const QString &id, const QString &name, const QString &shortcutFile)
{
    Workflow workflow = blank(id, name);
    workflow.icon = QStringLiteral("command");
    workflow.nodes.append(makeNode(QStringLiteral("shortcut"), kShortcut,
                                   QFileInfo(shortcutFile).completeBaseName().left(kMaxNodeName), 1, 0,
                                   {{QStringLiteral("file"), QFileInfo(shortcutFile).fileName()},
                                    {QStringLiteral("wait"), true},
                                    {QStringLiteral("allowEdits"), false}}));
    workflow.connections.append(connection(QStringLiteral("run"), QStringLiteral("shortcut")));
    workflow.nodes.append(makeNode(QStringLiteral("output"), kOutput, QStringLiteral("Save the report"), 2, 0,
                                   {{QStringLiteral("title"), QString()}, {QStringLiteral("path"), QString()}}));
    workflow.connections.append(connection(QStringLiteral("shortcut"), QStringLiteral("output")));
    return workflow;
}

QString uniqueNodeId(const Workflow &workflow, const QString &base)
{
    QString clean;
    for (const QChar ch : base)
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_'))
            clean.append(ch);
    clean = clean.left(28);
    if (clean.isEmpty())
        clean = QStringLiteral("step");
    QString candidate = clean;
    for (int suffix = 2; workflow.indexOf(candidate) >= 0; ++suffix)
        candidate = clean + QString::number(suffix);
    return candidate;
}

QString uniqueNodeName(const Workflow &workflow, const QString &base)
{
    const QString clean = base.trimmed().isEmpty() ? QStringLiteral("Step") : base.trimmed().left(kMaxNodeName - 4);
    const auto taken = [&workflow](const QString &name) {
        for (const QJsonValue &value : workflow.nodes)
            if (text(value.toObject(), "name") == name)
                return true;
        return false;
    };
    QString candidate = clean;
    for (int suffix = 2; taken(candidate); ++suffix)
        candidate = QStringLiteral("%1 %2").arg(clean).arg(suffix);
    return candidate;
}

QString safeRepositoryPath(const QString &path)
{
    const QString trimmed = QDir::fromNativeSeparators(path.trimmed());
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('/')) || trimmed.startsWith(QLatin1Char('~')) ||
        trimmed.contains(QLatin1Char(':')) || trimmed.contains(QChar(0)))
        return {};
    const QString clean = QDir::cleanPath(trimmed);
    if (clean == QLatin1String(".") || clean.startsWith(QLatin1String("../")) || clean == QLatin1String("..") ||
        clean.split(QLatin1Char('/')).contains(QStringLiteral("..")))
        return {};
    const QString first = clean.section(QLatin1Char('/'), 0, 0);
    if (first == QLatin1String(".git") || first == QLatin1String(".forkmesh"))
        return {};
    if (clean.size() > 240)
        return {};
    return clean;
}

QJsonObject StepRun::toJson() const
{
    QJsonObject result{{QStringLiteral("nodeId"), nodeId},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("type"), type},
                       {QStringLiteral("status"), status},
                       {QStringLiteral("startedAtMs"), double(startedAtMs)},
                       {QStringLiteral("finishedAtMs"), double(finishedAtMs)}};
    if (sessionId > 0)
        result.insert(QStringLiteral("sessionId"), sessionId);
    if (!output.isEmpty())
        result.insert(QStringLiteral("output"), output.left(kMaxStepOutputChars));
    if (!error.isEmpty())
        result.insert(QStringLiteral("error"), error.left(2000));
    if (!logs.isEmpty())
        result.insert(QStringLiteral("logs"), strings(logs.mid(qMax(0, int(logs.size()) - 40))));
    return result;
}

StepRun StepRun::fromJson(const QJsonObject &object)
{
    StepRun result;
    result.nodeId = text(object, "nodeId");
    result.name = text(object, "name");
    result.type = text(object, "type");
    result.status = object.value(QStringLiteral("status")).toString(kStatusQueued);
    result.startedAtMs = qint64(object.value(QStringLiteral("startedAtMs")).toDouble());
    result.finishedAtMs = qint64(object.value(QStringLiteral("finishedAtMs")).toDouble());
    result.sessionId = object.value(QStringLiteral("sessionId")).toInt();
    result.output = text(object, "output");
    result.error = text(object, "error");
    for (const QJsonValue &value : object.value(QStringLiteral("logs")).toArray())
        result.logs.append(value.toString());
    return result;
}

QJsonObject Run::toJson() const
{
    QJsonArray encodedSteps;
    for (const StepRun &entry : steps)
        encodedSteps.append(entry.toJson());
    QJsonObject result{{QStringLiteral("id"), id},
                       {QStringLiteral("workflowId"), workflowId},
                       {QStringLiteral("workflowName"), workflowName},
                       {QStringLiteral("repo"), repo},
                       {QStringLiteral("trigger"), trigger},
                       {QStringLiteral("taskType"), taskType},
                       {QStringLiteral("status"), status},
                       {QStringLiteral("startedAtMs"), double(startedAtMs)},
                       {QStringLiteral("finishedAtMs"), double(finishedAtMs)},
                       {QStringLiteral("steps"), encodedSteps},
                       {QStringLiteral("output"), output.left(kMaxStepOutputChars)},
                       {QStringLiteral("caption"), caption},
                       {QStringLiteral("outputFile"), outputFile}};
    if (!assignment.isEmpty())
        result.insert(QStringLiteral("assignment"), assignment.left(4000));
    if (!error.isEmpty())
        result.insert(QStringLiteral("error"), error.left(2000));
    return result;
}

Run Run::fromJson(const QJsonObject &object)
{
    Run result;
    result.id = text(object, "id");
    result.workflowId = text(object, "workflowId");
    result.workflowName = text(object, "workflowName");
    result.repo = text(object, "repo");
    result.trigger = text(object, "trigger");
    result.taskType = text(object, "taskType");
    result.assignment = text(object, "assignment");
    result.status = object.value(QStringLiteral("status")).toString(kStatusError);
    result.startedAtMs = qint64(object.value(QStringLiteral("startedAtMs")).toDouble());
    result.finishedAtMs = qint64(object.value(QStringLiteral("finishedAtMs")).toDouble());
    for (const QJsonValue &value : object.value(QStringLiteral("steps")).toArray())
        result.steps.append(StepRun::fromJson(value.toObject()));
    result.output = text(object, "output");
    result.caption = text(object, "caption");
    result.outputFile = text(object, "outputFile");
    result.error = text(object, "error");
    return result;
}

StepRun *Run::step(const QString &nodeId)
{
    for (StepRun &entry : steps)
        if (entry.nodeId == nodeId)
            return &entry;
    return nullptr;
}

const StepRun *Run::step(const QString &nodeId) const
{
    for (const StepRun &entry : steps)
        if (entry.nodeId == nodeId)
            return &entry;
    return nullptr;
}

bool Run::finished() const
{
    return status == kStatusSuccess || status == kStatusError || status == kStatusStopped;
}

int Run::sessionId() const
{
    for (auto it = steps.crbegin(); it != steps.crend(); ++it)
        if (it->sessionId > 0)
            return it->sessionId;
    return 0;
}

QString runMarkdown(const Run &run)
{
    QStringList parts;
    parts << QStringLiteral("# %1").arg(run.workflowName.isEmpty() ? run.workflowId : run.workflowName);
    QStringList facts;
    if (!run.repo.isEmpty())
        facts << QStringLiteral("- Repository: %1").arg(run.repo);
    if (!run.trigger.isEmpty())
        facts << QStringLiteral("- Trigger: %1").arg(run.trigger);
    facts << QStringLiteral("- Started: %1").arg(isoTime(run.startedAtMs));
    if (run.finishedAtMs > 0)
        facts << QStringLiteral("- Finished: %1 (%2)")
                     .arg(isoTime(run.finishedAtMs), durationText(run.finishedAtMs - run.startedAtMs));
    facts << QStringLiteral("- Result: %1").arg(statusWord(run.status));
    if (!run.caption.isEmpty())
        facts << QStringLiteral("- Caption: %1").arg(run.caption);
    parts << facts.join(QLatin1Char('\n'));
    if (!run.assignment.trimmed().isEmpty())
        parts << QStringLiteral("## Assignment\n\n%1").arg(run.assignment.trimmed());
    for (const StepRun &entry : run.steps) {
        if (isTriggerType(entry.type))
            continue;
        QString heading = QStringLiteral("## %1 (%2").arg(entry.name, statusWord(entry.status));
        if (entry.finishedAtMs > 0 && entry.startedAtMs > 0)
            heading += QStringLiteral(", ") + durationText(entry.finishedAtMs - entry.startedAtMs);
        if (entry.sessionId > 0)
            heading += QStringLiteral(", agent session #%1").arg(entry.sessionId);
        heading += QLatin1Char(')');
        QString body = entry.output.trimmed();
        if (!entry.error.trimmed().isEmpty())
            body = (body.isEmpty() ? QString() : body + QStringLiteral("\n\n")) +
                   QStringLiteral("Error: ") + entry.error.trimmed();
        parts << (body.isEmpty() ? heading : heading + QStringLiteral("\n\n") + body);
    }
    return parts.join(QStringLiteral("\n\n")) + QLatin1Char('\n');
}

QString runSummaryText(const Run &run)
{
    if (!run.caption.trimmed().isEmpty())
        return run.caption.trimmed();
    if (run.status == kStatusError && !run.error.trimmed().isEmpty())
        return run.error.trimmed().section(QLatin1Char('\n'), 0, 0).left(200);
    for (const QString &line : run.output.split(QLatin1Char('\n'))) {
        QString clean = line.trimmed();
        while (clean.startsWith(QLatin1Char('#')))
            clean.remove(0, 1);
        clean = clean.trimmed();
        if (!clean.isEmpty())
            return clean.left(200);
    }
    return statusWord(run.status);
}

QList<Run> loadRuns(const QString &file)
{
    QList<Run> runs;
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly))
        return runs;
    const QJsonDocument document = QJsonDocument::fromJson(handle.readAll());
    for (const QJsonValue &value : document.array()) {
        const Run run = Run::fromJson(value.toObject());
        if (!run.id.isEmpty())
            runs.append(run);
    }
    std::sort(runs.begin(), runs.end(),
              [](const Run &left, const Run &right) { return left.startedAtMs > right.startedAtMs; });
    return runs;
}

bool saveRun(const QString &file, const Run &run, QStringList *droppedRunIds)
{
    if (file.isEmpty() || run.id.isEmpty())
        return false;
    QList<Run> runs = loadRuns(file);
    bool replaced = false;
    for (Run &existing : runs) {
        if (existing.id == run.id) {
            existing = run;
            replaced = true;
        }
    }
    if (!replaced)
        runs.prepend(run);
    std::sort(runs.begin(), runs.end(),
              [](const Run &left, const Run &right) { return left.startedAtMs > right.startedAtMs; });
    while (runs.size() > kMaxRunsKept) {
        if (droppedRunIds)
            droppedRunIds->append(runs.constLast().id);
        runs.removeLast();
    }
    QJsonArray encoded;
    for (const Run &entry : std::as_const(runs))
        encoded.append(entry.toJson());
    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return false;
    QSaveFile handle(file);
    if (!handle.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    handle.write(QJsonDocument(encoded).toJson(QJsonDocument::Compact));
    return handle.commit();
}

QStringList rails()
{
    return {kRailTop, kRailLeft, kRailRight, kRailBottom};
}

QString railLabel(const QString &rail)
{
    if (rail == kRailTop)
        return QStringLiteral("Top rail");
    if (rail == kRailLeft)
        return QStringLiteral("Left rail");
    if (rail == kRailRight)
        return QStringLiteral("Right rail");
    if (rail == kRailBottom)
        return QStringLiteral("Bottom rail");
    return {};
}

bool isRail(const QString &rail)
{
    return rails().contains(rail);
}

QList<Pin> parsePins(const QString &json)
{
    QList<Pin> pins;
    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QJsonObject object = value.toObject();
        Pin pin{text(object, "repo").trimmed(), forkmesh::teammates::Store::safeId(text(object, "workflow")),
                text(object, "rail")};
        if (pin.repo.isEmpty() || pin.workflowId.isEmpty() || !isRail(pin.rail) || pins.contains(pin))
            continue;
        pins.append(pin);
        if (pins.size() >= kMaxPins)
            break;
    }
    return pins;
}

QString serializePins(const QList<Pin> &pins)
{
    QJsonArray encoded;
    for (const Pin &pin : pins)
        encoded.append(QJsonObject{{QStringLiteral("repo"), pin.repo},
                                   {QStringLiteral("workflow"), pin.workflowId},
                                   {QStringLiteral("rail"), pin.rail}});
    return QString::fromUtf8(QJsonDocument(encoded).toJson(QJsonDocument::Compact));
}

QList<Pin> pinWorkflow(QList<Pin> pins, const Pin &pin)
{
    if (pin.repo.trimmed().isEmpty() || pin.workflowId.isEmpty() || !isRail(pin.rail))
        return pins;
    for (Pin &existing : pins) {
        if (existing == pin) {
            existing.rail = pin.rail;
            return pins;
        }
    }
    if (pins.size() < kMaxPins)
        pins.append(pin);
    return pins;
}

QList<Pin> movePin(QList<Pin> pins, const Pin &pin, int slot)
{
    if (pin.repo.trimmed().isEmpty() || pin.workflowId.isEmpty() || !isRail(pin.rail))
        return pins;
    Pin moved = pin;
    const int existing = pins.indexOf(pin);
    if (existing >= 0) {
        moved.repo = pins.at(existing).repo;
        pins.removeAt(existing);
    } else if (pins.size() >= kMaxPins) {
        return pins;
    }
    moved.rail = pin.rail;
    int at = pins.size();
    int seen = 0;
    for (int i = 0; i < pins.size(); ++i) {
        if (pins.at(i).rail != pin.rail)
            continue;
        if (seen++ == slot) {
            at = i;
            break;
        }
        at = i + 1;
    }
    pins.insert(at, moved);
    return pins;
}

QList<Pin> unpinWorkflow(QList<Pin> pins, const QString &repo, const QString &workflowId)
{
    pins.removeIf([&](const Pin &pin) { return pin == Pin{repo, workflowId, QString()}; });
    return pins;
}

QString pinnedRail(const QList<Pin> &pins, const QString &repo, const QString &workflowId)
{
    for (const Pin &pin : pins)
        if (pin == Pin{repo, workflowId, QString()})
            return pin.rail;
    return {};
}

} // namespace forkmesh::repoworkflows

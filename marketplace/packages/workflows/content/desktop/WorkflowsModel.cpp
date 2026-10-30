#include "WorkflowsModel.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace forkmesh::workflows {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

qint64 integer(const QJsonObject &object, const char *key)
{
    return qint64(object.value(QLatin1String(key)).toDouble());
}

QString paramText(const QJsonObject &node, const QString &key)
{
    const QJsonValue value = node.value(QStringLiteral("params")).toObject().value(key);
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble());
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

QJsonObject connection(const QString &from, const QString &output, const QString &to)
{
    return QJsonObject{{QStringLiteral("from"), from},
                       {QStringLiteral("output"), output.isEmpty() ? QStringLiteral("main") : output},
                       {QStringLiteral("to"), to}};
}

bool sameConnection(const QJsonObject &a, const QJsonObject &b)
{
    return text(a, "from") == text(b, "from") && text(a, "to") == text(b, "to") &&
           (text(a, "output").isEmpty() ? QStringLiteral("main") : text(a, "output")) ==
               (text(b, "output").isEmpty() ? QStringLiteral("main") : text(b, "output"));
}

} // namespace

Stats computeStats(const QJsonArray &workflows, const QJsonArray &runs, qint64 nowMs)
{
    Stats stats;
    for (const QJsonValue &value : workflows) {
        const QJsonObject workflow = value.toObject();
        ++stats.workflows;
        const bool active = workflow.value(QStringLiteral("active")).toBool();
        const QString trigger = text(workflow, "trigger");
        if (active)
            ++stats.active;
        if (trigger == QLatin1String("core.schedule") && active)
            ++stats.scheduled;
        else if (trigger == QLatin1String("core.webhook"))
            ++stats.webhooks;
        else if (trigger == QLatin1String("core.manual"))
            ++stats.manual;
        const QJsonValue nodes = workflow.value(QStringLiteral("nodes"));
        stats.steps += nodes.isArray() ? int(nodes.toArray().size()) : nodes.toInt();
        const qint64 next = integer(workflow, "nextRunAt");
        if (active && next > 0 && (stats.nextRunAt == 0 || next < stats.nextRunAt))
            stats.nextRunAt = next;
        stats.lastRunAt = std::max(stats.lastRunAt, integer(workflow, "lastRunAt"));
    }
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    const qint64 today = QDateTime(now.date(), QTime(0, 0)).toMSecsSinceEpoch();
    qint64 totalMs = 0;
    int timed = 0;
    qint64 lastFailedAt = -1;
    for (const QJsonValue &value : runs) {
        const QJsonObject run = value.toObject();
        ++stats.runs;
        const QString status = text(run, "status");
        const qint64 started = integer(run, "startedAt");
        const qint64 finished = integer(run, "finishedAt");
        if (status == QLatin1String("success"))
            ++stats.succeeded;
        else if (status == QLatin1String("error")) {
            ++stats.failed;
            if (started > lastFailedAt) {
                lastFailedAt = started;
                stats.lastFailedWorkflow = text(run, "workflowName");
            }
        }
        if (started >= today)
            ++stats.runsToday;
        if (started > 0 && finished >= started) {
            totalMs += finished - started;
            ++timed;
        }
        stats.lastRunAt = std::max(stats.lastRunAt, started);
    }
    stats.averageMs = timed ? totalMs / timed : 0;
    return stats;
}

QString successRateText(const Stats &stats)
{
    const int finished = stats.succeeded + stats.failed;
    if (finished == 0)
        return QStringLiteral("–");
    return QString::number(std::lround(100.0 * stats.succeeded / finished)) + QLatin1Char('%');
}

QString triggerLabel(const QString &type)
{
    if (type == QLatin1String("core.manual"))
        return QStringLiteral("Manual");
    if (type == QLatin1String("core.webhook"))
        return QStringLiteral("Webhook");
    if (type == QLatin1String("core.schedule"))
        return QStringLiteral("Schedule");
    if (type.isEmpty())
        return QStringLiteral("No trigger");
    return type;
}

QString statusLabel(const QString &status)
{
    if (status == QLatin1String("success"))
        return QStringLiteral("Succeeded");
    if (status == QLatin1String("error"))
        return QStringLiteral("Failed");
    if (status == QLatin1String("skipped"))
        return QStringLiteral("Skipped");
    if (status == QLatin1String("running"))
        return QStringLiteral("Running");
    if (status == QLatin1String("stopped"))
        return QStringLiteral("Stopped");
    if (status.isEmpty())
        return QStringLiteral("Not run yet");
    return status;
}

QString errorText(const QString &code, const QString &fallback)
{
    static const QHash<QString, QString> messages{
        {QStringLiteral("unauthorized"), QStringLiteral("Sign in to your ForkMesh account on this desktop to use Workflows.")},
        {QStringLiteral("plugin_not_installed"), QStringLiteral("Workflows is not installed on your account. Install it from the Market, then refresh.")},
        {QStringLiteral("not_found"), QStringLiteral("That workflow no longer exists.")},
        {QStringLiteral("too_many_workflows"), QStringLiteral("You already have 50 workflows; delete one first.")},
        {QStringLiteral("too_many_credentials"), QStringLiteral("You already have 50 credentials; delete one first.")},
        {QStringLiteral("invalid_workflow"), QStringLiteral("The workflow could not be read.")},
        {QStringLiteral("too_many_nodes"), QStringLiteral("A workflow holds at most 60 steps.")},
        {QStringLiteral("too_many_connections"), QStringLiteral("A workflow holds at most 120 connections.")},
        {QStringLiteral("invalid_node"), QStringLiteral("A step is missing its id or type.")},
        {QStringLiteral("unknown_node_type"), QStringLiteral("A step uses a type this server does not know.")},
        {QStringLiteral("duplicate_node_name"), QStringLiteral("Two steps share a name; give each step its own name.")},
        {QStringLiteral("value_too_long"), QStringLiteral("A step setting is too long.")},
        {QStringLiteral("invalid_credential"), QStringLiteral("A step points at a credential that is not valid.")},
        {QStringLiteral("too_many_triggers"), QStringLiteral("A workflow has one trigger.")},
        {QStringLiteral("desktop_needs_manual"), QStringLiteral("A workflow with desktop steps must start from Manual run.")},
        {QStringLiteral("desktop_steps_only"), QStringLiteral("A workflow with desktop steps can only hold desktop steps.")},
        {QStringLiteral("invalid_connection"), QStringLiteral("A connection points at a missing step, a trigger or a branch the step does not have.")},
        {QStringLiteral("cycle"), QStringLiteral("The steps form a loop; a step cannot feed into itself.")},
        {QStringLiteral("no_trigger"), QStringLiteral("Add a trigger before running the workflow.")},
        {QStringLiteral("runs_on_desktop"), QStringLiteral("This workflow has desktop steps; run it on this desktop.")},
        {QStringLiteral("rate_limited"), QStringLiteral("Your account ran 120 workflows in the last hour; try again later.")},
        {QStringLiteral("unknown_integration"), QStringLiteral("Choose an integration.")},
        {QStringLiteral("no_credential_needed"), QStringLiteral("That integration needs no credential.")},
        {QStringLiteral("invalid_import_url"), QStringLiteral("Use a public https:// link to an n8n workflow, GitHub file or Gist.")},
        {QStringLiteral("import_fetch_failed"), QStringLiteral("The link could not be downloaded.")},
        {QStringLiteral("document_too_large"), QStringLiteral("The file is too large to import.")},
        {QStringLiteral("invalid_run"), QStringLiteral("The server rejected the desktop run report.")},
        {QStringLiteral("not_a_desktop_workflow"), QStringLiteral("This workflow has no desktop steps.")},
    };
    const auto found = messages.constFind(code);
    if (found != messages.cend())
        return found.value();
    return fallback.isEmpty() ? code : fallback;
}

QString durationText(qint64 ms)
{
    if (ms < 0)
        return QStringLiteral("–");
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    if (ms < 60000)
        return QLocale().toString(ms / 1000.0, 'f', 1) + QStringLiteral(" s");
    const qint64 seconds = ms / 1000;
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg(seconds % 3600 / 60);
}

QString relativeTime(qint64 atMs, qint64 nowMs)
{
    if (atMs <= 0)
        return QStringLiteral("Never");
    const qint64 delta = nowMs - atMs;
    const qint64 minutes = std::llabs(delta) / 60000;
    QString amount;
    if (minutes < 1)
        return delta >= 0 ? QStringLiteral("Just now") : QStringLiteral("In under a minute");
    if (minutes < 60)
        amount = QStringLiteral("%1 min").arg(minutes);
    else if (minutes < 48 * 60)
        amount = QStringLiteral("%1 h").arg(minutes / 60);
    else if (minutes < 60 * 24 * 30)
        amount = QStringLiteral("%1 d").arg(minutes / (60 * 24));
    else
        return QLocale().toString(QDateTime::fromMSecsSinceEpoch(atMs).date(), QLocale::ShortFormat);
    return delta >= 0 ? amount + QStringLiteral(" ago") : QStringLiteral("In ") + amount;
}

QString everyText(int minutes)
{
    if (minutes <= 0)
        return {};
    if (minutes % 1440 == 0)
        return minutes == 1440 ? QStringLiteral("Every day") : QStringLiteral("Every %1 days").arg(minutes / 1440);
    if (minutes % 60 == 0)
        return minutes == 60 ? QStringLiteral("Every hour") : QStringLiteral("Every %1 hours").arg(minutes / 60);
    return QStringLiteral("Every %1 minutes").arg(minutes);
}

QString humanBytes(double bytes)
{
    double size = std::max(0.0, bytes);
    const char *units[] = {"B", "KB", "MB", "GB"};
    for (const char *unit : units) {
        if (size < 1024) {
            if (QLatin1String(unit) == QLatin1String("B"))
                return QStringLiteral("%1 B").arg(qint64(size));
            return QStringLiteral("%1 %2").arg(QString::number(size, 'f', 1), QLatin1String(unit));
        }
        size /= 1024;
    }
    return QStringLiteral("%1 TB").arg(QString::number(size, 'f', 1));
}

QString runOutputText(const QJsonObject &run)
{
    const QJsonObject output = run.value(QStringLiteral("output")).toObject();
    const QString line = text(output, "text").trimmed();
    if (!line.isEmpty())
        return line;
    QStringList metrics;
    for (const QJsonValue &value : output.value(QStringLiteral("metrics")).toArray()) {
        const QJsonObject metric = value.toObject();
        metrics << text(metric, "label") + QStringLiteral(": ") + metric.value(QStringLiteral("value")).toVariant().toString();
    }
    if (!metrics.isEmpty())
        return metrics.join(QStringLiteral(" · "));
    return text(run, "error");
}

QJsonArray mergeRuns(const QHash<QString, QJsonArray> &byWorkflow,
                     const QHash<QString, QString> &names, int limit)
{
    QList<QJsonObject> all;
    for (auto it = byWorkflow.cbegin(); it != byWorkflow.cend(); ++it) {
        for (const QJsonValue &value : it.value()) {
            QJsonObject run = value.toObject();
            run.insert(QStringLiteral("workflowId"), it.key());
            run.insert(QStringLiteral("workflowName"), names.value(it.key(), QStringLiteral("Untitled workflow")));
            all.append(run);
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const qint64 left = integer(a, "startedAt");
        const qint64 right = integer(b, "startedAt");
        if (left != right)
            return left > right;
        return text(a, "id") < text(b, "id");
    });
    QJsonArray merged;
    for (const QJsonObject &run : all) {
        if (limit > 0 && merged.size() >= limit)
            break;
        merged.append(run);
    }
    return merged;
}

QJsonArray unifiedWorkflows(const QJsonArray &account, const QHash<QString, QJsonArray> &runsByWorkflow,
                            const QJsonArray &local, const QJsonArray &pins)
{
    QHash<QString, QString> accountPins;
    for (const QJsonValue &value : pins) {
        const QJsonObject pin = value.toObject();
        if (text(pin, "repo") == QLatin1String(kAccountRepo))
            accountPins.insert(text(pin, "workflow"), text(pin, "rail"));
    }
    QJsonArray entries;
    for (const QJsonValue &value : local) {
        QJsonObject entry = value.toObject();
        const bool agent = entry.value(QStringLiteral("specialAgent")).toBool();
        const QJsonArray recent = entry.value(QStringLiteral("recentRuns")).toArray();
        const QJsonObject last = recent.isEmpty() ? QJsonObject() : recent.first().toObject();
        entry.insert(QStringLiteral("key"), QStringLiteral("local:%1|%2").arg(text(entry, "repo"), text(entry, "id")));
        entry.insert(QStringLiteral("source"), QStringLiteral("local"));
        entry.insert(QStringLiteral("kind"), agent ? QStringLiteral("agent") : QStringLiteral("desktop"));
        entry.insert(QStringLiteral("kindLabel"), agent ? QStringLiteral("Special agent") : QStringLiteral("Desktop workflow"));
        entry.insert(QStringLiteral("place"), text(entry, "repo"));
        entry.insert(QStringLiteral("lastOutput"), text(entry, "lastSummary"));
        entry.insert(QStringLiteral("lastOutputFile"), text(last, "outputFile"));
        entry.insert(QStringLiteral("lastRunId"), text(last, "id"));
        entry.remove(QStringLiteral("recentRuns"));
        entries.append(entry);
    }
    for (const QJsonValue &value : account) {
        const QJsonObject summary = value.toObject();
        const QString id = text(summary, "id");
        const QString type = text(summary, "trigger");
        const bool active = summary.value(QStringLiteral("active")).toBool();
        QString trigger = triggerLabel(type);
        if (type == QLatin1String("core.schedule") && !everyText(int(integer(summary, "scheduleMinutes"))).isEmpty())
            trigger = everyText(int(integer(summary, "scheduleMinutes")));
        const QJsonArray runs = runsByWorkflow.value(id);
        int failed = 0;
        for (const QJsonValue &run : runs)
            failed += text(run.toObject(), "status") == QLatin1String("error") ? 1 : 0;
        const QJsonObject last = runs.isEmpty() ? QJsonObject() : runs.first().toObject();
        QJsonObject entry{{QStringLiteral("key"), QStringLiteral("account:") + id},
                          {QStringLiteral("source"), QStringLiteral("account")},
                          {QStringLiteral("kind"), QStringLiteral("account")},
                          {QStringLiteral("kindLabel"), QStringLiteral("On ForkMesh")},
                          {QStringLiteral("id"), id},
                          {QStringLiteral("repo"), QString::fromLatin1(kAccountRepo)},
                          {QStringLiteral("place"), QStringLiteral("Your account")},
                          {QStringLiteral("name"), text(summary, "name")},
                          {QStringLiteral("trigger"), trigger},
                          {QStringLiteral("triggerType"), type},
                          {QStringLiteral("enabled"), active},
                          {QStringLiteral("hasSchedule"), type == QLatin1String("core.schedule")},
                          {QStringLiteral("scheduleActive"), active && type == QLatin1String("core.schedule")},
                          {QStringLiteral("steps"), summary.value(QStringLiteral("nodes")).toInt()},
                          {QStringLiteral("lastRunAt"), summary.value(QStringLiteral("lastRunAt"))},
                          {QStringLiteral("lastStatus"), text(summary, "lastStatus")},
                          {QStringLiteral("nextRunAt"), active ? summary.value(QStringLiteral("nextRunAt")) : QJsonValue(0)},
                          {QStringLiteral("updatedAt"), summary.value(QStringLiteral("updatedAt"))},
                          {QStringLiteral("pinned"), accountPins.value(id)},
                          {QStringLiteral("lastOutput"), last.isEmpty() ? QString() : runOutputText(last)},
                          {QStringLiteral("lastRunId"), text(last, "id")}};
        if (!runs.isEmpty()) {
            entry.insert(QStringLiteral("runs"), int(runs.size()));
            entry.insert(QStringLiteral("failedRuns"), failed);
        }
        entries.append(entry);
    }
    return entries;
}

bool workflowMatches(const QJsonObject &entry, const QString &needle, const QString &scope)
{
    if (scope == QLatin1String("agent") && text(entry, "kind") != QLatin1String("agent"))
        return false;
    if (scope == QLatin1String("desktop") && text(entry, "source") != QLatin1String("local"))
        return false;
    if (scope == QLatin1String("account") && text(entry, "source") != QLatin1String("account"))
        return false;
    if (scope == QLatin1String("failing") && text(entry, "lastStatus") != QLatin1String("error") &&
        text(entry, "error").isEmpty())
        return false;
    if (scope == QLatin1String("pinned") && text(entry, "pinned").isEmpty())
        return false;
    const QString wanted = needle.trimmed();
    if (wanted.isEmpty())
        return true;
    for (const char *key : {"name", "description", "trigger", "place", "kindLabel", "lastOutput"})
        if (text(entry, key).contains(wanted, Qt::CaseInsensitive))
            return true;
    return false;
}

void sortWorkflows(QJsonArray &entries, const QString &order)
{
    QList<QJsonObject> list;
    for (const QJsonValue &value : std::as_const(entries))
        list.append(value.toObject());
    const auto byName = [](const QJsonObject &a, const QJsonObject &b) {
        return text(a, "name").compare(text(b, "name"), Qt::CaseInsensitive) < 0;
    };
    if (order == QLatin1String("recent")) {
        std::stable_sort(list.begin(), list.end(), [&byName](const QJsonObject &a, const QJsonObject &b) {
            const qint64 left = integer(a, "lastRunAt");
            const qint64 right = integer(b, "lastRunAt");
            return left != right ? left > right : byName(a, b);
        });
    } else if (order == QLatin1String("next")) {
        std::stable_sort(list.begin(), list.end(), [&byName](const QJsonObject &a, const QJsonObject &b) {
            const qint64 left = integer(a, "nextRunAt");
            const qint64 right = integer(b, "nextRunAt");
            if ((left > 0) != (right > 0))
                return left > 0;
            return left != right ? left < right : byName(a, b);
        });
    } else if (order == QLatin1String("kind")) {
        const auto rank = [](const QJsonObject &entry) {
            const QString kind = text(entry, "kind");
            return kind == QLatin1String("agent") ? 0 : kind == QLatin1String("desktop") ? 1 : 2;
        };
        std::stable_sort(list.begin(), list.end(), [&byName, &rank](const QJsonObject &a, const QJsonObject &b) {
            return rank(a) != rank(b) ? rank(a) < rank(b) : byName(a, b);
        });
    } else {
        std::stable_sort(list.begin(), list.end(), byName);
    }
    entries = QJsonArray();
    for (const QJsonObject &entry : std::as_const(list))
        entries.append(entry);
}

QJsonArray statsWorkflows(const QJsonArray &entries)
{
    QJsonArray rows;
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        QString trigger = text(entry, "triggerType");
        if (trigger.isEmpty())
            trigger = entry.value(QStringLiteral("hasSchedule")).toBool() &&
                              entry.value(QStringLiteral("scheduleActive")).toBool()
                          ? QStringLiteral("core.schedule")
                          : QStringLiteral("core.manual");
        rows.append(QJsonObject{{QStringLiteral("name"), text(entry, "name")},
                                {QStringLiteral("active"), entry.value(QStringLiteral("enabled")).toBool()},
                                {QStringLiteral("trigger"), trigger},
                                {QStringLiteral("nodes"), entry.value(QStringLiteral("steps")).toInt()},
                                {QStringLiteral("nextRunAt"), entry.value(QStringLiteral("nextRunAt"))},
                                {QStringLiteral("lastRunAt"), entry.value(QStringLiteral("lastRunAt"))}});
    }
    return rows;
}

QString workflowStateText(const QJsonObject &entry)
{
    if (!text(entry, "error").isEmpty())
        return QStringLiteral("Needs a fix");
    if (entry.value(QStringLiteral("running")).toBool())
        return text(entry, "runningStep").isEmpty() ? QStringLiteral("Running")
                                                    : QStringLiteral("Running · %1").arg(text(entry, "runningStep"));
    if (entry.value(QStringLiteral("agentWorking")).toBool())
        return QStringLiteral("Agent working");
    if (!entry.value(QStringLiteral("enabled")).toBool())
        return QStringLiteral("Off");
    const QString status = text(entry, "lastStatus");
    if (status == QLatin1String("error"))
        return QStringLiteral("Failed");
    if (status == QLatin1String("success"))
        return QStringLiteral("Succeeded");
    if (status == QLatin1String("stopped"))
        return QStringLiteral("Stopped");
    return integer(entry, "lastRunAt") > 0 ? QStringLiteral("On") : QStringLiteral("Not run yet");
}

void Catalog::load(const QJsonObject &catalog)
{
    m_types.clear();
    m_order.clear();
    for (const QJsonValue &value : catalog.value(QStringLiteral("core")).toArray()) {
        const QJsonObject spec = value.toObject();
        const QString type = text(spec, "type");
        if (type.isEmpty())
            continue;
        m_types.insert(type, spec);
        m_order.append(type);
    }
    for (const QJsonValue &value : catalog.value(QStringLiteral("integrations")).toArray()) {
        QJsonObject spec = value.toObject();
        const QString type = text(spec, "type");
        if (type.isEmpty())
            continue;
        spec.insert(QStringLiteral("group"), text(spec, "category"));
        spec.insert(QStringLiteral("app"), true);
        spec.insert(QStringLiteral("outputs"), QJsonArray{QStringLiteral("main")});
        m_types.insert(type, spec);
        m_order.append(type);
    }
    m_operators = catalog.value(QStringLiteral("operators")).toArray();
}

QJsonObject Catalog::spec(const QString &type) const
{
    return m_types.value(type);
}

QString Catalog::name(const QString &type) const
{
    const QString value = text(spec(type), "name");
    return value.isEmpty() ? type : value;
}

QString Catalog::description(const QString &type) const
{
    return text(spec(type), "description");
}

QString Catalog::group(const QString &type) const
{
    return text(spec(type), "group");
}

bool Catalog::isTrigger(const QString &type) const
{
    return spec(type).value(QStringLiteral("trigger")).toBool();
}

bool Catalog::isApp(const QString &type) const
{
    return type.startsWith(QLatin1String("app."));
}

bool Catalog::isDesktop(const QString &type) const
{
    return text(spec(type), "runsOn") == QLatin1String("desktop");
}

QString Catalog::desktopStep(const QString &type) const
{
    return text(spec(type), "desktopStep");
}

QStringList Catalog::outputs(const QString &type) const
{
    QStringList names;
    for (const QJsonValue &value : spec(type).value(QStringLiteral("outputs")).toArray())
        names << value.toString();
    return names.isEmpty() ? QStringList{QStringLiteral("main")} : names;
}

QJsonArray Catalog::operations(const QString &type) const
{
    return spec(type).value(QStringLiteral("operations")).toArray();
}

QJsonArray Catalog::fields(const QString &type, const QString &operation) const
{
    if (!isApp(type))
        return spec(type).value(QStringLiteral("fields")).toArray();
    const QJsonArray ops = operations(type);
    for (const QJsonValue &value : ops) {
        const QJsonObject op = value.toObject();
        if (text(op, "key") == operation)
            return op.value(QStringLiteral("fields")).toArray();
    }
    return ops.isEmpty() ? QJsonArray() : ops.first().toObject().value(QStringLiteral("fields")).toArray();
}

QJsonArray Catalog::credentialFields(const QString &integration) const
{
    return spec(QStringLiteral("app.") + integration).value(QStringLiteral("credential")).toArray();
}

QStringList Catalog::triggerTypes() const
{
    QStringList types;
    for (const QString &type : m_order)
        if (isTrigger(type))
            types << type;
    return types;
}

QStringList Catalog::stepTypes() const
{
    QStringList types;
    for (const QString &type : m_order)
        if (!isTrigger(type))
            types << type;
    return types;
}

QStringList Catalog::integrationKeys() const
{
    QStringList keys;
    for (const QString &type : m_order)
        if (isApp(type) && !credentialFields(type.mid(4)).isEmpty())
            keys << type.mid(4);
    return keys;
}

QString Catalog::summary(const QJsonObject &node) const
{
    const QString type = text(node, "type");
    QStringList parts;
    if (type == QLatin1String("core.http")) {
        const QString method = paramText(node, QStringLiteral("method"));
        parts << QStringList{method.isEmpty() ? QStringLiteral("GET") : method,
                             paramText(node, QStringLiteral("url"))}
                     .join(QLatin1Char(' '))
                     .trimmed();
    } else if (type == QLatin1String("core.if") || type == QLatin1String("core.filter")) {
        QString op = paramText(node, QStringLiteral("operator"));
        for (const QJsonValue &value : m_operators)
            if (text(value.toObject(), "value") == op)
                op = text(value.toObject(), "label");
        QStringList condition{paramText(node, QStringLiteral("left")), op, paramText(node, QStringLiteral("right"))};
        condition.removeAll(QString());
        parts << condition.join(QLatin1Char(' '));
    } else if (type == QLatin1String("core.schedule")) {
        parts << everyText(paramText(node, QStringLiteral("every")).toInt());
    } else if (type == QLatin1String("core.limit")) {
        const QString max = paramText(node, QStringLiteral("max"));
        parts << QStringLiteral("Keep %1").arg(max.isEmpty() ? QStringLiteral("10") : max);
    } else if (type == QLatin1String("core.split")) {
        parts << paramText(node, QStringLiteral("field"));
    } else if (isApp(type)) {
        const QString operation = paramText(node, QStringLiteral("operation"));
        const QJsonArray ops = operations(type);
        QString label = ops.isEmpty() ? QString() : text(ops.first().toObject(), "name");
        for (const QJsonValue &value : ops)
            if (text(value.toObject(), "key") == operation)
                label = text(value.toObject(), "name");
        parts << label;
    } else {
        parts << description(type);
    }
    if (node.value(QStringLiteral("disabled")).toBool())
        parts << QStringLiteral("Off");
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

Draft Draft::blank()
{
    Draft draft;
    draft.name = QStringLiteral("Untitled workflow");
    draft.nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("trigger")},
                                   {QStringLiteral("type"), QStringLiteral("core.manual")},
                                   {QStringLiteral("name"), QStringLiteral("Run manually")},
                                   {QStringLiteral("x"), 0},
                                   {QStringLiteral("y"), 0},
                                   {QStringLiteral("params"), QJsonObject()}});
    return draft;
}

Draft Draft::fromJson(const QJsonObject &workflow)
{
    Draft draft;
    draft.id = text(workflow, "id");
    draft.name = text(workflow, "name");
    if (draft.name.isEmpty())
        draft.name = QStringLiteral("Untitled workflow");
    draft.active = workflow.value(QStringLiteral("active")).toBool();
    const QJsonValue nodes = workflow.value(QStringLiteral("nodes"));
    draft.nodes = nodes.isArray() ? nodes.toArray() : QJsonArray();
    draft.connections = workflow.value(QStringLiteral("connections")).toArray();
    draft.hookPath = text(workflow, "hookPath");
    return draft;
}

Draft Draft::fromLocal(const QJsonObject &workflow, const QString &repo)
{
    Draft draft = fromJson(workflow);
    draft.repo = repo;
    draft.active = workflow.value(QStringLiteral("enabled")).toBool(true);
    draft.icon = text(workflow, "icon");
    draft.description = text(workflow, "description");
    draft.createdAtMs = qint64(workflow.value(QStringLiteral("createdAtMs")).toDouble());
    draft.hookPath.clear();
    return draft;
}

QJsonObject Draft::localJson() const
{
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("forkmesh.workflow")},
                       {QStringLiteral("version"), 1},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("icon"), icon},
                       {QStringLiteral("enabled"), active},
                       {QStringLiteral("createdAtMs"), double(createdAtMs)},
                       {QStringLiteral("nodes"), nodes},
                       {QStringLiteral("connections"), connections}};
}

QString Draft::addTrigger(const Catalog &catalog, const QString &type)
{
    if (!catalog.isTrigger(type))
        return {};
    QJsonObject params;
    for (const QJsonValue &value : catalog.fields(type, QString())) {
        const QJsonObject field = value.toObject();
        const QString fallback = field.value(QStringLiteral("default")).toVariant().toString();
        if (!fallback.isEmpty())
            params.insert(text(field, "key"), fallback);
    }
    QStringList targets;
    qint64 left = 0;
    int triggers = 0;
    for (const QJsonValue &value : nodes) {
        const QJsonObject current = value.toObject();
        left = std::min(left, integer(current, "x"));
        if (catalog.isTrigger(text(current, "type"))) {
            ++triggers;
            continue;
        }
        if (inputs(text(current, "id")).isEmpty())
            targets << text(current, "id");
    }
    if (targets.isEmpty()) {
        for (const QString &candidate : ordered()) {
            if (!catalog.isTrigger(text(node(candidate), "type"))) {
                targets << candidate;
                break;
            }
        }
    }
    const QString triggerId = newId(type);
    nodes.insert(0, QJsonObject{{QStringLiteral("id"), triggerId},
                                {QStringLiteral("type"), type},
                                {QStringLiteral("name"), uniqueName(catalog.name(type))},
                                {QStringLiteral("x"), left - (triggers == 0 ? kColumn : 0)},
                                {QStringLiteral("y"), triggers * kRow},
                                {QStringLiteral("params"), params}});
    for (const QString &target : targets)
        connections.append(connection(triggerId, QStringLiteral("main"), target));
    return triggerId;
}

void Draft::retype(const Catalog &catalog, const QString &nodeId, const QString &type)
{
    const int index = indexOf(nodeId);
    if (index < 0 || !catalog.isTrigger(type))
        return;
    QJsonObject current = nodes.at(index).toObject();
    if (text(current, "type") == type)
        return;
    QJsonObject params;
    for (const QJsonValue &value : catalog.fields(type, QString())) {
        const QJsonObject field = value.toObject();
        const QString fallback = field.value(QStringLiteral("default")).toVariant().toString();
        if (!fallback.isEmpty())
            params.insert(text(field, "key"), fallback);
    }
    current.insert(QStringLiteral("type"), type);
    current.insert(QStringLiteral("params"), params);
    current.insert(QStringLiteral("name"), uniqueName(catalog.name(type), nodeId));
    nodes.replace(index, current);
}

bool Draft::hasAgentStep() const
{
    for (const QJsonValue &value : nodes)
        if (text(value.toObject(), "type") == QLatin1String("agent.run"))
            return true;
    return false;
}

QString localWorkflowId(const QString &name)
{
    QString result;
    bool dash = false;
    for (const QChar ch : name.trimmed().toLower()) {
        if (ch.isLetterOrNumber() && ch.unicode() < 128) {
            result += ch;
            dash = false;
        } else if (!result.isEmpty() && !dash) {
            result += QLatin1Char('-');
            dash = true;
        }
        if (result.size() >= 60)
            break;
    }
    while (result.endsWith(QLatin1Char('-')))
        result.chop(1);
    return result.isEmpty() ? QStringLiteral("workflow") : result;
}

QString localStatusLabel(const QString &status)
{
    if (status == QLatin1String("stopped"))
        return QStringLiteral("Stopped");
    if (status == QLatin1String("queued"))
        return QStringLiteral("Waiting");
    return statusLabel(status);
}

QJsonObject Draft::graph() const
{
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("nodes"), nodes},
                       {QStringLiteral("connections"), connections}};
}

QJsonObject Draft::body() const
{
    QJsonObject out = graph();
    out.insert(QStringLiteral("active"), active);
    return out;
}

int Draft::indexOf(const QString &nodeId) const
{
    for (int index = 0; index < nodes.size(); ++index)
        if (text(nodes.at(index).toObject(), "id") == nodeId)
            return index;
    return -1;
}

QJsonObject Draft::node(const QString &nodeId) const
{
    const int index = indexOf(nodeId);
    return index < 0 ? QJsonObject() : nodes.at(index).toObject();
}

QJsonObject Draft::trigger(const Catalog &catalog) const
{
    for (const QJsonValue &value : nodes)
        if (catalog.isTrigger(text(value.toObject(), "type")))
            return value.toObject();
    return {};
}

QStringList Draft::ordered() const
{
    QHash<QString, int> incoming;
    QHash<QString, QStringList> edges;
    QStringList ids;
    for (const QJsonValue &value : nodes) {
        const QString nodeId = text(value.toObject(), "id");
        ids << nodeId;
        incoming.insert(nodeId, 0);
        edges.insert(nodeId, {});
    }
    for (const QJsonValue &value : connections) {
        const QJsonObject link = value.toObject();
        const QString from = text(link, "from");
        const QString to = text(link, "to");
        if (!incoming.contains(to) || !edges.contains(from))
            continue;
        edges[from].append(to);
        ++incoming[to];
    }
    QStringList ready;
    for (const QString &nodeId : ids)
        if (incoming.value(nodeId) == 0)
            ready << nodeId;
    QStringList order;
    while (!ready.isEmpty()) {
        const QString current = ready.takeFirst();
        order << current;
        for (const QString &target : edges.value(current)) {
            if (--incoming[target] == 0)
                ready << target;
        }
    }
    for (const QString &nodeId : ids)
        if (!order.contains(nodeId))
            order << nodeId;
    return order;
}

QJsonArray Draft::inputs(const QString &nodeId) const
{
    QJsonArray found;
    for (const QJsonValue &value : connections)
        if (text(value.toObject(), "to") == nodeId)
            found.append(value);
    return found;
}

QJsonArray Draft::outputsOf(const QString &nodeId) const
{
    QJsonArray found;
    for (const QJsonValue &value : connections)
        if (text(value.toObject(), "from") == nodeId)
            found.append(value);
    return found;
}

bool Draft::nameTaken(const QString &value, const QString &except) const
{
    for (const QJsonValue &item : nodes) {
        const QJsonObject other = item.toObject();
        if (text(other, "id") != except && text(other, "name") == value)
            return true;
    }
    return false;
}

QString Draft::uniqueName(const QString &base, const QString &except) const
{
    const QString root = (base.trimmed().isEmpty() ? QStringLiteral("Step") : base.trimmed()).left(56);
    if (!nameTaken(root, except))
        return root;
    int index = 2;
    while (nameTaken(QStringLiteral("%1 %2").arg(root).arg(index), except))
        ++index;
    return QStringLiteral("%1 %2").arg(root).arg(index);
}

QString Draft::newId(const QString &type) const
{
    QString stem = type.section(QLatin1Char('.'), -1);
    stem.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")));
    if (stem.isEmpty())
        stem = QStringLiteral("step");
    stem = stem.left(24);
    int index = 1;
    while (indexOf(stem + QString::number(index)) >= 0)
        ++index;
    return stem + QString::number(index);
}

QString Draft::addStep(const Catalog &catalog, const QString &type, const QString &parent,
                       const QString &output)
{
    if (catalog.isTrigger(type)) {
        setTrigger(catalog, type);
        return text(trigger(catalog), "id");
    }
    QJsonObject anchor = node(parent);
    if (anchor.isEmpty() && !nodes.isEmpty()) {
        const QStringList order = ordered();
        anchor = node(order.last());
    }
    const QString anchorId = text(anchor, "id");
    const int siblings = anchorId.isEmpty() ? 0 : int(outputsOf(anchorId).size());
    QJsonObject params;
    QString operation;
    if (catalog.isApp(type)) {
        const QJsonArray ops = catalog.operations(type);
        if (!ops.isEmpty()) {
            operation = text(ops.first().toObject(), "key");
            params.insert(QStringLiteral("operation"), operation);
        }
    }
    for (const QJsonValue &value : catalog.fields(type, operation)) {
        const QJsonObject field = value.toObject();
        const QString fallback = field.value(QStringLiteral("default")).toVariant().toString();
        if (!fallback.isEmpty())
            params.insert(text(field, "key"), fallback);
    }
    const QString stepId = newId(type);
    nodes.append(QJsonObject{
        {QStringLiteral("id"), stepId},
        {QStringLiteral("type"), type},
        {QStringLiteral("name"), uniqueName(catalog.name(type))},
        {QStringLiteral("x"), integer(anchor, "x") + (anchorId.isEmpty() ? 0 : kColumn)},
        {QStringLiteral("y"), integer(anchor, "y") + siblings * kRow},
        {QStringLiteral("params"), params}});
    if (!anchorId.isEmpty()) {
        QString branch = output;
        const QStringList allowed = catalog.outputs(text(anchor, "type"));
        if (!allowed.contains(branch))
            branch = allowed.first();
        connections.append(connection(anchorId, branch, stepId));
    }
    return stepId;
}

void Draft::setTrigger(const Catalog &catalog, const QString &type)
{
    if (!catalog.isTrigger(type))
        return;
    QJsonObject params;
    for (const QJsonValue &value : catalog.fields(type, QString())) {
        const QJsonObject field = value.toObject();
        const QString fallback = field.value(QStringLiteral("default")).toVariant().toString();
        if (!fallback.isEmpty())
            params.insert(text(field, "key"), fallback);
    }
    for (int index = 0; index < nodes.size(); ++index) {
        QJsonObject current = nodes.at(index).toObject();
        if (!catalog.isTrigger(text(current, "type")))
            continue;
        if (text(current, "type") == type)
            return;
        const QString currentId = text(current, "id");
        current.insert(QStringLiteral("type"), type);
        current.insert(QStringLiteral("params"), params);
        current.insert(QStringLiteral("name"), uniqueName(catalog.name(type), currentId));
        nodes.replace(index, current);
        return;
    }
    const QStringList roots = [this] {
        QStringList found;
        for (const QJsonValue &value : nodes) {
            const QString nodeId = text(value.toObject(), "id");
            if (inputs(nodeId).isEmpty())
                found << nodeId;
        }
        return found;
    }();
    const QString triggerId = newId(type);
    qint64 x = 0;
    qint64 y = 0;
    if (!roots.isEmpty()) {
        x = integer(node(roots.first()), "x") - kColumn;
        y = integer(node(roots.first()), "y");
    }
    nodes.insert(0, QJsonObject{{QStringLiteral("id"), triggerId},
                                {QStringLiteral("type"), type},
                                {QStringLiteral("name"), uniqueName(catalog.name(type))},
                                {QStringLiteral("x"), x},
                                {QStringLiteral("y"), y},
                                {QStringLiteral("params"), params}});
    for (const QString &root : roots)
        connections.append(connection(triggerId, QStringLiteral("main"), root));
}

void Draft::remove(const QString &nodeId)
{
    const QJsonArray parents = inputs(nodeId);
    const QJsonArray children = outputsOf(nodeId);
    const int index = indexOf(nodeId);
    if (index < 0)
        return;
    nodes.removeAt(index);
    QJsonArray kept;
    for (const QJsonValue &value : connections) {
        const QJsonObject link = value.toObject();
        if (text(link, "from") != nodeId && text(link, "to") != nodeId)
            kept.append(link);
    }
    connections = kept;
    for (const QJsonValue &parentValue : parents) {
        const QJsonObject parent = parentValue.toObject();
        for (const QJsonValue &childValue : children) {
            const QJsonObject joined = connection(text(parent, "from"), text(parent, "output"),
                                                  text(childValue.toObject(), "to"));
            bool exists = false;
            for (const QJsonValue &value : connections)
                exists = exists || sameConnection(value.toObject(), joined);
            if (!exists)
                connections.append(joined);
        }
    }
}

bool Draft::updateNode(const QJsonObject &updated)
{
    const int index = indexOf(text(updated, "id"));
    if (index < 0)
        return false;
    nodes.replace(index, updated);
    return true;
}

bool Draft::reaches(const QString &from, const QString &to) const
{
    QSet<QString> seen;
    QStringList stack{from};
    while (!stack.isEmpty()) {
        const QString current = stack.takeLast();
        if (current == to)
            return true;
        if (seen.contains(current))
            continue;
        seen.insert(current);
        for (const QJsonValue &value : outputsOf(current))
            stack << text(value.toObject(), "to");
    }
    return false;
}

bool Draft::canConnect(const Catalog &catalog, const QString &from, const QString &output,
                       const QString &to) const
{
    const QJsonObject source = node(from);
    const QJsonObject target = node(to);
    if (source.isEmpty() || target.isEmpty() || from == to)
        return false;
    if (catalog.isTrigger(text(target, "type")))
        return false;
    if (!catalog.outputs(text(source, "type")).contains(output))
        return false;
    return !reaches(to, from);
}

bool Draft::connectSteps(const Catalog &catalog, const QString &from, const QString &output,
                         const QString &to)
{
    if (!canConnect(catalog, from, output, to))
        return false;
    const QJsonObject link = connection(from, output, to);
    for (const QJsonValue &value : connections)
        if (sameConnection(value.toObject(), link))
            return true;
    connections.append(link);
    return true;
}

void Draft::disconnectSteps(const QString &from, const QString &output, const QString &to)
{
    const QJsonObject link = connection(from, output, to);
    QJsonArray kept;
    for (const QJsonValue &value : connections)
        if (!sameConnection(value.toObject(), link))
            kept.append(value);
    connections = kept;
}

bool Draft::hasDesktopSteps(const Catalog &catalog) const
{
    for (const QJsonValue &value : nodes)
        if (catalog.isDesktop(text(value.toObject(), "type")))
            return true;
    return false;
}

QString Draft::inputText(const QJsonObject &link) const
{
    const QString from = text(node(text(link, "from")), "name");
    const QString name = from.isEmpty() ? text(link, "from") : from;
    const QString output = text(link, "output");
    if (output.isEmpty() || output == QLatin1String("main"))
        return QStringLiteral("After %1").arg(name);
    return QStringLiteral("%1: %2").arg(name, output);
}

QString Draft::inputsText(const QString &nodeId) const
{
    QStringList parts;
    for (const QJsonValue &value : inputs(nodeId))
        parts << inputText(value.toObject());
    return parts.join(QStringLiteral(", "));
}

QStringList freeDiskSpaceArguments(const QString &checkout, const QString &step)
{
    return {QStringLiteral("-u"), QDir(checkout).filePath(QStringLiteral("tools/free_disk_space.py")),
            QStringLiteral("--execute"), QStringLiteral("--step"), step,
            QStringLiteral("--repo"), checkout, QStringLiteral("--json")};
}

QJsonObject freeDiskSpaceReply(const QString &step, const QByteArray &output, qint64 ms)
{
    static const QString prefix = QStringLiteral("::result::");
    QJsonObject result;
    QStringList lines;
    for (const QString &raw : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(prefix)) {
            result = QJsonDocument::fromJson(line.mid(prefix.size()).toUtf8()).object();
            continue;
        }
        if (!line.isEmpty())
            lines << line.left(300);
    }
    if (result.isEmpty() || text(result, "step") != step)
        return {};
    if (lines.size() > 200)
        lines = lines.mid(lines.size() - 200);
    QJsonObject reply{{QStringLiteral("step"), step},
                      {QStringLiteral("ms"), double(std::max<qint64>(0, ms))},
                      {QStringLiteral("lines"), QJsonArray::fromStringList(lines)}};
    for (const char *key : {"freedBytes", "items", "freeBytes", "totalBytes"})
        reply.insert(QLatin1String(key), std::max(0.0, result.value(QLatin1String(key)).toDouble()));
    return reply;
}

QString freeDiskSpaceFailure(const QByteArray &output)
{
    const QString last = QString::fromUtf8(output).trimmed().section(QLatin1Char('\n'), -1).trimmed();
    return last.isEmpty() ? QStringLiteral("The cleanup step stopped before it finished.") : last.left(300);
}

} // namespace forkmesh::workflows

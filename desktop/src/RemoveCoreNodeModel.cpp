#include "RemoveCoreNodeModel.h"

#include "AddCoreNodeModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace forkmesh::fleet {

namespace {

QJsonObject planObject(const QByteArray &json)
{
    QByteArray text = json.trimmed();
    const int brace = text.lastIndexOf("\n{");
    if (brace >= 0)
        text = text.mid(brace + 1);
    const QJsonDocument document = QJsonDocument::fromJson(text);
    return document.isObject() ? document.object() : QJsonObject();
}

QStringList strings(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray())
        out.append(item.toString());
    return out;
}

QString joined(const QStringList &items)
{
    return items.isEmpty() ? QStringLiteral("none") : items.join(QStringLiteral(", "));
}

} // namespace

RemoveCorePlan parseRemoveCorePlan(const QByteArray &json)
{
    RemoveCorePlan plan;
    const QJsonObject object = planObject(json);
    if (object.isEmpty()) {
        plan.error = QStringLiteral("the fleet tool returned no plan");
        return plan;
    }
    plan.valid = true;
    plan.name = object.value(QStringLiteral("name")).toString();
    plan.label = object.value(QStringLiteral("label")).toString();
    plan.provider = object.value(QStringLiteral("provider")).toString();
    plan.instanceId = object.value(QStringLiteral("instance_id")).toString();
    plan.ip = object.value(QStringLiteral("ip")).toString();
    plan.leader = object.value(QStringLiteral("leader")).toString();
    plan.promote = object.value(QStringLiteral("promote")).toString();
    plan.voter = object.value(QStringLiteral("voter")).toBool(true);
    plan.isLeader = object.value(QStringLiteral("is_leader")).toBool();
    plan.allowed = object.value(QStringLiteral("allowed")).toBool();
    plan.resume = object.value(QStringLiteral("resume")).toBool();
    plan.destroyInstance = object.value(QStringLiteral("destroy_instance")).toBool();
    plan.blocker = object.value(QStringLiteral("blocker")).toString();
    plan.error = object.value(QStringLiteral("error")).toString();
    plan.quorum = object.value(QStringLiteral("quorum")).toString();
    plan.votersBefore = strings(object.value(QStringLiteral("voters_before")));
    plan.votersAfter = strings(object.value(QStringLiteral("voters_after")));
    plan.healthyAfter = strings(object.value(QStringLiteral("healthy_after")));
    plan.warnings = strings(object.value(QStringLiteral("warnings")));
    plan.description = strings(object.value(QStringLiteral("description")));
    plan.estimateSeconds = object.value(QStringLiteral("estimate_seconds")).toInt();
    plan.steps = AddCoreRun::stepsFromJson(json);
    const QJsonObject state = object.value(QStringLiteral("state")).toObject();
    plan.finished = state.value(QStringLiteral("finished")).toBool();
    plan.destroyed = state.value(QStringLiteral("destroyed")).toBool();
    const QJsonObject failed = state.value(QStringLiteral("failed")).toObject();
    plan.failedStep = failed.value(QStringLiteral("step")).toString();
    plan.failedError = failed.value(QStringLiteral("error")).toString();
    if (plan.label.isEmpty() && isRemovableCoreName(plan.name))
        plan.label = QStringLiteral("fmnext-core-%1").arg(plan.name.mid(1));
    return plan;
}

bool isRemovableCoreName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^n[0-9]{1,2}$"));
    return pattern.match(name).hasMatch();
}

QString coreCountBlocker(const QString &name, int coreCount)
{
    if (coreCount > kMinimumCores)
        return {};
    return QStringLiteral("The fleet has %1 core(s). Removing %2 would leave fewer than %3, "
                          "which breaks rqlite quorum. Add a core first.")
        .arg(coreCount)
        .arg(name)
        .arg(kMinimumCores);
}

bool removeConfirmationMatches(const QString &name, const QString &typed)
{
    return !name.isEmpty() && typed.trimmed() == name;
}

QString removeCoreBlocker(const RemoveCorePlan &plan, bool planLoading, bool running,
                          bool destroyInstance, const QString &typed)
{
    if (running)
        return QStringLiteral("A removal is already running.");
    if (planLoading || (plan.valid && plan.destroyInstance != destroyInstance))
        return QStringLiteral("Checking the fleet and the rqlite cluster…");
    if (!plan.valid)
        return plan.error.isEmpty() ? QStringLiteral("No plan from the fleet tool.") : plan.error;
    if (!plan.allowed)
        return plan.blocker.isEmpty() ? QStringLiteral("The fleet tool refused this removal.")
                                      : plan.blocker;
    if (!removeConfirmationMatches(plan.name, typed))
        return QStringLiteral("Type %1 to confirm.").arg(plan.name);
    return {};
}

QString removeCoreSummaryText(const RemoveCorePlan &plan, bool destroyInstance)
{
    const QString name = plan.name;
    const QString provider = providerDisplayName(plan.provider);
    QStringList lines;
    if (plan.resume && !plan.finished)
        lines << QStringLiteral("%1 is partly removed%2. Remove resumes from there.")
                     .arg(name, plan.failedStep.isEmpty()
                                    ? QString()
                                    : QStringLiteral(" (stopped at %1)").arg(plan.failedStep))
              << QString();
    else if (plan.resume && plan.finished)
        lines << QStringLiteral("%1 is already out of the fleet; only its cloud instance is left.")
                     .arg(name)
              << QString();
    lines << QStringLiteral("This will:");
    if (!plan.promote.isEmpty())
        lines << QStringLiteral("• make %1 an rqlite voter first (it is a non-voter now), so "
                                "%2 voters remain; %1 is out of DNS for about a minute;")
                     .arg(plan.promote)
                     .arg(plan.votersAfter.size());
    if (plan.isLeader)
        lines << QStringLiteral("• hand rqlite leadership to another voter first (%1 leads now);")
                     .arg(name);
    lines << QStringLiteral("• drain %1 from DNS (app and world) and wait out the record TTL;")
                 .arg(name);
    lines << QStringLiteral("• stop the app, mirror daemon, Caddy and certsync on %1, then stop "
                            "its rqlite and remove it from the cluster;")
                 .arg(name);
    if (plan.voter)
        lines << QStringLiteral("• rqlite voters go from %1 to %2 (healthy after: %3);")
                     .arg(joined(plan.votersBefore), joined(plan.votersAfter),
                          joined(plan.healthyAfter));
    else
        lines << QStringLiteral("• %1 is an rqlite non-voter, so the voters (%2) do not change;")
                     .arg(name, joined(plan.votersBefore));
    lines << QStringLiteral("• take %1 out of deploy/fleet.toml and update the other cores one "
                            "at a time (each is drained for about a minute) so they drop its "
                            "WireGuard peer and firewall allowances;")
                 .arg(name);
    lines << QStringLiteral("• delete its own DNS name and forget it in the DNS health loop;");
    lines << QStringLiteral("• commit deploy/fleet.toml to fleet/remove-core-%1 for you to merge "
                            "(nothing is pushed);")
                 .arg(name);
    if (destroyInstance)
        lines << QStringLiteral("• DELETE the %1 instance %2 (%3). Everything on it is gone and "
                                "this cannot be undone.")
                     .arg(provider, plan.label,
                          plan.instanceId.isEmpty() ? QStringLiteral("id unknown")
                                                    : plan.instanceId);
    else
        lines << QStringLiteral("• keep the %1 instance %2 running, and billed, until you delete "
                                "it.")
                     .arg(provider, plan.label);
    for (const QString &warning : plan.warnings)
        lines << QStringLiteral("Note: %1.").arg(warning);
    lines << QString();
    lines << QStringLiteral("Estimated time: %1. A failure stops at the failed step, and Retry "
                            "resumes from it.")
                 .arg(formatDuration(plan.estimateSeconds));
    return lines.join(QLatin1Char('\n'));
}

QStringList removeCorePlanArguments(const QString &script, const QString &name,
                                    bool destroyInstance)
{
    QStringList arguments{script, QStringLiteral("remove-core-plan"), name,
                          QStringLiteral("--json")};
    if (destroyInstance)
        arguments << QStringLiteral("--destroy-instance");
    return arguments;
}

QStringList removeCoreArguments(const QString &script, const QString &name,
                                bool destroyInstance)
{
    QStringList arguments{script, QStringLiteral("remove-core"), name,
                          QStringLiteral("--execute")};
    if (destroyInstance)
        arguments << QStringLiteral("--destroy-instance");
    return arguments;
}

QString removeCoreUnitName(const QString &stamp)
{
    return addCoreUnitName(stamp).replace(QLatin1String("fmnext-add-core-"),
                                          QLatin1String("fmnext-remove-core-"));
}

void configureRemoveRun(AddCoreRun &run)
{
    run.setEventPrefix(QString::fromLatin1(kRemoveCoreEventPrefix));
    run.setVerb(QStringLiteral("Removing"));
    run.setDoneSummary(QStringLiteral("%1 is out of the fleet"));
}

} // namespace forkmesh::fleet

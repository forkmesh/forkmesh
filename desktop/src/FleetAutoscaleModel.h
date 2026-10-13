#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <cmath>

namespace forkmesh::autoscale {

constexpr int kMaxNodesPerPool = 4;
constexpr int kMinCooldownMinutes = 5;
constexpr int kMaxMonthlyCapCents = 10000;

struct PoolSettings {
    bool runner = false;
    bool enabled = false;
    bool dryRun = true;
    int min = 0;
    int max = 2;
    int upP95Ms = 500;
    int upLoopLagMs = 250;
    double upLoadPerCpu = 0.8;
    int upRps = 0;
    int upForMinutes = 5;
    double downFraction = 0.5;
    int downForMinutes = 30;
    int upQueueWaitMinutes = 2;
    int upQueueDepth = 1;
    int downIdleMinutes = 10;
    int cooldownMinutes = 10;
    QString plan = QStringLiteral("vc2-1c-1gb");
    QString region = QStringLiteral("ewr");
    int monthlyCapCents = 1500;
    QString snapshotId;
};

struct Settings {
    int version = 1;
    bool killSwitch = false;
    PoolSettings app;
    PoolSettings runner;
};

inline Settings defaultSettings()
{
    Settings settings;
    settings.runner.runner = true;
    settings.runner.monthlyCapCents = 1000;
    return settings;
}

inline int intOf(const QJsonObject &object, const char *key, int fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? int(std::lround(value.toDouble())) : fallback;
}

inline double doubleOf(const QJsonObject &object, const char *key,
                       double fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? value.toDouble() : fallback;
}

inline bool boolOf(const QJsonObject &object, const char *key, bool fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isBool() ? value.toBool() : fallback;
}

inline QString stringOf(const QJsonObject &object, const char *key,
                        const QString &fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : fallback;
}

inline PoolSettings poolFromJson(const QJsonObject &object,
                                 const PoolSettings &base)
{
    PoolSettings pool = base;
    pool.enabled = boolOf(object, "enabled", base.enabled);
    pool.dryRun = boolOf(object, "dryRun", base.dryRun);
    pool.min = intOf(object, "min", base.min);
    pool.max = intOf(object, "max", base.max);
    const QJsonObject up = object.value(QStringLiteral("scaleUp")).toObject();
    const QJsonObject down =
        object.value(QStringLiteral("scaleDown")).toObject();
    if (pool.runner) {
        pool.upQueueWaitMinutes =
            intOf(up, "queueWaitMinutes", base.upQueueWaitMinutes);
        pool.upQueueDepth = intOf(up, "queueDepth", base.upQueueDepth);
        pool.downIdleMinutes = intOf(down, "idleMinutes", base.downIdleMinutes);
    } else {
        pool.upP95Ms = intOf(up, "p95Ms", base.upP95Ms);
        pool.upLoopLagMs = intOf(up, "loopLagMs", base.upLoopLagMs);
        pool.upLoadPerCpu = doubleOf(up, "loadPerCpu", base.upLoadPerCpu);
        pool.upRps = intOf(up, "rps", base.upRps);
        pool.upForMinutes = intOf(up, "forMinutes", base.upForMinutes);
        pool.downFraction = doubleOf(down, "fraction", base.downFraction);
        pool.downForMinutes = intOf(down, "forMinutes", base.downForMinutes);
    }
    pool.snapshotId = stringOf(object, "snapshotId", base.snapshotId);
    pool.cooldownMinutes = intOf(object, "cooldownMinutes", base.cooldownMinutes);
    pool.plan = stringOf(object, "plan", base.plan);
    pool.region = stringOf(object, "region", base.region);
    pool.monthlyCapCents = intOf(object, "monthlyCapCents", base.monthlyCapCents);
    return pool;
}

inline QJsonObject poolToJson(const PoolSettings &pool)
{
    QJsonObject object{
        {QStringLiteral("enabled"), pool.enabled},
        {QStringLiteral("dryRun"), pool.dryRun},
        {QStringLiteral("min"), pool.min},
        {QStringLiteral("max"), pool.max},
        {QStringLiteral("cooldownMinutes"), pool.cooldownMinutes},
        {QStringLiteral("plan"), pool.plan},
        {QStringLiteral("region"), pool.region},
        {QStringLiteral("monthlyCapCents"), pool.monthlyCapCents},
        {QStringLiteral("snapshotId"), pool.snapshotId},
    };
    if (pool.runner) {
        object.insert(QStringLiteral("scaleUp"),
                      QJsonObject{{QStringLiteral("queueWaitMinutes"),
                                   pool.upQueueWaitMinutes},
                                  {QStringLiteral("queueDepth"),
                                   pool.upQueueDepth}});
        object.insert(QStringLiteral("scaleDown"),
                      QJsonObject{{QStringLiteral("idleMinutes"),
                                   pool.downIdleMinutes}});
    } else {
        object.insert(QStringLiteral("scaleUp"),
                      QJsonObject{{QStringLiteral("p95Ms"), pool.upP95Ms},
                                  {QStringLiteral("loopLagMs"), pool.upLoopLagMs},
                                  {QStringLiteral("loadPerCpu"), pool.upLoadPerCpu},
                                  {QStringLiteral("rps"), pool.upRps},
                                  {QStringLiteral("forMinutes"),
                                   pool.upForMinutes}});
        object.insert(QStringLiteral("scaleDown"),
                      QJsonObject{{QStringLiteral("fraction"), pool.downFraction},
                                  {QStringLiteral("forMinutes"),
                                   pool.downForMinutes}});
    }
    return object;
}

inline Settings settingsFromJson(const QJsonObject &object)
{
    const Settings defaults = defaultSettings();
    Settings settings;
    settings.version = intOf(object, "version", 1);
    settings.killSwitch = boolOf(object, "killSwitch", false);
    settings.app =
        poolFromJson(object.value(QStringLiteral("app")).toObject(), defaults.app);
    settings.runner = poolFromJson(
        object.value(QStringLiteral("runner")).toObject(), defaults.runner);
    return settings;
}

inline QJsonObject settingsToJson(const Settings &settings)
{
    return {{QStringLiteral("version"), settings.version},
            {QStringLiteral("killSwitch"), settings.killSwitch},
            {QStringLiteral("app"), poolToJson(settings.app)},
            {QStringLiteral("runner"), poolToJson(settings.runner)}};
}

inline QString validatePool(const PoolSettings &pool)
{
    const QString name =
        pool.runner ? QStringLiteral("Burst runners") : QStringLiteral("App capacity");
    if (pool.min < 0 || pool.max < 0)
        return name + QStringLiteral(": node counts cannot be negative");
    if (pool.max > kMaxNodesPerPool)
        return name + QStringLiteral(": at most %1 nodes").arg(kMaxNodesPerPool);
    if (pool.min > pool.max)
        return name + QStringLiteral(": the minimum is above the maximum");
    if (pool.cooldownMinutes < kMinCooldownMinutes)
        return name + QStringLiteral(": the cooldown must be at least %1 minutes")
                          .arg(kMinCooldownMinutes);
    if (pool.monthlyCapCents < 0 || pool.monthlyCapCents > kMaxMonthlyCapCents)
        return name + QStringLiteral(": the monthly cap must be $0 to $%1")
                          .arg(kMaxMonthlyCapCents / 100);
    static const QRegularExpression slug(QStringLiteral("^[a-z0-9-]{2,40}$"));
    if (!slug.match(pool.plan).hasMatch())
        return name + QStringLiteral(": the plan is not a Vultr plan id");
    if (!slug.match(pool.region).hasMatch())
        return name + QStringLiteral(": the region is not a Vultr region id");
    static const QRegularExpression snapshot(
        QStringLiteral("^([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-"
                       "[0-9a-f]{12})?$"));
    if (!snapshot.match(pool.snapshotId).hasMatch())
        return name + QStringLiteral(": the snapshot id is not a Vultr id");
    if (pool.runner) {
        if (pool.upQueueWaitMinutes < 1 || pool.upQueueDepth < 1 ||
            pool.downIdleMinutes < 1)
            return name + QStringLiteral(": thresholds must be positive");
    } else {
        if (pool.upP95Ms < 0 || pool.upLoopLagMs < 0 || pool.upLoadPerCpu < 0 ||
            pool.upRps < 0)
            return name + QStringLiteral(": thresholds cannot be negative");
        if (pool.upP95Ms == 0 && pool.upLoopLagMs == 0 &&
            pool.upLoadPerCpu == 0 && pool.upRps == 0)
            return name + QStringLiteral(": at least one scale-up signal is needed");
        if (pool.upForMinutes < 1 || pool.downForMinutes < 1)
            return name + QStringLiteral(": durations must be at least a minute");
        if (!(pool.downFraction > 0 && pool.downFraction < 1))
            return name + QStringLiteral(": the scale-down fraction must be between 0 and 1");
    }
    return {};
}

inline QString validateSettings(const Settings &settings)
{
    const QString app = validatePool(settings.app);
    return app.isEmpty() ? validatePool(settings.runner) : app;
}

struct NodeRow {
    QString name;
    QString label;
    QString ip;
    QString state;
    qint64 createdMs = 0;
    QString instanceId;
};

struct PoolStatus {
    QList<NodeRow> nodes;
    QString decisionAction = QStringLiteral("none");
    QString decisionReason;
    qint64 decisionAtMs = 0;
    qint64 lastActionMs = 0;
    double spentCents = 0;
    QString month;
    QJsonObject signalValues;
    QString snapshotId;
    qint64 snapshotBuiltMs = 0;
};

struct RecentEvent {
    qint64 ts = 0;
    QString pool;
    QString event;
    QString detail;
    bool ok = true;
};

struct Status {
    bool parsed = false;
    bool ok = false;
    QString error;
    Settings settings;
    bool verified = false;
    QString settingsError;
    bool serviceActive = false;
    QString serviceUnit;
    QString serviceOwner;
    qint64 serviceLastTickMs = 0;
    PoolStatus app;
    PoolStatus runner;
    QList<RecentEvent> recent;
};

inline PoolStatus poolStatusFromJson(const QJsonObject &object)
{
    PoolStatus pool;
    for (const QJsonValue &value :
         object.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject node = value.toObject();
        NodeRow row;
        row.name = node.value(QStringLiteral("name")).toString();
        row.label = node.value(QStringLiteral("label")).toString();
        row.ip = node.value(QStringLiteral("ip")).toString();
        row.state = node.value(QStringLiteral("state")).toString();
        row.createdMs =
            qint64(node.value(QStringLiteral("createdMs")).toDouble());
        row.instanceId = node.value(QStringLiteral("instanceId")).toString();
        if (!row.name.isEmpty() || !row.label.isEmpty())
            pool.nodes.append(row);
    }
    const QJsonObject decision =
        object.value(QStringLiteral("decision")).toObject();
    pool.decisionAction =
        decision.value(QStringLiteral("action")).toString(QStringLiteral("none"));
    pool.decisionReason = decision.value(QStringLiteral("reason")).toString();
    pool.decisionAtMs = qint64(decision.value(QStringLiteral("atMs")).toDouble());
    pool.lastActionMs =
        qint64(object.value(QStringLiteral("lastActionMs")).toDouble());
    pool.spentCents = object.value(QStringLiteral("spentCents")).toDouble();
    pool.month = object.value(QStringLiteral("month")).toString();
    pool.signalValues = object.value(QStringLiteral("signals")).toObject();
    const QJsonObject snapshot = object.value(QStringLiteral("snapshot")).toObject();
    pool.snapshotId = snapshot.value(QStringLiteral("id")).toString();
    pool.snapshotBuiltMs = qint64(snapshot.value(QStringLiteral("builtMs")).toDouble());
    return pool;
}

inline Status parseStatus(const QByteArray &output)
{
    Status status;
    QByteArray body = output.trimmed();
    const int brace = body.indexOf('{');
    if (brace > 0)
        body = body.mid(brace);
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        status.error = QStringLiteral("the scaler printed no status");
        return status;
    }
    const QJsonObject object = document.object();
    status.parsed = true;
    status.ok = object.value(QStringLiteral("ok")).toBool();
    status.error = object.value(QStringLiteral("error")).toString();
    status.settings =
        settingsFromJson(object.value(QStringLiteral("settings")).toObject());
    status.verified = object.value(QStringLiteral("verified")).toBool();
    status.settingsError = object.value(QStringLiteral("settingsError")).toString();
    const QJsonObject service = object.value(QStringLiteral("service")).toObject();
    status.serviceActive = service.value(QStringLiteral("active")).toBool();
    status.serviceUnit = service.value(QStringLiteral("unit")).toString();
    status.serviceOwner = service.value(QStringLiteral("owner")).toString();
    status.serviceLastTickMs =
        qint64(service.value(QStringLiteral("lastTickMs")).toDouble());
    const QJsonObject pools = object.value(QStringLiteral("pools")).toObject();
    status.app = poolStatusFromJson(pools.value(QStringLiteral("app")).toObject());
    status.runner =
        poolStatusFromJson(pools.value(QStringLiteral("runner")).toObject());
    for (const QJsonValue &value :
         object.value(QStringLiteral("recent")).toArray()) {
        const QJsonObject item = value.toObject();
        RecentEvent event;
        event.ts = qint64(item.value(QStringLiteral("ts")).toDouble());
        event.pool = item.value(QStringLiteral("pool")).toString();
        event.event = item.value(QStringLiteral("event")).toString();
        event.detail = item.value(QStringLiteral("detail")).toString();
        event.ok = item.value(QStringLiteral("ok")).toBool(true);
        status.recent.append(event);
    }
    return status;
}

inline QString formatNumber(const QJsonValue &value, const QString &unit,
                            int decimals = 0)
{
    if (!value.isDouble())
        return QStringLiteral("n/a");
    return QString::number(value.toDouble(), 'f', decimals) + unit;
}

inline QString formatSignals(bool runner, const QJsonObject &values)
{
    if (runner) {
        return QStringLiteral("queued %1 · oldest wait %2")
            .arg(formatNumber(values.value(QStringLiteral("queued")), QString()),
                 formatNumber(values.value(QStringLiteral("oldestWaitS")),
                              QStringLiteral(" s")));
    }
    return QStringLiteral("p95 %1 · loop lag %2 · load/CPU %3 · %4")
        .arg(formatNumber(values.value(QStringLiteral("p95Ms")),
                          QStringLiteral(" ms")),
             formatNumber(values.value(QStringLiteral("loopLagMs")),
                          QStringLiteral(" ms")),
             formatNumber(values.value(QStringLiteral("loadPerCpu")), QString(), 2),
             formatNumber(values.value(QStringLiteral("rps")),
                          QStringLiteral(" req/s"), 1));
}

inline QString formatDollars(double cents)
{
    return QStringLiteral("$%1").arg(cents / 100.0, 0, 'f', 2);
}

inline QString formatAgo(qint64 atMs, qint64 nowMs)
{
    if (atMs <= 0)
        return QStringLiteral("never");
    const qint64 seconds = qMax<qint64>(0, (nowMs - atMs) / 1000);
    if (seconds < 120)
        return QStringLiteral("%1 s ago").arg(seconds);
    if (seconds < 2 * 3600)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 2 * 86400)
        return QStringLiteral("%1 h ago").arg(seconds / 3600);
    return QStringLiteral("%1 days ago").arg(seconds / 86400);
}

inline QString serviceLine(const Status &status, qint64 nowMs)
{
    QString line;
    if (status.serviceActive) {
        line = QStringLiteral("Scaler running on the cores (owner %1, last tick %2).")
                   .arg(status.serviceOwner.isEmpty() ? QStringLiteral("unknown")
                                                      : status.serviceOwner,
                        formatAgo(status.serviceLastTickMs, nowMs));
    } else {
        line = QStringLiteral("No core has run a scaler tick in the last 3 minutes "
                              "(last tick %1): nothing scales automatically.")
                   .arg(formatAgo(status.serviceLastTickMs, nowMs));
    }
    if (status.settings.killSwitch)
        line = QStringLiteral("KILL SWITCH ON: the cores take no scaling action. ") + line;
    return line;
}

inline QJsonObject autoscaledInstancesJson(const Status &status)
{
    QJsonObject instances;
    for (const PoolStatus *pool : {&status.app, &status.runner}) {
        for (const NodeRow &node : pool->nodes) {
            static const QRegularExpression label(
                QStringLiteral("^fmnext-(auto|burst)-[0-9]{1,4}$"));
            static const QRegularExpression address(
                QStringLiteral("^[0-9A-Fa-f:.]{2,45}$"));
            if (!label.match(node.label).hasMatch() ||
                !address.match(node.ip).hasMatch())
                continue;
            instances.insert(node.label,
                             QJsonObject{{QStringLiteral("id"), node.instanceId},
                                         {QStringLiteral("ip"), node.ip}});
        }
    }
    return instances;
}

inline QString formatAge(qint64 createdMs, qint64 nowMs)
{
    if (createdMs <= 0 || nowMs < createdMs)
        return QStringLiteral("—");
    const qint64 minutes = (nowMs - createdMs) / 60000;
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 48)
        return QStringLiteral("%1 h %2 min").arg(hours).arg(minutes % 60);
    return QStringLiteral("%1 days").arg(hours / 24);
}

inline double planMonthlyDollars(const QString &plan)
{
    static const QList<QPair<QString, double>> known = {
        {QStringLiteral("vc2-1c-0.5gb"), 2.5}, {QStringLiteral("vc2-1c-1gb"), 5},
        {QStringLiteral("vc2-1c-2gb"), 10},    {QStringLiteral("vc2-2c-2gb"), 15},
        {QStringLiteral("vc2-2c-4gb"), 20},    {QStringLiteral("vc2-4c-8gb"), 40},
    };
    for (const auto &entry : known) {
        if (entry.first == plan)
            return entry.second;
    }
    return 0;
}

inline QString scaleUpCostNote(const PoolSettings &pool)
{
    const double monthly = planMonthlyDollars(pool.plan);
    const QString rate =
        monthly > 0
            ? QStringLiteral("about $%1/month ($%2/hour)")
                  .arg(monthly, 0, 'f', 2)
                  .arg(monthly / 730.0, 0, 'f', 4)
            : QStringLiteral("the plan's Vultr rate");
    return QStringLiteral("One %1 instance in %2 is billed at %3 until it is "
                          "destroyed. This pool's cap is %4 a month.")
        .arg(pool.plan, pool.region, rate,
             formatDollars(pool.monthlyCapCents));
}

inline QString decisionLine(const PoolStatus &pool, qint64 nowMs)
{
    if (pool.decisionAtMs <= 0 && pool.decisionReason.isEmpty())
        return QStringLiteral("no decision yet");
    QString when;
    if (pool.decisionAtMs > 0) {
        const qint64 seconds = qMax<qint64>(0, (nowMs - pool.decisionAtMs) / 1000);
        when = seconds < 120 ? QStringLiteral("%1 s ago").arg(seconds)
                             : QStringLiteral("%1 min ago").arg(seconds / 60);
    }
    QString action = pool.decisionAction;
    if (action == QLatin1String("none"))
        action = QStringLiteral("hold");
    else if (action == QLatin1String("up"))
        action = QStringLiteral("scale up");
    else if (action == QLatin1String("down"))
        action = QStringLiteral("scale down");
    QString line = action;
    if (!pool.decisionReason.isEmpty())
        line += QStringLiteral(": ") + pool.decisionReason;
    if (!when.isEmpty())
        line += QStringLiteral(" (") + when + QLatin1Char(')');
    return line;
}

inline QStringList scaleArguments(const QString &script, bool runnerPool,
                                  bool up, bool execute)
{
    QStringList arguments{script, QStringLiteral("scale"),
                          runnerPool ? QStringLiteral("runner")
                                     : QStringLiteral("app"),
                          up ? QStringLiteral("up") : QStringLiteral("down")};
    if (execute)
        arguments.append(QStringLiteral("--execute"));
    return arguments;
}

inline QString fleetPython(const QString &script)
{
    const QString configured =
        qEnvironmentVariable("FORKMESH_FLEET_PYTHON").trimmed();
    if (!configured.isEmpty())
        return configured;
    if (!script.isEmpty()) {
        const QString venv = QDir(QFileInfo(script).absolutePath())
                                 .absoluteFilePath(
                                     QStringLiteral("../.venv/bin/python"));
        if (QFileInfo(venv).isExecutable())
            return QDir::cleanPath(venv);
    }
    return QStringLiteral("python3");
}

} // namespace forkmesh::autoscale

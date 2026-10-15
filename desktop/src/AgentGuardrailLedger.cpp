#include "AgentGuardrailLedger.h"

#include "BackgroundFileWriter.h"
#include "CoreInsightsHook.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <utility>

namespace {

constexpr int kLedgerVersion = 1;

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

qint64 currentMs(qint64 nowMs)
{
    return nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
}

// Re-align an hourly window whose newest bucket started at startMs to the
// bucket holding nowMs, dropping the hours that fell off the front.
QVector<qint64> alignedActivity(const QVector<qint64> &values, qint64 startMs,
                                qint64 nowMs)
{
    const int buckets = AgentGuardrailLedger::kActivityBuckets;
    const qint64 width = AgentGuardrailLedger::kActivityBucketMs;
    if (values.size() != buckets || startMs <= 0)
        return QVector<qint64>(buckets, 0);
    const qint64 shift = ((nowMs / width) * width - startMs) / width;
    if (shift <= 0)
        return values;
    if (shift >= buckets)
        return QVector<qint64>(buckets, 0);
    QVector<qint64> out = values.mid(int(shift));
    while (out.size() < buckets)
        out.append(0);
    return out;
}

qint64 count(const QJsonObject &object, const char *key)
{
    return qMax<qint64>(0, object.value(QLatin1String(key)).toInteger());
}

} // namespace

AgentGuardrailLedger::AgentGuardrailLedger(QString persistPath)
    : m_persistPath(std::move(persistPath))
{
}

AgentGuardrailRepoStats &AgentGuardrailLedger::touch(const QString &repository,
                                                     qint64 nowMs)
{
    AgentGuardrailRepoStats &stats = m_repositories[repository];
    stats.repository = repository;
    stats.lastEventMs = qMax(stats.lastEventMs, nowMs);
    return stats;
}

void AgentGuardrailLedger::bumpActivity(qint64 nowMs)
{
    const qint64 bucket = (nowMs / kActivityBucketMs) * kActivityBucketMs;
    m_activity = alignedActivity(m_activity, m_activityBucketStartMs, nowMs);
    m_activityBucketStartMs = qMax(m_activityBucketStartMs, bucket);
    ++m_activity.last();
}

void AgentGuardrailLedger::recordBlocked(const QString &repository,
                                         const QString &tool,
                                         const QString &path,
                                         bool outsideRepository, qint64 nowMs)
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        forkmesh::coreinsights::forward(QStringLiteral("core.insights.guardrail"),
                                        {{QStringLiteral("op"), QStringLiteral("blocked")},
                                         {QStringLiteral("repository"), repository},
                                         {QStringLiteral("tool"), tool},
                                         {QStringLiteral("path"), path},
                                         {QStringLiteral("outside"), outsideRepository}});
        return;
    }
    if (repository.isEmpty())
        return;
    const qint64 now = currentMs(nowMs);
    AgentGuardrailRepoStats &stats = touch(repository, now);
    ++stats.blocked;
    if (outsideRepository)
        ++stats.blockedOutside;
    const QString toolName = tool.trimmed().isEmpty()
                                 ? QStringLiteral("tool call")
                                 : tool.trimmed();
    ++stats.tools[toolName];
    const QString shownPath = path.trimmed().left(kMaxPathLength);
    if (!shownPath.isEmpty()) {
        stats.recentPaths.removeAll(shownPath);
        stats.recentPaths.prepend(shownPath);
        while (stats.recentPaths.size() > kRecentPaths)
            stats.recentPaths.removeLast();
    }
    bumpActivity(now);
    notify();
}

void AgentGuardrailLedger::recordDecision(const QString &repository,
                                          bool approved, qint64 nowMs)
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        forkmesh::coreinsights::forward(QStringLiteral("core.insights.guardrail"),
                                        {{QStringLiteral("op"), QStringLiteral("decision")},
                                         {QStringLiteral("repository"), repository},
                                         {QStringLiteral("approved"), approved}});
        return;
    }
    if (repository.isEmpty())
        return;
    AgentGuardrailRepoStats &stats = touch(repository, currentMs(nowMs));
    if (approved)
        ++stats.approved;
    else
        ++stats.denied;
    notify();
}

void AgentGuardrailLedger::recordAgentsStopped(const QString &repository,
                                               int count, qint64 nowMs)
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        forkmesh::coreinsights::forward(QStringLiteral("core.insights.guardrail"),
                                        {{QStringLiteral("op"), QStringLiteral("stopped")},
                                         {QStringLiteral("repository"), repository},
                                         {QStringLiteral("count"), count}});
        return;
    }
    if (repository.isEmpty() || count <= 0)
        return;
    touch(repository, currentMs(nowMs)).agentsStopped += count;
    notify();
}

AgentGuardrailRepoStats AgentGuardrailLedger::stats(const QString &repository) const
{
    AgentGuardrailRepoStats stats = m_repositories.value(repository);
    stats.repository = repository;
    return stats;
}

QStringList AgentGuardrailLedger::repositories() const
{
    QStringList keys = m_repositories.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return keys;
}

AgentGuardrailTotals AgentGuardrailLedger::totals(qint64 nowMs) const
{
    AgentGuardrailTotals totals;
    for (const AgentGuardrailRepoStats &stats : m_repositories) {
        totals.blocked += stats.blocked;
        totals.blockedOutside += stats.blockedOutside;
        totals.approved += stats.approved;
        totals.denied += stats.denied;
        totals.agentsStopped += stats.agentsStopped;
        totals.lastEventMs = qMax(totals.lastEventMs, stats.lastEventMs);
        ++totals.repositoriesWithEvents;
    }
    totals.recentBlocked = alignedActivity(m_activity, m_activityBucketStartMs,
                                           currentMs(nowMs));
    return totals;
}

void AgentGuardrailLedger::clear()
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        forkmesh::coreinsights::forward(QStringLiteral("core.insights.clear"),
                                        {{QStringLiteral("what"), QStringLiteral("guardrails")}});
        return;
    }
    m_repositories.clear();
    m_activity.clear();
    m_activityBucketStartMs = 0;
    notify();
}

QJsonObject AgentGuardrailLedger::toJson() const
{
    QJsonObject repositories;
    for (const AgentGuardrailRepoStats &stats : m_repositories) {
        QJsonObject tools;
        for (auto it = stats.tools.cbegin(); it != stats.tools.cend(); ++it)
            tools.insert(it.key(), double(it.value()));
        repositories.insert(
            stats.repository,
            QJsonObject{
                {QStringLiteral("blocked"), double(stats.blocked)},
                {QStringLiteral("blockedOutside"), double(stats.blockedOutside)},
                {QStringLiteral("approved"), double(stats.approved)},
                {QStringLiteral("denied"), double(stats.denied)},
                {QStringLiteral("agentsStopped"), double(stats.agentsStopped)},
                {QStringLiteral("lastEventMs"), double(stats.lastEventMs)},
                {QStringLiteral("tools"), tools},
                {QStringLiteral("recentPaths"),
                 QJsonArray::fromStringList(stats.recentPaths)}});
    }
    QJsonArray activity;
    for (qint64 value : m_activity)
        activity.append(double(value));
    return QJsonObject{
        {QStringLiteral("version"), kLedgerVersion},
        {QStringLiteral("repositories"), repositories},
        {QStringLiteral("activity"), activity},
        {QStringLiteral("activityBucketStartMs"),
         double(m_activityBucketStartMs)}};
}

bool AgentGuardrailLedger::fromJson(const QJsonObject &json, QString *error)
{
    if (json.value(QStringLiteral("version")).toInt() > kLedgerVersion)
        return fail(error, QStringLiteral("guardrail ledger is from a newer version"));
    QHash<QString, AgentGuardrailRepoStats> repositories;
    const QJsonObject stored = json.value(QStringLiteral("repositories")).toObject();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        if (it.key().isEmpty())
            continue;
        const QJsonObject object = it.value().toObject();
        AgentGuardrailRepoStats stats;
        stats.repository = it.key();
        stats.blocked = count(object, "blocked");
        stats.blockedOutside = qMin(stats.blocked, count(object, "blockedOutside"));
        stats.approved = count(object, "approved");
        stats.denied = count(object, "denied");
        stats.agentsStopped = count(object, "agentsStopped");
        stats.lastEventMs = count(object, "lastEventMs");
        const QJsonObject tools = object.value(QStringLiteral("tools")).toObject();
        for (auto tool = tools.constBegin(); tool != tools.constEnd(); ++tool)
            if (const qint64 calls = qMax<qint64>(0, tool.value().toInteger()); calls > 0)
                stats.tools.insert(tool.key(), calls);
        for (const QJsonValue &path :
             object.value(QStringLiteral("recentPaths")).toArray()) {
            if (path.isString() && stats.recentPaths.size() < kRecentPaths)
                stats.recentPaths.append(path.toString().left(kMaxPathLength));
        }
        repositories.insert(stats.repository, stats);
    }
    QVector<qint64> activity;
    for (const QJsonValue &value : json.value(QStringLiteral("activity")).toArray())
        activity.append(qMax<qint64>(0, value.toInteger()));
    m_repositories = std::move(repositories);
    m_activity = activity.size() == kActivityBuckets ? activity : QVector<qint64>();
    m_activityBucketStartMs =
        m_activity.isEmpty()
            ? 0
            : qMax<qint64>(0, json.value(QStringLiteral("activityBucketStartMs"))
                                  .toInteger());
    return true;
}

bool AgentGuardrailLedger::load(QString *error)
{
    if (m_persistPath.isEmpty())
        return fail(error, QStringLiteral("no ledger path"));
    QFile file(m_persistPath);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("cannot open %1").arg(m_persistPath));
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(error, QStringLiteral("guardrail ledger is not valid JSON"));
    return fromJson(doc.object(), error);
}

bool AgentGuardrailLedger::save(QString *error) const
{
    if (m_persistPath.isEmpty())
        return fail(error, QStringLiteral("no ledger path"));
    if (forkmesh::coreinsights::owns(m_persistPath))
        return true;
    return forkmesh::diskwrite::writeNow(
        m_persistPath, QJsonDocument(toJson()).toJson(QJsonDocument::Compact),
        error);
}

void AgentGuardrailLedger::saveLater() const
{
    if (forkmesh::coreinsights::owns(m_persistPath))
        return;
    forkmesh::diskwrite::writeLater(
        m_persistPath, QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

void AgentGuardrailLedger::notify()
{
    if (!forkmesh::coreinsights::owns(m_persistPath))
        forkmesh::coreinsights::hooks().guardrailsLocalDirty = true;
    if (changed)
        changed();
}

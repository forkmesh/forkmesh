#include "CoreAgentPages.h"

#include "AgentStore.h"
#include "AppPaths.h"
#include "CoreGit.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

namespace forkmesh::coreagents {

namespace {

constexpr qint64 kDiffStatsTimeoutMs = 30LL * 60 * 1000;
constexpr qint64 kSearchTimeoutMs = 120000;

QString patchPath(const AgentStore &store, const AgentSession &session)
{
    return QFileInfo(store.logPath(session)).absolutePath() + QStringLiteral("/changes.patch");
}

bool storeIsTheCoreStore(const AgentStore &store, const QList<AgentSession> &sessions)
{
    if (sessions.isEmpty())
        return true;
    const QString root = QDir::cleanPath(forkmesh::paths::dataRoot() + QStringLiteral("/agents/sessions")) +
                         QLatin1Char('/');
    return QDir::cleanPath(store.logPath(sessions.first())).startsWith(root);
}

DiffStat statFrom(const QJsonObject &object)
{
    DiffStat stat;
    stat.files = object.value(QStringLiteral("files")).toInt(-1);
    stat.ahead = object.value(QStringLiteral("ahead")).toInt(-1);
    stat.behind = object.value(QStringLiteral("behind")).toInt(-1);
    stat.conflicted = object.value(QStringLiteral("conflicted")).toBool();
    stat.base = object.value(QStringLiteral("base")).toString();
    stat.worktree = object.value(QStringLiteral("worktree")).toString();
    stat.dirty = object.value(QStringLiteral("dirty")).toInt(-1);
    stat.added = object.value(QStringLiteral("added")).toInt(-1);
    stat.removed = object.value(QStringLiteral("removed")).toInt(-1);
    return stat;
}

}

bool diffStats(const AgentStore &store, const QList<AgentSession> &sessions, const DiffRequest &request,
               const DiffStatHandler &onStat, DiffResult *result)
{
    if (!result || request.gitDir.isEmpty() || !coregit::attached())
        return false;
    QJsonArray list;
    for (const AgentSession &session : sessions) {
        list.append(QJsonObject{
            {QStringLiteral("id"), session.id},
            {QStringLiteral("status"), session.status},
            {QStringLiteral("branch"), session.branchName},
            {QStringLiteral("baseBranch"), session.baseBranch},
            {QStringLiteral("merged"), session.merged},
            {QStringLiteral("finishedAtMs"), double(session.finishedAtMs)},
            {QStringLiteral("prNumber"), session.prNumber},
            {QStringLiteral("patchPath"), patchPath(store, session)},
            {QStringLiteral("known"), request.cachedIds.contains(session.id)},
            {QStringLiteral("signature"), request.oldSignatures.value(session.id)},
        });
    }
    const QString token = QStringLiteral("diff-%1").arg(quintptr(result), 0, 16);
    const QJsonObject params{{QStringLiteral("path"), request.gitDir},
                             {QStringLiteral("configuredBase"), request.configuredBase},
                             {QStringLiteral("checkedOut"), request.checkedOut},
                             {QStringLiteral("selected"), request.selectedSessionId},
                             {QStringLiteral("token"), token},
                             {QStringLiteral("sessions"), list}};
    QJsonObject reply;
    QString error;
    const auto onEvent = [&onStat, &token](const QString &event, const QJsonObject &data) {
        if (event != QLatin1String("core.git.agentDiffStat") ||
            data.value(QStringLiteral("token")).toString() != token || !onStat)
            return;
        onStat(data.value(QStringLiteral("id")).toInt(), statFrom(data.value(QStringLiteral("stat")).toObject()),
               data.value(QStringLiteral("signature")).toString());
    };
    if (!coregit::callCore(QStringLiteral("core.git.agentDiffStats"), params, kDiffStatsTimeoutMs, &reply, &error,
                           nullptr, onEvent))
        return false;
    result->base = reply.value(QStringLiteral("base")).toString();
    for (const QJsonValue &value : reply.value(QStringLiteral("stats")).toArray()) {
        const QJsonObject entry = value.toObject();
        result->stats.insert(entry.value(QStringLiteral("id")).toInt(),
                             statFrom(entry.value(QStringLiteral("stat")).toObject()));
    }
    for (const QJsonValue &value : reply.value(QStringLiteral("signatures")).toArray()) {
        const QJsonObject entry = value.toObject();
        result->signatures.insert(entry.value(QStringLiteral("id")).toInt(),
                                  entry.value(QStringLiteral("signature")).toString());
    }
    for (const QJsonValue &value : reply.value(QStringLiteral("liveIds")).toArray())
        result->liveIds.insert(value.toInt());
    return true;
}

bool searchTranscripts(const AgentStore &store, const QList<AgentSession> &sessions, const QString &needle,
                       QHash<int, TranscriptHit> *hits)
{
    if (!hits || !coregit::attached() || !storeIsTheCoreStore(store, sessions))
        return false;
    AgentStore::flushTranscriptWrites();
    QJsonArray ids;
    for (const AgentSession &session : sessions)
        ids.append(session.id);
    QJsonObject reply;
    QString error;
    if (!coregit::callCore(QStringLiteral("core.agents.search"),
                           {{QStringLiteral("needle"), needle}, {QStringLiteral("ids"), ids}}, kSearchTimeoutMs,
                           &reply, &error))
        return false;
    hits->clear();
    for (const QJsonValue &value : reply.value(QStringLiteral("hits")).toArray()) {
        const QJsonObject hit = value.toObject();
        TranscriptHit entry;
        entry.count = hit.value(QStringLiteral("count")).toInt();
        entry.snippet = hit.value(QStringLiteral("snippet")).toString();
        if (entry.count > 0)
            hits->insert(hit.value(QStringLiteral("id")).toInt(), entry);
    }
    return true;
}

}

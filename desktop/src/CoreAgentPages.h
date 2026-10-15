#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>

class AgentStore;
struct AgentSession;

namespace forkmesh::coreagents {

struct DiffStat {
    int files = -1;
    int ahead = -1;
    int behind = -1;
    bool conflicted = false;
    QString base;
    QString worktree;
    int dirty = -1;
    int added = -1;
    int removed = -1;
};

struct DiffRequest {
    QString gitDir;
    QString configuredBase;
    QString checkedOut;
    int selectedSessionId = 0;
    QHash<int, QString> oldSignatures;
    QSet<int> cachedIds;
};

struct DiffResult {
    QString base;
    QHash<int, DiffStat> stats;
    QHash<int, QString> signatures;
    QSet<int> liveIds;
};

using DiffStatHandler = std::function<void(int sessionId, const DiffStat &stat, const QString &signature)>;

bool diffStats(const AgentStore &store, const QList<AgentSession> &sessions, const DiffRequest &request,
               const DiffStatHandler &onStat, DiffResult *result);

struct TranscriptHit {
    int count = 0;
    QString snippet;
};

bool searchTranscripts(const AgentStore &store, const QList<AgentSession> &sessions, const QString &needle,
                       QHash<int, TranscriptHit> *hits);

}

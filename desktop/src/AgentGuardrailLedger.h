#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// Local record of what repository guardrails did: tool calls stopped at a
// repository boundary, the owner's Approve/Deny answers, and agents stopped
// because a boundary tightened. Keyed by "owner/name" and persisted beside the
// agent store so the Guardrails tab can show counts across restarts.
struct AgentGuardrailRepoStats {
    QString repository;
    qint64 blocked = 0;
    qint64 blockedOutside = 0; // of blocked: paths outside the repository
    qint64 approved = 0;
    qint64 denied = 0;
    qint64 agentsStopped = 0;
    qint64 lastEventMs = 0;
    QHash<QString, qint64> tools; // tool name -> blocked calls
    QStringList recentPaths;      // newest first
};

struct AgentGuardrailTotals {
    qint64 blocked = 0;
    qint64 blockedOutside = 0;
    qint64 approved = 0;
    qint64 denied = 0;
    qint64 agentsStopped = 0;
    qint64 lastEventMs = 0;
    int repositoriesWithEvents = 0;
    // Newest last; one bucket per hour for the last day.
    QVector<qint64> recentBlocked;
};

class AgentGuardrailLedger
{
public:
    static constexpr int kActivityBuckets = 24;
    static constexpr qint64 kActivityBucketMs = 60 * 60 * 1000;
    static constexpr int kRecentPaths = 8;
    static constexpr int kMaxPathLength = 300;

    explicit AgentGuardrailLedger(QString persistPath = QString());

    void recordBlocked(const QString &repository, const QString &tool,
                       const QString &path, bool outsideRepository,
                       qint64 nowMs = -1);
    void recordDecision(const QString &repository, bool approved,
                        qint64 nowMs = -1);
    void recordAgentsStopped(const QString &repository, int count,
                             qint64 nowMs = -1);

    AgentGuardrailRepoStats stats(const QString &repository) const;
    QStringList repositories() const; // sorted
    AgentGuardrailTotals totals(qint64 nowMs = -1) const;
    bool hasEvents() const { return !m_repositories.isEmpty(); }

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    void saveLater() const;
    void clear();
    QString persistPath() const { return m_persistPath; }

    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &json, QString *error = nullptr);

    // Invoked after every change so the UI can debounce a refresh and save.
    std::function<void()> changed;

private:
    AgentGuardrailRepoStats &touch(const QString &repository, qint64 nowMs);
    void bumpActivity(qint64 nowMs);
    void notify();

    QString m_persistPath;
    QHash<QString, AgentGuardrailRepoStats> m_repositories;
    QVector<qint64> m_activity;
    qint64 m_activityBucketStartMs = 0; // bucket of the newest sample
};

#pragma once

#include "AgentStore.h"
#include "AgentToolEfficiencyLedger.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

struct AgentModelGitCredit {
    QString key;
    QString provider;
    QString model;
    QString name;
    int merges = 0;
    int commits = 0;
    qint64 lastMergeMs = 0;
};

struct AgentModelGitHistory {
    QHash<QString, AgentModelGitCredit> models;
    int mergesScanned = 0;
    int mergesCredited = 0;
    int commitsScanned = 0;

    void add(const AgentModelGitHistory &other);
};

struct AgentModelCatalogEntry {
    QString provider;
    QString model;
    QString label;
    QString agentName;
};

struct AgentModelStatsRow {
    QString key;
    QString provider;
    QString model;
    QString label;
    QString agentName;
    int catalogIndex = -1;
    int runs = 0;
    int retiredRuns = 0;
    int active = 0;
    int succeeded = 0;
    int failed = 0;
    int stopped = 0;
    int merged = 0;
    int gitMerges = 0;
    int gitCommits = 0;
    int pullRequests = 0;
    qint64 promptTokens = 0;
    qint64 completionTokens = 0;
    qint64 peakContextTokens = 0;
    double reportedUsd = 0.0;
    double estimatedUsd = 0.0;
    qint64 turns = 0;
    int timedRuns = 0;
    qint64 runMs = 0;
    qint64 toolCalls = 0;
    qint64 toolErrors = 0;
    qint64 toolTokens = 0;
    qint64 toolSavedTokens = 0;
    qint64 toolLatencyMsTotal = 0;
    qint64 toolLatencySamples = 0;
    qint64 lastUsedMs = 0;

    int finished() const { return succeeded + failed + stopped; }
    double successRate() const;
    double failureRate() const;
    double mergeRate() const;
    double costUsd() const { return reportedUsd + estimatedUsd; }
    double costPerRun() const;
    double costPerMerge() const;
    double turnsPerRun() const;
    double averageRunMs() const;
    double toolErrorRate() const;
    double tokensPerToolCall() const;
    double toolCallsPerRun() const;
    double averageToolLatencyMs() const;
    bool hasActivity() const;
};

namespace AgentModelStats {

QString rowKey(const QString &provider, const QString &model);
QStringList gitLogArguments(const QString &ref);
QString trailerModelName(const QString &value);
AgentModelGitHistory parseGitHistory(const QByteArray &log);
QVector<AgentModelStatsRow> build(const QList<AgentModelCatalogEntry> &catalog,
                                  const QList<AgentSession> &sessions,
                                  const QHash<QString, AgentModelOutcome> &retired,
                                  const AgentModelGitHistory &git,
                                  const QVector<AgentToolModelStats> &tools,
                                  qint64 nowMs);

} // namespace AgentModelStats

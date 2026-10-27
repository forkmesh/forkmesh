#pragma once

#include "AgentToolEfficiencyLedger.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

struct AgentRunRecord {
    int sessionId = 0;
    QString title;
    QString provider;
    QString model;
    QStringList disabledFamilies;
    QString outcome;
    qint64 startedMs = 0;
    qint64 endedMs = 0;
    qint64 durationMs = 0;
    qint64 modelCalls = 0;
    qint64 inputTokens = 0;
    qint64 cachedInputTokens = 0;
    qint64 cacheWriteTokens = 0;
    qint64 outputTokens = 0;
    qint64 toolCalls = 0;
    qint64 toolErrors = 0;
    qint64 toolInputTokens = 0;
    qint64 toolResultTokens = 0;
    qint64 toolSavedTokens = 0;
    qint64 toolLatencyMs = 0;
    qint64 signalCount = 0;
    qint64 tokensAtStake = 0;
    double estimatedUsd = 0.0;
    QHash<QString, qint64> familyCalls;

    qint64 contextTokens() const { return inputTokens + cachedInputTokens + cacheWriteTokens; }
    qint64 toolTokens() const { return toolInputTokens + toolResultTokens; }
    double efficiency() const;
    bool failed() const;
    QString configLabel() const;

    QJsonObject toJson() const;
    static AgentRunRecord fromJson(const QJsonObject &json);
};

struct AgentRunComparison {
    QString provider;
    QString config;
    QStringList disabledFamilies;
    qint64 runs = 0;
    qint64 failedRuns = 0;
    qint64 durationMs = 0;
    qint64 modelCalls = 0;
    qint64 contextTokens = 0;
    qint64 outputTokens = 0;
    qint64 toolCalls = 0;
    qint64 toolErrors = 0;
    qint64 toolTokens = 0;
    qint64 savedTokens = 0;
    qint64 signalCount = 0;
    qint64 tokensAtStake = 0;
    double estimatedUsd = 0.0;

    double perRun(qint64 total) const { return runs > 0 ? double(total) / runs : 0.0; }
    double usdPerRun() const { return runs > 0 ? estimatedUsd / runs : 0.0; }
    double toolErrorRate() const { return toolCalls > 0 ? double(toolErrors) / toolCalls : 0.0; }
    double efficiency() const;
    bool allToolsOn() const { return disabledFamilies.isEmpty(); }
};

class AgentRunEfficiencyLog
{
public:
    static constexpr int kMaxRuns = 2000;
    static constexpr qint64 kRotateBytes = 4 * 1024 * 1024;

    struct RunContext {
        int sessionId = 0;
        QString title;
        QString provider;
        QString model;
        QStringList disabledFamilies;
    };

    explicit AgentRunEfficiencyLog(QString logPath = QString());

    bool ingest(const RunContext &context, const QJsonObject &event, qint64 nowMs = -1);
    void observeCall(const AgentToolCallObservation &call);
    void noteSignal(int sessionId, qint64 tokens);
    bool finishOpenRun(int sessionId, const QString &outcome, qint64 nowMs = -1);
    bool hasOpenRun(int sessionId) const { return m_open.contains(sessionId); }

    QVector<AgentRunRecord> runs() const { return m_runs; }
    QVector<AgentRunComparison> comparison() const;
    QString comparisonMarkdown() const;

    bool load(QString *error = nullptr);
    void clear();
    QString logPath() const { return m_logPath; }

    std::function<void()> changed;
    std::function<void(const AgentRunRecord &)> runFinished;

private:
    struct ClaudeUsage {
        qint64 input = 0;
        qint64 cacheRead = 0;
        qint64 cacheWrite = 0;
        qint64 output = 0;
    };
    struct OpenRun {
        AgentRunRecord record;
        QHash<QString, ClaudeUsage> claudeUsage;
        QJsonObject codexBaseline;
        QJsonObject codexLatest;
        qint64 codexUpdates = 0;
    };

    OpenRun &openRun(const RunContext &context, qint64 nowMs);
    void finish(OpenRun run, const QJsonObject &result, qint64 nowMs);
    void appendLine(const AgentRunRecord &record);

    QString m_logPath;
    QHash<int, OpenRun> m_open;
    QHash<int, QJsonObject> m_codexTotals;
    QVector<AgentRunRecord> m_runs;
};

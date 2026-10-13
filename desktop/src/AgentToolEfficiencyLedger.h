#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

// Live, per-tool-family accounting of what agent tool calls cost and what
// they saved compared with the naive alternative. Every number is derived on
// the owner device from the transcript the runtime already streams: no
// provider billing data is consulted, and token counts are byte-based
// estimates (four bytes per token) so the board can be truthful about them.
//
// Savings are only claimed where the baseline is measurable from local
// files (see AgentToolBaseline). Everything else reports consumption only.
struct AgentToolFamilyStats {
    QString family;
    qint64 calls = 0;
    qint64 errors = 0;
    qint64 inFlight = 0;
    // Calls whose baseline could be measured; savedTokens only counts these.
    qint64 measuredCalls = 0;
    qint64 latencyMsTotal = 0;
    qint64 latencyMsMax = 0;
    qint64 latencySamples = 0;
    qint64 inputTokens = 0;    // tool arguments the model emitted
    qint64 resultTokens = 0;   // tool output injected back into context
    qint64 baselineTokens = 0; // naive alternative for measured calls
    qint64 savedTokens = 0;
    double savedUsd = 0.0;
    double consumedUsd = 0.0;
    qint64 lastCallMs = 0;
    // Calls that arrived while the operator had this family switched off:
    // evidence of whether the runtime honoured the policy.
    qint64 callsWhileOff = 0;
    // Newest last. Per-call tokens saved for the sparkline (measured calls
    // record their saving; unmeasured calls record 0).
    QVector<qint64> recentSaved;
    QHash<QString, qint64> runtimeNames; // runtime tool name -> calls

    qint64 consumedTokens() const { return inputTokens + resultTokens; }
    double averageLatencyMs() const
    {
        return latencySamples > 0 ? double(latencyMsTotal) / latencySamples : 0.0;
    }
    double savingsRatio() const
    {
        return baselineTokens > 0 ? double(savedTokens) / baselineTokens : 0.0;
    }
};

// Usage of one runtime tool, and of each shell command shape the shell tools
// ran ("Bash · git status"), so the tools can be ranked against each other by
// how often they run, how often they fail, how slow they are and how much
// context they cost. Keyed by usageKey().
struct AgentToolUsageStats {
    QString key;
    QString tool;    // runtime tool name ("Bash", "Read", "mcp__x__y")
    QString command; // shell command shape; empty for non-shell tools
    QString family;
    qint64 calls = 0;
    qint64 errors = 0;
    qint64 latencyMsTotal = 0;
    qint64 latencyMsMax = 0;
    qint64 latencySamples = 0;
    qint64 inputTokens = 0;
    qint64 resultTokens = 0;
    qint64 savedTokens = 0;
    double consumedUsd = 0.0;
    qint64 lastCallMs = 0;

    qint64 consumedTokens() const { return inputTokens + resultTokens; }
    double errorRate() const { return calls > 0 ? double(errors) / calls : 0.0; }
    double averageLatencyMs() const
    {
        return latencySamples > 0 ? double(latencyMsTotal) / latencySamples : 0.0;
    }
    double tokensPerCall() const
    {
        return calls > 0 ? double(consumedTokens()) / calls : 0.0;
    }
};

struct AgentToolModelStats {
    QString provider;
    QString model;
    qint64 calls = 0;
    qint64 errors = 0;
    qint64 latencyMsTotal = 0;
    qint64 latencySamples = 0;
    qint64 inputTokens = 0;
    qint64 resultTokens = 0;
    qint64 savedTokens = 0;
    double consumedUsd = 0.0;
    qint64 lastCallMs = 0;

    qint64 consumedTokens() const { return inputTokens + resultTokens; }
    double errorRate() const { return calls > 0 ? double(errors) / calls : 0.0; }
    double tokensPerCall() const
    {
        return calls > 0 ? double(consumedTokens()) / calls : 0.0;
    }
    double averageLatencyMs() const
    {
        return latencySamples > 0 ? double(latencyMsTotal) / latencySamples : 0.0;
    }
};

// One finished or abandoned call as the ledger saw it, handed to observers
// such as the improvement log. resultText is only set for error results.
struct AgentToolCallObservation {
    int sessionId = 0;
    QString family;
    QString runtimeName;
    QString provider;
    QString model;
    QString workdir;
    QJsonObject input;
    qint64 startedMs = 0;
    qint64 completedMs = 0;
    qint64 inputTokens = 0;
    qint64 resultTokens = 0;
    bool isError = false;
    bool abandoned = false;
    bool whileOff = false;
    // The result arrived together with an earlier, concurrent call's result:
    // runtimes deliver a parallel batch in order, so this call's own latency
    // is hidden behind that sibling and must not be blamed as slow.
    bool heldBehindSibling = false;
    QString resultText;
    bool measured = false;
    qint64 baselineTokens = 0;
    qint64 actualTokens = 0;
    bool outputSide = false;
};

struct AgentToolLedgerTotals {
    qint64 calls = 0;
    qint64 errors = 0;
    qint64 inFlight = 0;
    qint64 consumedTokens = 0;
    qint64 savedTokens = 0;
    double savedUsd = 0.0;
    double consumedUsd = 0.0;
    int familiesObserved = 0;
    int familiesTotal = 0;
    qint64 firstCallMs = 0;
    qint64 lastCallMs = 0;
    // Newest last; one bucket per five minutes for the last two hours.
    QVector<qint64> recentCalls;
    QVector<qint64> recentSaved;
};

class AgentToolEfficiencyLedger
{
public:
    static constexpr int kRecentSamples = 24;
    static constexpr int kActivityBuckets = 24;
    static constexpr qint64 kActivityBucketMs = 5 * 60 * 1000;
    static constexpr int kMaxMatchedFiles = 400;
    static constexpr qint64 kMaxPendingCalls = 4096;
    // Results this close behind a concurrent sibling's were released with it.
    static constexpr qint64 kHeldBehindSiblingMs = 1000;
    // Distinct tool/command rows kept; the least recently used go first.
    static constexpr int kMaxUsageRows = 400;

    struct SessionContext {
        int sessionId = 0;
        QString provider;
        QString model;
        QString workdir; // worktree the runtime executes in; empty = unknown
    };

    explicit AgentToolEfficiencyLedger(QString persistPath = QString());

    // Feeds one transcript event (Claude stream-json shape, which Codex is
    // normalized to). Returns true when the event changed any statistic.
    bool ingest(const SessionContext &context, const QJsonObject &event,
                qint64 nowMs = -1);

    // Lower-level entry points for tests and future host tools.
    bool beginCall(const SessionContext &context, const QString &toolUseId,
                   const QString &runtimeToolName, const QJsonObject &input,
                   qint64 nowMs);
    bool completeCall(int sessionId, const QString &toolUseId,
                      const QString &resultText, bool isError, qint64 nowMs);
    // A session ended: forget its in-flight calls so they never count as
    // running forever. Observers see each one as abandoned.
    void abandonSession(int sessionId, qint64 nowMs = -1);

    // Families the operator has switched off. Calls still record; they are
    // additionally counted as callsWhileOff.
    void setDisabledFamilies(const QSet<QString> &families);
    const QSet<QString> &disabledFamilies() const { return m_disabled; }

    AgentToolFamilyStats stats(const QString &family) const;
    QVector<AgentToolFamilyStats> allStats() const; // catalog order
    AgentToolLedgerTotals totals(qint64 nowMs = -1) const;
    // Every tool and shell command shape seen, unordered; the caller sorts.
    QVector<AgentToolUsageStats> usage() const;
    QVector<AgentToolModelStats> modelUsage() const;
    static QString modelUsageKey(const QString &provider, const QString &model);
    // "Bash · git status" for a shell call, the runtime tool name otherwise.
    static QString usageKey(const QString &runtimeToolName, const QString &family,
                            const QJsonObject &input, QString *command = nullptr);
    bool hasSamples() const { return m_totalCalls > 0; }

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    void saveLater() const;
    void clear();
    QString persistPath() const { return m_persistPath; }

    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &json, QString *error = nullptr);

    // Invoked after every change so the UI can debounce a refresh.
    std::function<void()> changed;
    // Invoked for every completed or abandoned call, after its statistics
    // are recorded.
    std::function<void(const AgentToolCallObservation &)> callObserved;

    static qint64 estimateTokens(qint64 bytes)
    {
        return bytes <= 0 ? 0 : (bytes + 3) / 4;
    }

private:
    struct PendingCall {
        int sessionId = 0;
        QString family;
        QString runtimeName;
        QString usageKey;
        QString provider;
        QString model;
        QString workdir;
        QJsonObject input;
        qint64 startedMs = 0;
        qint64 inputTokens = 0;
        bool whileOff = false;
    };

    struct BaselineResult {
        bool measured = false;
        qint64 baselineTokens = 0;
        qint64 actualTokens = 0;
        bool outputSide = false; // saved tokens are model output, not input
    };

    struct LastCompletion {
        qint64 startedMs = 0;
        qint64 completedMs = 0;
    };

    static QString pendingKey(int sessionId, const QString &toolUseId);
    static AgentToolCallObservation observationFor(const PendingCall &call);
    BaselineResult measureBaseline(const PendingCall &call,
                                   const QString &resultText,
                                   qint64 resultTokens) const;
    static qint64 fileBytes(const QString &workdir, const QString &path);
    static qint64 matchedFileBytes(const QString &workdir,
                                   const QString &resultText);
    void bumpActivity(qint64 nowMs, qint64 savedTokens);
    void trimUsage();
    void notify();

    QString m_persistPath;
    QHash<QString, AgentToolFamilyStats> m_families;
    QHash<QString, AgentToolUsageStats> m_usage;
    QHash<QString, AgentToolModelStats> m_models;
    QHash<QString, PendingCall> m_pending;
    QHash<int, LastCompletion> m_lastCompletion;
    QSet<QString> m_disabled;
    QVector<qint64> m_activityCalls;
    QVector<qint64> m_activitySaved;
    qint64 m_activityBucketStartMs = 0;
    qint64 m_totalCalls = 0;
    qint64 m_firstCallMs = 0;
    qint64 m_lastCallMs = 0;
};

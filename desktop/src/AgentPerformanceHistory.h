#pragma once

#include "AgentRunEfficiencyLog.h"
#include "AgentToolEfficiencyLedger.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

#include <array>
#include <functional>

struct AgentPerformanceBucket {
    qint64 startMs = 0;
    qint64 toolCalls = 0;
    qint64 toolErrors = 0;
    qint64 toolTokens = 0;
    qint64 latencyMsTotal = 0;
    qint64 latencySamples = 0;
    qint64 signalCount = 0;
    qint64 tokensAtStake = 0;
    qint64 runs = 0;
    qint64 failedRuns = 0;
    qint64 runDurationMs = 0;
    double usd = 0.0;
    qint64 agentSamples = 0;
    double agentsTotal = 0.0;
    int agentsPeak = 0;
    qint64 memorySamples = 0;
    double memoryPercentTotal = 0.0;
    qint64 cpuSamples = 0;
    double cpuPercentTotal = 0.0;

    void add(const AgentPerformanceBucket &other);
    QJsonObject toJson() const;
    static AgentPerformanceBucket fromJson(const QJsonObject &json);
};

struct AgentPerformanceMarker {
    qint64 ms = 0;
    QString label;
};

struct AgentPerformanceSeries {
    QString metric;
    int period = 0;
    qint64 startMs = 0;
    qint64 bucketMs = 0;
    QVector<double> values;
    QVector<double> previous;
    QVector<QString> notes;
    double summary = 0.0;
    double previousSummary = 0.0;
    bool hasSummary = false;
    bool hasPreviousSummary = false;
    QString summaryNote;

    bool hasData() const;
    double maximum() const;
    double change() const;
};

class AgentPerformanceHistory
{
public:
    enum Period { Hour = 0, Day, Week, Month };
    static constexpr int kPeriodCount = 4;
    static constexpr int kMaxMarkers = 200;

    enum class Unit { Count, Percent, Tokens, Milliseconds, Usd, Agents };

    struct Metric {
        QString id;
        QString title;
        QString description;
        Unit unit = Unit::Count;
        bool higherIsBetter = false;
        bool neutral = false;
        bool sparseIsZero = false;
    };

    struct SystemSample {
        int runningAgents = -1;
        int agentLimit = -1;
        double memoryPercent = -1.0;
        double cpuPercent = -1.0;
        qint64 memoryTotalBytes = 0;
        int cpuCount = 0;
        QString build;
    };

    explicit AgentPerformanceHistory(QString persistPath = QString());

    static const QVector<Metric> &metrics();
    static const Metric *metric(const QString &id);
    static qint64 bucketMs(int period);
    static int bucketCount(int period);
    static qint64 spanMs(int period) { return bucketMs(period) * bucketCount(period); }
    static QString periodLabel(int period);
    static QString periodShortLabel(int period);
    static QString bucketLabel(int period);
    static QString formatValue(Unit unit, double value);

    void recordCall(const AgentToolCallObservation &call);
    void recordSignal(qint64 tokens, qint64 nowMs = -1);
    void recordRun(const AgentRunRecord &run);
    void recordSample(const SystemSample &sample, qint64 nowMs = -1);
    void addMarker(const QString &label, qint64 nowMs = -1);
    bool seedFromRuns(const QVector<AgentRunRecord> &runs);

    AgentPerformanceSeries series(const QString &metricId, int period,
                                  qint64 nowMs = -1) const;
    QVector<AgentPerformanceMarker> markers(int period, qint64 nowMs = -1) const;
    QString summaryMarkdown(int period, qint64 nowMs = -1) const;
    int bucketsStored(int period) const;
    bool isEmpty() const;

    bool load(QString *error = nullptr);
    void saveLater() const;
    bool saveNow(QString *error = nullptr) const;
    QString persistPath() const { return m_persistPath; }
    bool loadedFromDisk() const { return m_loadedFromDisk; }
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &json, QString *error = nullptr);

    std::function<void()> changed;

private:
    using Store = QMap<qint64, AgentPerformanceBucket>;

    template <typename Fn> void update(qint64 ms, Fn &&fn);
    void prune(qint64 nowMs);
    void notify();
    static double bucketValue(const QString &metricId,
                              const AgentPerformanceBucket &bucket, bool *has);
    static QString bucketNote(const QString &metricId,
                              const AgentPerformanceBucket &bucket);

    QString m_persistPath;
    std::array<Store, kPeriodCount> m_buckets;
    QVector<AgentPerformanceMarker> m_markers;
    SystemSample m_lastEnvironment;
    bool m_hasEnvironment = false;
    bool m_loadedFromDisk = false;
    bool m_seeded = false;
};

#include "AgentPerformanceHistory.h"

#include "BackgroundFileWriter.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kSchemaVersion = 1;

qint64 nowOr(qint64 ms)
{
    return ms >= 0 ? ms : QDateTime::currentMSecsSinceEpoch();
}

double missingValue()
{
    return std::numeric_limits<double>::quiet_NaN();
}

QString compact(double value)
{
    const double magnitude = std::fabs(value);
    if (magnitude >= 1e6)
        return QStringLiteral("%1M").arg(value / 1e6, 0, 'f', magnitude >= 1e7 ? 0 : 1);
    if (magnitude >= 1e3)
        return QStringLiteral("%1K").arg(value / 1e3, 0, 'f', magnitude >= 1e4 ? 0 : 1);
    if (magnitude > 0 && magnitude < 10 && std::fabs(value - std::round(value)) > 0.05)
        return QString::number(value, 'f', 1);
    return QString::number(qRound64(value));
}

void putIf(QJsonObject &json, const char *key, qint64 value)
{
    if (value != 0)
        json.insert(QLatin1String(key), double(value));
}

void putIf(QJsonObject &json, const char *key, double value)
{
    if (value != 0.0)
        json.insert(QLatin1String(key), value);
}

qint64 readInt(const QJsonObject &json, const char *key)
{
    return qint64(json.value(QLatin1String(key)).toDouble());
}

} // namespace

void AgentPerformanceBucket::add(const AgentPerformanceBucket &other)
{
    toolCalls += other.toolCalls;
    toolErrors += other.toolErrors;
    toolTokens += other.toolTokens;
    latencyMsTotal += other.latencyMsTotal;
    latencySamples += other.latencySamples;
    signalCount += other.signalCount;
    tokensAtStake += other.tokensAtStake;
    runs += other.runs;
    failedRuns += other.failedRuns;
    runDurationMs += other.runDurationMs;
    usd += other.usd;
    agentSamples += other.agentSamples;
    agentsTotal += other.agentsTotal;
    agentsPeak = qMax(agentsPeak, other.agentsPeak);
    memorySamples += other.memorySamples;
    memoryPercentTotal += other.memoryPercentTotal;
    cpuSamples += other.cpuSamples;
    cpuPercentTotal += other.cpuPercentTotal;
}

QJsonObject AgentPerformanceBucket::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("t"), double(startMs));
    putIf(json, "calls", toolCalls);
    putIf(json, "errors", toolErrors);
    putIf(json, "tokens", toolTokens);
    putIf(json, "latencyMs", latencyMsTotal);
    putIf(json, "latencyN", latencySamples);
    putIf(json, "signals", signalCount);
    putIf(json, "atStake", tokensAtStake);
    putIf(json, "runs", runs);
    putIf(json, "failed", failedRuns);
    putIf(json, "runMs", runDurationMs);
    putIf(json, "usd", usd);
    putIf(json, "agentN", agentSamples);
    putIf(json, "agents", agentsTotal);
    putIf(json, "agentsPeak", qint64(agentsPeak));
    putIf(json, "memN", memorySamples);
    putIf(json, "mem", memoryPercentTotal);
    putIf(json, "cpuN", cpuSamples);
    putIf(json, "cpu", cpuPercentTotal);
    return json;
}

AgentPerformanceBucket AgentPerformanceBucket::fromJson(const QJsonObject &json)
{
    AgentPerformanceBucket bucket;
    bucket.startMs = readInt(json, "t");
    bucket.toolCalls = readInt(json, "calls");
    bucket.toolErrors = readInt(json, "errors");
    bucket.toolTokens = readInt(json, "tokens");
    bucket.latencyMsTotal = readInt(json, "latencyMs");
    bucket.latencySamples = readInt(json, "latencyN");
    bucket.signalCount = readInt(json, "signals");
    bucket.tokensAtStake = readInt(json, "atStake");
    bucket.runs = readInt(json, "runs");
    bucket.failedRuns = readInt(json, "failed");
    bucket.runDurationMs = readInt(json, "runMs");
    bucket.usd = json.value(QStringLiteral("usd")).toDouble();
    bucket.agentSamples = readInt(json, "agentN");
    bucket.agentsTotal = json.value(QStringLiteral("agents")).toDouble();
    bucket.agentsPeak = int(readInt(json, "agentsPeak"));
    bucket.memorySamples = readInt(json, "memN");
    bucket.memoryPercentTotal = json.value(QStringLiteral("mem")).toDouble();
    bucket.cpuSamples = readInt(json, "cpuN");
    bucket.cpuPercentTotal = json.value(QStringLiteral("cpu")).toDouble();
    return bucket;
}

bool AgentPerformanceSeries::hasData() const
{
    for (double value : values)
        if (!std::isnan(value) && value != 0.0)
            return true;
    return hasSummary && summary != 0.0;
}

double AgentPerformanceSeries::maximum() const
{
    double top = 0.0;
    for (const QVector<double> *line : {&values, &previous})
        for (double value : *line)
            if (!std::isnan(value))
                top = qMax(top, value);
    return top;
}

double AgentPerformanceSeries::change() const
{
    if (!hasSummary || !hasPreviousSummary || previousSummary == 0.0)
        return missingValue();
    return (summary - previousSummary) / std::fabs(previousSummary);
}

AgentPerformanceHistory::AgentPerformanceHistory(QString persistPath)
    : m_persistPath(std::move(persistPath))
{
}

const QVector<AgentPerformanceHistory::Metric> &AgentPerformanceHistory::metrics()
{
    static const QVector<Metric> list = {
        {QStringLiteral("agents"), QStringLiteral("Agents running"),
         QStringLiteral("Agents holding a run slot, sampled every minute; the peak is in the tooltip."),
         Unit::Agents, true, true, false},
        {QStringLiteral("memory"), QStringLiteral("Memory used"),
         QStringLiteral("Share of this computer's memory in use, sampled every minute."),
         Unit::Percent, false, false, false},
        {QStringLiteral("cpu"), QStringLiteral("CPU busy"),
         QStringLiteral("Share of all CPU cores busy, sampled every minute."),
         Unit::Percent, false, false, false},
        {QStringLiteral("toolCalls"), QStringLiteral("Tool calls"),
         QStringLiteral("Tool calls the agents finished."),
         Unit::Count, true, true, true},
        {QStringLiteral("toolErrors"), QStringLiteral("Tool error rate"),
         QStringLiteral("Share of tool calls that came back as an error."),
         Unit::Percent, false, false, false},
        {QStringLiteral("toolLatency"), QStringLiteral("Tool call time"),
         QStringLiteral("Average time from a tool call to its result."),
         Unit::Milliseconds, false, false, false},
        {QStringLiteral("contextPerCall"), QStringLiteral("Context per call"),
         QStringLiteral("Average tokens a tool call adds to the model's context, arguments and result."),
         Unit::Tokens, false, false, false},
        {QStringLiteral("tokensAtStake"), QStringLiteral("Tokens at stake"),
         QStringLiteral("Context spent on calls flagged as potential improvements."),
         Unit::Tokens, false, false, true},
        {QStringLiteral("efficiency"), QStringLiteral("Tool efficiency"),
         QStringLiteral("1 minus tokens at stake over all tool tokens."),
         Unit::Percent, true, false, false},
        {QStringLiteral("runs"), QStringLiteral("Runs finished"),
         QStringLiteral("Agent runs (one prompt to its result) that ended; failures are in the tooltip."),
         Unit::Count, true, false, true},
        {QStringLiteral("runTime"), QStringLiteral("Run time"),
         QStringLiteral("Average wall time of a finished run."),
         Unit::Milliseconds, false, false, false},
        {QStringLiteral("cost"), QStringLiteral("Model cost"),
         QStringLiteral("Estimated model cost of finished runs at list prices."),
         Unit::Usd, false, false, true},
    };
    return list;
}

const AgentPerformanceHistory::Metric *AgentPerformanceHistory::metric(const QString &id)
{
    for (const Metric &entry : metrics())
        if (entry.id == id)
            return &entry;
    return nullptr;
}

qint64 AgentPerformanceHistory::bucketMs(int period)
{
    switch (period) {
    case Hour: return 60 * 1000LL;
    case Day: return 15 * 60 * 1000LL;
    case Week: return 60 * 60 * 1000LL;
    default: return 6 * 60 * 60 * 1000LL;
    }
}

int AgentPerformanceHistory::bucketCount(int period)
{
    switch (period) {
    case Hour: return 60;
    case Day: return 96;
    case Week: return 168;
    default: return 120;
    }
}

QString AgentPerformanceHistory::periodLabel(int period)
{
    switch (period) {
    case Hour: return QStringLiteral("hour");
    case Day: return QStringLiteral("24 hours");
    case Week: return QStringLiteral("7 days");
    default: return QStringLiteral("30 days");
    }
}

QString AgentPerformanceHistory::periodShortLabel(int period)
{
    switch (period) {
    case Hour: return QStringLiteral("1 h");
    case Day: return QStringLiteral("24 h");
    case Week: return QStringLiteral("7 d");
    default: return QStringLiteral("30 d");
    }
}

QString AgentPerformanceHistory::bucketLabel(int period)
{
    switch (period) {
    case Hour: return QStringLiteral("minute");
    case Day: return QStringLiteral("15 minutes");
    case Week: return QStringLiteral("hour");
    default: return QStringLiteral("6 hours");
    }
}

QString AgentPerformanceHistory::formatValue(Unit unit, double value)
{
    if (std::isnan(value))
        return QStringLiteral("–");
    switch (unit) {
    case Unit::Percent:
        return QStringLiteral("%1%").arg(value, 0, 'f', value != 0.0 && std::fabs(value) < 10 ? 1 : 0);
    case Unit::Milliseconds:
        if (value < 1000)
            return QStringLiteral("%1 ms").arg(qRound64(value));
        if (value < 60000)
            return QStringLiteral("%1 s").arg(value / 1000.0, 0, 'f', value < 10000 ? 1 : 0);
        if (value < 3600000)
            return QStringLiteral("%1 min").arg(value / 60000.0, 0, 'f', value < 600000 ? 1 : 0);
        return QStringLiteral("%1 h").arg(value / 3600000.0, 0, 'f', 1);
    case Unit::Usd:
        if (value > 0 && value < 0.01)
            return QStringLiteral("<$0.01");
        return QStringLiteral("$%1").arg(value, 0, 'f', value < 100 ? 2 : 0);
    case Unit::Agents:
        return QString::number(value, 'f', value == std::round(value) ? 0 : 1);
    case Unit::Tokens:
    case Unit::Count:
        break;
    }
    return compact(value);
}

template <typename Fn> void AgentPerformanceHistory::update(qint64 ms, Fn &&fn)
{
    for (int period = 0; period < kPeriodCount; ++period) {
        const qint64 size = bucketMs(period);
        const qint64 start = ms - ms % size;
        auto found = m_buckets[period].find(start);
        if (found == m_buckets[period].end()) {
            AgentPerformanceBucket bucket;
            bucket.startMs = start;
            found = m_buckets[period].insert(start, bucket);
        }
        fn(found.value());
    }
}

void AgentPerformanceHistory::prune(qint64 nowMs)
{
    for (int period = 0; period < kPeriodCount; ++period) {
        const qint64 cutoff = nowMs - nowMs % bucketMs(period) - 2 * spanMs(period);
        Store &store = m_buckets[period];
        while (!store.isEmpty() && store.firstKey() < cutoff)
            store.erase(store.begin());
    }
    const qint64 markerCutoff = nowMs - 2 * spanMs(Month);
    while (!m_markers.isEmpty() &&
           (m_markers.first().ms < markerCutoff || m_markers.size() > kMaxMarkers))
        m_markers.removeFirst();
}

void AgentPerformanceHistory::notify()
{
    if (changed)
        changed();
}

void AgentPerformanceHistory::recordCall(const AgentToolCallObservation &call)
{
    const qint64 ms = call.completedMs > 0 ? call.completedMs : nowOr(-1);
    const qint64 latency = call.completedMs - call.startedMs;
    const bool timed = !call.abandoned && !call.heldBehindSibling && call.startedMs > 0 &&
                       latency >= 0;
    update(ms, [&](AgentPerformanceBucket &bucket) {
        bucket.toolCalls += 1;
        if (call.isError)
            bucket.toolErrors += 1;
        bucket.toolTokens += call.inputTokens + call.resultTokens;
        if (timed) {
            bucket.latencyMsTotal += latency;
            bucket.latencySamples += 1;
        }
    });
    prune(ms);
    notify();
}

void AgentPerformanceHistory::recordSignal(qint64 tokens, qint64 nowMs)
{
    const qint64 ms = nowOr(nowMs);
    update(ms, [&](AgentPerformanceBucket &bucket) {
        bucket.signalCount += 1;
        bucket.tokensAtStake += qMax<qint64>(0, tokens);
    });
    notify();
}

void AgentPerformanceHistory::recordRun(const AgentRunRecord &run)
{
    const qint64 ms = run.endedMs > 0 ? run.endedMs : nowOr(-1);
    update(ms, [&](AgentPerformanceBucket &bucket) {
        bucket.runs += 1;
        if (run.failed())
            bucket.failedRuns += 1;
        bucket.runDurationMs += qMax<qint64>(0, run.durationMs);
        bucket.usd += qMax(0.0, run.estimatedUsd);
    });
    prune(ms);
    notify();
}

void AgentPerformanceHistory::recordSample(const SystemSample &sample, qint64 nowMs)
{
    const qint64 ms = nowOr(nowMs);
    if (m_hasEnvironment) {
        const SystemSample &last = m_lastEnvironment;
        const auto gigabytes = [](qint64 bytes) {
            return qRound64(double(bytes) / (1024.0 * 1024.0 * 1024.0));
        };
        if (sample.memoryTotalBytes > 0 && last.memoryTotalBytes > 0 &&
            gigabytes(sample.memoryTotalBytes) != gigabytes(last.memoryTotalBytes))
            addMarker(QStringLiteral("Memory %1 → %2 GB")
                          .arg(gigabytes(last.memoryTotalBytes))
                          .arg(gigabytes(sample.memoryTotalBytes)),
                      ms);
        if (sample.cpuCount > 0 && last.cpuCount > 0 && sample.cpuCount != last.cpuCount)
            addMarker(QStringLiteral("CPU cores %1 → %2").arg(last.cpuCount).arg(sample.cpuCount),
                      ms);
        if (sample.agentLimit > 0 && last.agentLimit > 0 && sample.agentLimit != last.agentLimit)
            addMarker(QStringLiteral("Agent limit %1 → %2")
                          .arg(last.agentLimit)
                          .arg(sample.agentLimit),
                      ms);
        if (!sample.build.isEmpty() && !last.build.isEmpty() && sample.build != last.build)
            addMarker(QStringLiteral("Build %1").arg(sample.build), ms);
    }
    SystemSample environment = m_hasEnvironment ? m_lastEnvironment : SystemSample();
    if (sample.memoryTotalBytes > 0)
        environment.memoryTotalBytes = sample.memoryTotalBytes;
    if (sample.cpuCount > 0)
        environment.cpuCount = sample.cpuCount;
    if (sample.agentLimit > 0)
        environment.agentLimit = sample.agentLimit;
    if (!sample.build.isEmpty())
        environment.build = sample.build;
    m_lastEnvironment = environment;
    m_hasEnvironment = true;
    update(ms, [&](AgentPerformanceBucket &bucket) {
        if (sample.runningAgents >= 0) {
            bucket.agentSamples += 1;
            bucket.agentsTotal += sample.runningAgents;
            bucket.agentsPeak = qMax(bucket.agentsPeak, sample.runningAgents);
        }
        if (sample.memoryPercent >= 0) {
            bucket.memorySamples += 1;
            bucket.memoryPercentTotal += qBound(0.0, sample.memoryPercent, 100.0);
        }
        if (sample.cpuPercent >= 0) {
            bucket.cpuSamples += 1;
            bucket.cpuPercentTotal += qBound(0.0, sample.cpuPercent, 100.0);
        }
    });
    prune(ms);
    notify();
}

void AgentPerformanceHistory::addMarker(const QString &label, qint64 nowMs)
{
    if (label.trimmed().isEmpty())
        return;
    m_markers.append({nowOr(nowMs), label.trimmed().left(80)});
    prune(nowOr(nowMs));
    notify();
}

bool AgentPerformanceHistory::seedFromRuns(const QVector<AgentRunRecord> &runs)
{
    if (m_seeded || !isEmpty())
        return false;
    m_seeded = true;
    const qint64 now = nowOr(-1);
    const qint64 cutoff = now - 2 * spanMs(Month);
    bool any = false;
    for (const AgentRunRecord &run : runs) {
        if (run.endedMs <= cutoff || run.endedMs > now)
            continue;
        any = true;
        update(run.endedMs, [&](AgentPerformanceBucket &bucket) {
            bucket.runs += 1;
            if (run.failed())
                bucket.failedRuns += 1;
            bucket.runDurationMs += qMax<qint64>(0, run.durationMs);
            bucket.usd += qMax(0.0, run.estimatedUsd);
            bucket.toolCalls += run.toolCalls;
            bucket.toolErrors += run.toolErrors;
            bucket.toolTokens += run.toolTokens();
            if (run.toolLatencyMs > 0 && run.toolCalls > 0) {
                bucket.latencyMsTotal += run.toolLatencyMs;
                bucket.latencySamples += run.toolCalls;
            }
            bucket.signalCount += run.signalCount;
            bucket.tokensAtStake += run.tokensAtStake;
        });
    }
    prune(now);
    if (any)
        notify();
    return any;
}

double AgentPerformanceHistory::bucketValue(const QString &id,
                                            const AgentPerformanceBucket &b, bool *has)
{
    *has = true;
    const auto ratio = [has](double numerator, double denominator, double scale) {
        if (denominator <= 0) {
            *has = false;
            return missingValue();
        }
        return numerator / denominator * scale;
    };
    if (id == QLatin1String("agents"))
        return ratio(b.agentsTotal, double(b.agentSamples), 1.0);
    if (id == QLatin1String("memory"))
        return ratio(b.memoryPercentTotal, double(b.memorySamples), 1.0);
    if (id == QLatin1String("cpu"))
        return ratio(b.cpuPercentTotal, double(b.cpuSamples), 1.0);
    if (id == QLatin1String("toolCalls"))
        return double(b.toolCalls);
    if (id == QLatin1String("toolErrors"))
        return ratio(double(b.toolErrors), double(b.toolCalls), 100.0);
    if (id == QLatin1String("toolLatency"))
        return ratio(double(b.latencyMsTotal), double(b.latencySamples), 1.0);
    if (id == QLatin1String("contextPerCall"))
        return ratio(double(b.toolTokens), double(b.toolCalls), 1.0);
    if (id == QLatin1String("tokensAtStake"))
        return double(b.tokensAtStake);
    if (id == QLatin1String("efficiency")) {
        if (b.toolTokens <= 0) {
            *has = false;
            return missingValue();
        }
        return qBound(0.0, (1.0 - double(b.tokensAtStake) / double(b.toolTokens)) * 100.0,
                      100.0);
    }
    if (id == QLatin1String("runs"))
        return double(b.runs);
    if (id == QLatin1String("runTime"))
        return ratio(double(b.runDurationMs), double(b.runs), 1.0);
    if (id == QLatin1String("cost"))
        return b.usd;
    *has = false;
    return missingValue();
}

QString AgentPerformanceHistory::bucketNote(const QString &id, const AgentPerformanceBucket &b)
{
    if (id == QLatin1String("agents") && b.agentSamples > 0)
        return QStringLiteral("peak %1").arg(b.agentsPeak);
    if ((id == QLatin1String("toolErrors") || id == QLatin1String("toolCalls")) &&
        b.toolCalls > 0)
        return QStringLiteral("%1 errors of %2 calls").arg(compact(double(b.toolErrors)),
                                                           compact(double(b.toolCalls)));
    if ((id == QLatin1String("tokensAtStake") || id == QLatin1String("efficiency")) &&
        b.toolTokens > 0)
        return QStringLiteral("%1 signals, %2 of %3 tool tokens")
            .arg(compact(double(b.signalCount)), compact(double(b.tokensAtStake)),
                 compact(double(b.toolTokens)));
    if (id == QLatin1String("contextPerCall") && b.toolCalls > 0)
        return QStringLiteral("%1 tokens over %2 calls")
            .arg(compact(double(b.toolTokens)), compact(double(b.toolCalls)));
    if ((id == QLatin1String("runs") || id == QLatin1String("runTime")) && b.runs > 0)
        return QStringLiteral("%1 runs, %2 failed").arg(b.runs).arg(b.failedRuns);
    if (id == QLatin1String("cost") && b.runs > 0)
        return QStringLiteral("%1 per run").arg(formatValue(Unit::Usd, b.usd / double(b.runs)));
    return QString();
}

AgentPerformanceSeries AgentPerformanceHistory::series(const QString &metricId, int period,
                                                      qint64 nowMs) const
{
    AgentPerformanceSeries out;
    out.metric = metricId;
    out.period = qBound(0, period, kPeriodCount - 1);
    const Metric *info = metric(metricId);
    const qint64 size = bucketMs(out.period);
    const int count = bucketCount(out.period);
    const qint64 now = nowOr(nowMs);
    out.bucketMs = size;
    out.startMs = now - now % size - qint64(count - 1) * size;
    const qint64 previousStart = out.startMs - qint64(count) * size;
    const Store &store = m_buckets[out.period];
    out.values.fill(missingValue(), count);
    out.previous.fill(missingValue(), count);
    out.notes.resize(count);
    AgentPerformanceBucket total;
    AgentPerformanceBucket previousTotal;
    bool anyCurrent = false;
    bool anyPrevious = false;
    for (auto it = store.lowerBound(previousStart); it != store.cend(); ++it) {
        const qint64 offset = it.key() - previousStart;
        const int index = int(offset / size);
        if (index < 0 || index >= 2 * count)
            continue;
        bool has = false;
        const double value = bucketValue(metricId, it.value(), &has);
        if (index >= count) {
            total.add(it.value());
            anyCurrent = true;
            if (has)
                out.values[index - count] = value;
            out.notes[index - count] = bucketNote(metricId, it.value());
        } else {
            previousTotal.add(it.value());
            anyPrevious = true;
            if (has)
                out.previous[index] = value;
        }
    }
    if (info && info->sparseIsZero) {
        for (double &value : out.values)
            if (std::isnan(value))
                value = 0.0;
        if (anyPrevious)
            for (double &value : out.previous)
                if (std::isnan(value))
                    value = 0.0;
    }
    bool has = false;
    if (anyCurrent) {
        out.summary = bucketValue(metricId, total, &has);
        out.hasSummary = has;
        out.summaryNote = bucketNote(metricId, total);
    }
    if (anyPrevious) {
        out.previousSummary = bucketValue(metricId, previousTotal, &has);
        out.hasPreviousSummary = has;
    }
    return out;
}

QVector<AgentPerformanceMarker> AgentPerformanceHistory::markers(int period, qint64 nowMs) const
{
    const qint64 now = nowOr(nowMs);
    const qint64 size = bucketMs(period);
    const qint64 start = now - now % size - qint64(bucketCount(period) - 1) * size;
    QVector<AgentPerformanceMarker> out;
    for (const AgentPerformanceMarker &marker : m_markers)
        if (marker.ms >= start && marker.ms <= now + size)
            out.append(marker);
    return out;
}

QString AgentPerformanceHistory::summaryMarkdown(int period, qint64 nowMs) const
{
    const QString label = periodLabel(period);
    QStringList lines;
    lines << QStringLiteral("| Metric | Last %1 | The %1 before | Change |").arg(label)
          << QStringLiteral("| --- | ---: | ---: | ---: |");
    for (const Metric &entry : metrics()) {
        const AgentPerformanceSeries s = series(entry.id, period, nowMs);
        const double change = s.change();
        lines << QStringLiteral("| %1 | %2 | %3 | %4 |")
                     .arg(entry.title,
                          s.hasSummary ? formatValue(entry.unit, s.summary) : QStringLiteral("–"),
                          s.hasPreviousSummary ? formatValue(entry.unit, s.previousSummary)
                                               : QStringLiteral("–"),
                          std::isnan(change)
                              ? QStringLiteral("–")
                              : QStringLiteral("%1%2%").arg(change >= 0 ? QStringLiteral("+")
                                                                        : QString())
                                    .arg(change * 100.0, 0, 'f', 0));
    }
    const QVector<AgentPerformanceMarker> changes = markers(period, nowMs);
    if (!changes.isEmpty()) {
        lines << QString() << QStringLiteral("Changes in this period:");
        for (const AgentPerformanceMarker &marker : changes)
            lines << QStringLiteral("- %1: %2")
                         .arg(QDateTime::fromMSecsSinceEpoch(marker.ms)
                                  .toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                              marker.label);
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

int AgentPerformanceHistory::bucketsStored(int period) const
{
    return period >= 0 && period < kPeriodCount ? int(m_buckets[period].size()) : 0;
}

bool AgentPerformanceHistory::isEmpty() const
{
    for (const Store &store : m_buckets)
        if (!store.isEmpty())
            return false;
    return m_markers.isEmpty();
}

QJsonObject AgentPerformanceHistory::toJson() const
{
    QJsonArray periods;
    for (const Store &store : m_buckets) {
        QJsonArray buckets;
        for (const AgentPerformanceBucket &bucket : store)
            buckets.append(bucket.toJson());
        periods.append(buckets);
    }
    QJsonArray markers;
    for (const AgentPerformanceMarker &marker : m_markers)
        markers.append(QJsonObject{{QStringLiteral("ms"), double(marker.ms)},
                                   {QStringLiteral("label"), marker.label}});
    QJsonObject json{{QStringLiteral("schema"), kSchemaVersion},
                     {QStringLiteral("seeded"), m_seeded},
                     {QStringLiteral("periods"), periods},
                     {QStringLiteral("markers"), markers}};
    if (m_hasEnvironment)
        json.insert(QStringLiteral("environment"),
                    QJsonObject{{QStringLiteral("memoryTotalBytes"),
                                 double(m_lastEnvironment.memoryTotalBytes)},
                                {QStringLiteral("cpuCount"), m_lastEnvironment.cpuCount},
                                {QStringLiteral("agentLimit"), m_lastEnvironment.agentLimit},
                                {QStringLiteral("build"), m_lastEnvironment.build}});
    return json;
}

bool AgentPerformanceHistory::fromJson(const QJsonObject &json, QString *error)
{
    if (json.value(QStringLiteral("schema")).toInt() != kSchemaVersion) {
        if (error)
            *error = QStringLiteral("unsupported performance history schema");
        return false;
    }
    for (Store &store : m_buckets)
        store.clear();
    m_markers.clear();
    const QJsonArray periods = json.value(QStringLiteral("periods")).toArray();
    for (int period = 0; period < kPeriodCount && period < periods.size(); ++period) {
        const qint64 size = bucketMs(period);
        for (const QJsonValue &value : periods.at(period).toArray()) {
            const AgentPerformanceBucket bucket = AgentPerformanceBucket::fromJson(value.toObject());
            if (bucket.startMs > 0 && bucket.startMs % size == 0)
                m_buckets[period].insert(bucket.startMs, bucket);
        }
    }
    for (const QJsonValue &value : json.value(QStringLiteral("markers")).toArray()) {
        const QJsonObject marker = value.toObject();
        const qint64 ms = readInt(marker, "ms");
        const QString label = marker.value(QStringLiteral("label")).toString();
        if (ms > 0 && !label.isEmpty())
            m_markers.append({ms, label.left(80)});
    }
    std::sort(m_markers.begin(), m_markers.end(),
              [](const AgentPerformanceMarker &a, const AgentPerformanceMarker &b) {
                  return a.ms < b.ms;
              });
    m_seeded = json.value(QStringLiteral("seeded")).toBool();
    const QJsonObject environment = json.value(QStringLiteral("environment")).toObject();
    m_hasEnvironment = !environment.isEmpty();
    m_lastEnvironment = SystemSample();
    if (m_hasEnvironment) {
        m_lastEnvironment.memoryTotalBytes = readInt(environment, "memoryTotalBytes");
        m_lastEnvironment.cpuCount = environment.value(QStringLiteral("cpuCount")).toInt();
        m_lastEnvironment.agentLimit = environment.value(QStringLiteral("agentLimit")).toInt();
        m_lastEnvironment.build = environment.value(QStringLiteral("build")).toString();
    }
    prune(nowOr(-1));
    return true;
}

bool AgentPerformanceHistory::load(QString *error)
{
    m_loadedFromDisk = false;
    if (m_persistPath.isEmpty())
        return true;
    QFile file(m_persistPath);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = parse.errorString();
        return false;
    }
    if (!fromJson(document.object(), error))
        return false;
    m_loadedFromDisk = true;
    return true;
}

void AgentPerformanceHistory::saveLater() const
{
    if (m_persistPath.isEmpty())
        return;
    forkmesh::diskwrite::writeLater(m_persistPath,
                                    QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

bool AgentPerformanceHistory::saveNow(QString *error) const
{
    if (m_persistPath.isEmpty())
        return true;
    return forkmesh::diskwrite::writeNow(
        m_persistPath, QJsonDocument(toJson()).toJson(QJsonDocument::Compact), error);
}

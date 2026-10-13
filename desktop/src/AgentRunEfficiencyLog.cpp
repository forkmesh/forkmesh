#include "AgentRunEfficiencyLog.h"

#include "AgentTokenPricing.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>

namespace {

qint64 number(const QJsonObject &object, const QString &key)
{
    return qint64(object.value(key).toDouble());
}

qint64 delta(const QJsonObject &latest, const QJsonObject &baseline, const QString &key)
{
    return qMax<qint64>(0, number(latest, key) - number(baseline, key));
}

double clampUnit(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double efficiencyOf(qint64 toolTokens, qint64 stake)
{
    return toolTokens > 0 ? clampUnit(1.0 - double(stake) / double(toolTokens)) : 1.0;
}

QString configLabelFor(const QStringList &disabled)
{
    return disabled.isEmpty() ? QStringLiteral("All tools on")
                              : QStringLiteral("Off: %1").arg(disabled.join(QStringLiteral(", ")));
}

QString compact(double value)
{
    if (value >= 1'000'000)
        return QStringLiteral("%1M").arg(value / 1'000'000.0, 0, 'f', 2);
    if (value >= 10'000)
        return QStringLiteral("%1K").arg(value / 1000.0, 0, 'f', 1);
    return QString::number(qRound64(value));
}

} // namespace

double AgentRunRecord::efficiency() const
{
    return efficiencyOf(toolTokens(), tokensAtStake);
}

bool AgentRunRecord::failed() const
{
    return outcome != QLatin1String("success");
}

QString AgentRunRecord::configLabel() const
{
    return configLabelFor(disabledFamilies);
}

QJsonObject AgentRunRecord::toJson() const
{
    QJsonObject families;
    for (auto it = familyCalls.cbegin(); it != familyCalls.cend(); ++it)
        families.insert(it.key(), double(it.value()));
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("session"), sessionId},
        {QStringLiteral("title"), title},
        {QStringLiteral("provider"), provider},
        {QStringLiteral("model"), model},
        {QStringLiteral("off"), QJsonArray::fromStringList(disabledFamilies)},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("startedMs"), double(startedMs)},
        {QStringLiteral("endedMs"), double(endedMs)},
        {QStringLiteral("durationMs"), double(durationMs)},
        {QStringLiteral("modelCalls"), double(modelCalls)},
        {QStringLiteral("inputTokens"), double(inputTokens)},
        {QStringLiteral("cachedInputTokens"), double(cachedInputTokens)},
        {QStringLiteral("cacheWriteTokens"), double(cacheWriteTokens)},
        {QStringLiteral("outputTokens"), double(outputTokens)},
        {QStringLiteral("toolCalls"), double(toolCalls)},
        {QStringLiteral("toolErrors"), double(toolErrors)},
        {QStringLiteral("toolInputTokens"), double(toolInputTokens)},
        {QStringLiteral("toolResultTokens"), double(toolResultTokens)},
        {QStringLiteral("savedTokens"), double(toolSavedTokens)},
        {QStringLiteral("toolLatencyMs"), double(toolLatencyMs)},
        {QStringLiteral("signals"), double(signalCount)},
        {QStringLiteral("tokensAtStake"), double(tokensAtStake)},
        {QStringLiteral("efficiency"), qRound(efficiency() * 1000) / 1000.0},
        {QStringLiteral("estimatedUsd"), estimatedUsd},
        {QStringLiteral("families"), families}};
}

AgentRunRecord AgentRunRecord::fromJson(const QJsonObject &json)
{
    AgentRunRecord record;
    record.sessionId = json.value(QStringLiteral("session")).toInt();
    record.title = json.value(QStringLiteral("title")).toString();
    record.provider = json.value(QStringLiteral("provider")).toString();
    record.model = json.value(QStringLiteral("model")).toString();
    for (const QJsonValue &family : json.value(QStringLiteral("off")).toArray())
        record.disabledFamilies << family.toString();
    record.outcome = json.value(QStringLiteral("outcome")).toString();
    record.startedMs = number(json, QStringLiteral("startedMs"));
    record.endedMs = number(json, QStringLiteral("endedMs"));
    record.durationMs = number(json, QStringLiteral("durationMs"));
    record.modelCalls = number(json, QStringLiteral("modelCalls"));
    record.inputTokens = number(json, QStringLiteral("inputTokens"));
    record.cachedInputTokens = number(json, QStringLiteral("cachedInputTokens"));
    record.cacheWriteTokens = number(json, QStringLiteral("cacheWriteTokens"));
    record.outputTokens = number(json, QStringLiteral("outputTokens"));
    record.toolCalls = number(json, QStringLiteral("toolCalls"));
    record.toolErrors = number(json, QStringLiteral("toolErrors"));
    record.toolInputTokens = number(json, QStringLiteral("toolInputTokens"));
    record.toolResultTokens = number(json, QStringLiteral("toolResultTokens"));
    record.toolSavedTokens = number(json, QStringLiteral("savedTokens"));
    record.toolLatencyMs = number(json, QStringLiteral("toolLatencyMs"));
    record.signalCount = number(json, QStringLiteral("signals"));
    record.tokensAtStake = number(json, QStringLiteral("tokensAtStake"));
    record.estimatedUsd = json.value(QStringLiteral("estimatedUsd")).toDouble();
    const QJsonObject families = json.value(QStringLiteral("families")).toObject();
    for (auto it = families.constBegin(); it != families.constEnd(); ++it)
        record.familyCalls.insert(it.key(), qint64(it.value().toDouble()));
    return record;
}

double AgentRunComparison::efficiency() const
{
    return efficiencyOf(toolTokens, tokensAtStake);
}

AgentRunEfficiencyLog::AgentRunEfficiencyLog(QString logPath)
    : m_logPath(std::move(logPath))
{
}

AgentRunEfficiencyLog::OpenRun &AgentRunEfficiencyLog::openRun(const RunContext &context,
                                                               qint64 nowMs)
{
    auto found = m_open.find(context.sessionId);
    if (found == m_open.end()) {
        OpenRun run;
        run.record.sessionId = context.sessionId;
        run.record.title = context.title;
        run.record.provider = context.provider;
        run.record.model = context.model;
        run.record.disabledFamilies = context.disabledFamilies;
        run.record.disabledFamilies.sort();
        run.record.startedMs = nowMs;
        found = m_open.insert(context.sessionId, run);
    } else if (found->record.model.isEmpty()) {
        found->record.model = context.model;
    }
    return found.value();
}

bool AgentRunEfficiencyLog::ingest(const RunContext &context, const QJsonObject &event,
                                   qint64 nowMs)
{
    if (nowMs < 0)
        nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("system")) {
        if (event.value(QStringLiteral("subtype")).toString() != QLatin1String("init"))
            return false;
        bool finished = false;
        if (const auto open = m_open.constFind(context.sessionId);
            open != m_open.cend() &&
            (open->record.toolCalls > 0 || !open->claudeUsage.isEmpty() ||
             open->codexUpdates > 0))
            finished = finishOpenRun(context.sessionId, QStringLiteral("no result"), nowMs);
        openRun(context, nowMs);
        return finished;
    }
    if (type == QLatin1String("assistant")) {
        OpenRun &run = openRun(context, nowMs);
        const QJsonObject message = event.value(QStringLiteral("message")).toObject();
        const QString id = message.value(QStringLiteral("id")).toString();
        const QJsonObject usage = message.value(QStringLiteral("usage")).toObject();
        if (id.isEmpty() || usage.isEmpty())
            return false;
        ClaudeUsage &seen = run.claudeUsage[id];
        seen.input = qMax(seen.input, number(usage, QStringLiteral("input_tokens")));
        seen.cacheRead =
            qMax(seen.cacheRead, number(usage, QStringLiteral("cache_read_input_tokens")));
        seen.cacheWrite = qMax(
            seen.cacheWrite, number(usage, QStringLiteral("cache_creation_input_tokens")));
        seen.output = qMax(seen.output, number(usage, QStringLiteral("output_tokens")));
        return true;
    }
    if (type == QLatin1String("user") || type == QLatin1String("_local_user")) {
        openRun(context, nowMs);
        return false;
    }
    if (type == QLatin1String("_codex_usage")) {
        OpenRun &run = openRun(context, nowMs);
        const QJsonObject tokenUsage = event.value(QStringLiteral("tokenUsage")).toObject();
        const QJsonObject total = tokenUsage.value(QStringLiteral("total")).toObject();
        if (total.isEmpty())
            return false;
        if (run.codexUpdates == 0) {
            if (const auto previous = m_codexTotals.constFind(context.sessionId);
                previous != m_codexTotals.cend()) {
                run.codexBaseline = previous.value();
            } else {
                const QJsonObject last = tokenUsage.value(QStringLiteral("last")).toObject();
                for (auto it = total.constBegin(); it != total.constEnd(); ++it)
                    run.codexBaseline.insert(
                        it.key(), it.value().toDouble() - last.value(it.key()).toDouble());
            }
        }
        run.codexLatest = total;
        run.codexUpdates += 1;
        m_codexTotals.insert(context.sessionId, total);
        return true;
    }
    if (type == QLatin1String("result")) {
        openRun(context, nowMs);
        finish(m_open.take(context.sessionId), event, nowMs);
        return true;
    }
    return false;
}

void AgentRunEfficiencyLog::observeCall(const AgentToolCallObservation &call)
{
    auto found = m_open.find(call.sessionId);
    if (found == m_open.end())
        return;
    AgentRunRecord &record = found->record;
    record.toolCalls += 1;
    if (call.isError)
        record.toolErrors += 1;
    if (!call.abandoned && call.completedMs > call.startedMs && call.startedMs > 0)
        record.toolLatencyMs += call.completedMs - call.startedMs;
    record.toolInputTokens += call.inputTokens;
    record.toolResultTokens += call.resultTokens;
    if (call.measured)
        record.toolSavedTokens += qMax<qint64>(0, call.baselineTokens - call.actualTokens);
    record.familyCalls[call.family] += 1;
}

void AgentRunEfficiencyLog::noteSignal(int sessionId, qint64 tokens)
{
    auto found = m_open.find(sessionId);
    if (found == m_open.end())
        return;
    found->record.signalCount += 1;
    found->record.tokensAtStake += qMax<qint64>(0, tokens);
}

bool AgentRunEfficiencyLog::finishOpenRun(int sessionId, const QString &outcome, qint64 nowMs)
{
    if (!m_open.contains(sessionId))
        return false;
    if (nowMs < 0)
        nowMs = QDateTime::currentMSecsSinceEpoch();
    OpenRun run = m_open.take(sessionId);
    run.record.outcome = outcome;
    finish(run, QJsonObject(), nowMs);
    return true;
}

void AgentRunEfficiencyLog::finish(OpenRun run, const QJsonObject &result, qint64 nowMs)
{
    AgentRunRecord &record = run.record;
    record.endedMs = nowMs;
    const qint64 reported = number(result, QStringLiteral("duration_ms"));
    record.durationMs = reported > 0 ? reported : qMax<qint64>(0, nowMs - record.startedMs);
    if (!result.isEmpty()) {
        QString subtype = result.value(QStringLiteral("subtype")).toString();
        if (subtype.isEmpty())
            subtype = QStringLiteral("success");
        if (subtype == QLatin1String("success") &&
            result.value(QStringLiteral("is_error")).toBool())
            subtype = QStringLiteral("error");
        record.outcome = subtype;
    }
    if (!run.claudeUsage.isEmpty()) {
        for (const ClaudeUsage &usage : std::as_const(run.claudeUsage)) {
            record.inputTokens += usage.input;
            record.cachedInputTokens += usage.cacheRead;
            record.cacheWriteTokens += usage.cacheWrite;
            record.outputTokens += usage.output;
        }
        record.modelCalls = run.claudeUsage.size();
    } else if (run.codexUpdates > 0) {
        const qint64 input =
            delta(run.codexLatest, run.codexBaseline, QStringLiteral("inputTokens"));
        record.cachedInputTokens =
            delta(run.codexLatest, run.codexBaseline, QStringLiteral("cachedInputTokens"));
        record.inputTokens = qMax<qint64>(0, input - record.cachedInputTokens);
        record.outputTokens =
            delta(run.codexLatest, run.codexBaseline, QStringLiteral("outputTokens"));
        record.modelCalls = run.codexUpdates;
    } else if (const QJsonObject usage = result.value(QStringLiteral("usage")).toObject();
               !usage.isEmpty()) {
        record.inputTokens = number(usage, QStringLiteral("input_tokens"));
        record.cachedInputTokens = number(usage, QStringLiteral("cache_read_input_tokens"));
        record.cacheWriteTokens = number(usage, QStringLiteral("cache_creation_input_tokens"));
        record.outputTokens = number(usage, QStringLiteral("output_tokens"));
    }
    const AgentTokenPrice price = agentTokenPriceFor(record.provider, record.model);
    record.estimatedUsd =
        (double(record.inputTokens) + double(record.cacheWriteTokens) * 1.25 +
         double(record.cachedInputTokens) * 0.1) *
            price.inputPerMillion / 1e6 +
        double(record.outputTokens) * price.outputPerMillion / 1e6;
    m_runs.append(record);
    if (m_runs.size() > kMaxRuns)
        m_runs.remove(0, m_runs.size() - kMaxRuns);
    appendLine(record);
    if (changed)
        changed();
}

void AgentRunEfficiencyLog::appendLine(const AgentRunRecord &record)
{
    if (m_logPath.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_logPath).absolutePath());
    QFile file(m_logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    file.write(QJsonDocument(record.toJson()).toJson(QJsonDocument::Compact));
    file.write("\n");
}

QVector<AgentRunComparison> AgentRunEfficiencyLog::comparison() const
{
    QHash<QString, int> index;
    QVector<AgentRunComparison> groups;
    for (const AgentRunRecord &run : m_runs) {
        const QString key = run.provider + QLatin1Char('\x1f') +
                            run.disabledFamilies.join(QLatin1Char(','));
        auto found = index.constFind(key);
        if (found == index.cend()) {
            AgentRunComparison group;
            group.provider = run.provider;
            group.disabledFamilies = run.disabledFamilies;
            group.config = run.configLabel();
            groups.append(group);
            found = index.insert(key, int(groups.size() - 1));
        }
        AgentRunComparison &group = groups[found.value()];
        group.runs += 1;
        group.failedRuns += run.failed() ? 1 : 0;
        group.durationMs += run.durationMs;
        group.modelCalls += run.modelCalls;
        group.contextTokens += run.contextTokens();
        group.outputTokens += run.outputTokens;
        group.toolCalls += run.toolCalls;
        group.toolErrors += run.toolErrors;
        group.toolTokens += run.toolTokens();
        group.savedTokens += run.toolSavedTokens;
        group.signalCount += run.signalCount;
        group.tokensAtStake += run.tokensAtStake;
        group.estimatedUsd += run.estimatedUsd;
    }
    std::sort(groups.begin(), groups.end(),
              [](const AgentRunComparison &a, const AgentRunComparison &b) {
                  if (a.provider != b.provider)
                      return a.provider < b.provider;
                  if (a.allToolsOn() != b.allToolsOn())
                      return a.allToolsOn();
                  return a.runs > b.runs;
              });
    return groups;
}

QString AgentRunEfficiencyLog::comparisonMarkdown() const
{
    const QVector<AgentRunComparison> groups = comparison();
    QStringList lines;
    lines << QStringLiteral("# Agent runs: with and without tools turned off")
          << QString()
          << QStringLiteral("%1 runs recorded on this device; tokens are what the "
                            "runtime reported, tool tokens are 4-bytes-per-token "
                            "estimates.")
                 .arg(m_runs.size())
          << QString()
          << QStringLiteral("| Runtime | Tools | Runs | Failed | Avg time | Model calls/run "
                            "| Context/run | Output/run | Tool calls/run | Tool errors "
                            "| Tool tokens/run | At stake/run | Efficiency | Est. $/run |")
          << QStringLiteral("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: "
                            "| ---: | ---: | ---: | ---: | ---: |");
    for (const AgentRunComparison &group : groups)
        lines << QStringLiteral("| %1 | %2 | %3 | %4 | %5 s | %6 | %7 | %8 | %9 | %10% | %11 "
                                "| %12 | %13% | %14 |")
                     .arg(group.provider, group.config)
                     .arg(group.runs)
                     .arg(group.failedRuns)
                     .arg(group.perRun(group.durationMs) / 1000.0, 0, 'f', 0)
                     .arg(group.perRun(group.modelCalls), 0, 'f', 1)
                     .arg(compact(group.perRun(group.contextTokens)),
                          compact(group.perRun(group.outputTokens)))
                     .arg(group.perRun(group.toolCalls), 0, 'f', 1)
                     .arg(group.toolErrorRate() * 100.0, 0, 'f', 0)
                     .arg(compact(group.perRun(group.toolTokens)),
                          compact(group.perRun(group.tokensAtStake)))
                     .arg(group.efficiency() * 100.0, 0, 'f', 0)
                     .arg(QStringLiteral("$%1").arg(group.usdPerRun(), 0, 'f', 2));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool AgentRunEfficiencyLog::load(QString *error)
{
    m_runs.clear();
    if (m_logPath.isEmpty())
        return true;
    QFile file(m_logPath);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const bool oversized = file.size() > kRotateBytes;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject())
            continue;
        m_runs.append(AgentRunRecord::fromJson(document.object()));
    }
    file.close();
    if (m_runs.size() > kMaxRuns)
        m_runs.remove(0, m_runs.size() - kMaxRuns);
    if (oversized) {
        QSaveFile rewrite(m_logPath);
        if (rewrite.open(QIODevice::WriteOnly)) {
            for (const AgentRunRecord &run : std::as_const(m_runs)) {
                rewrite.write(QJsonDocument(run.toJson()).toJson(QJsonDocument::Compact));
                rewrite.write("\n");
            }
            rewrite.commit();
        }
    }
    return true;
}

void AgentRunEfficiencyLog::clear()
{
    m_runs.clear();
    m_open.clear();
    if (!m_logPath.isEmpty())
        QFile::remove(m_logPath);
    if (changed)
        changed();
}

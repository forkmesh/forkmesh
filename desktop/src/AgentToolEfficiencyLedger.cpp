#include "AgentToolEfficiencyLedger.h"

#include "BackgroundFileWriter.h"

#include "AgentTokenPricing.h"
#include "AgentToolCatalog.h"
#include "AgentToolImprovementLog.h"
#include "CoreInsightsHook.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <utility>

namespace {

constexpr int kSchemaVersion = 1;
constexpr int kMaxResultLines = 5000;

qint64 nowOr(qint64 nowMs)
{
    return nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

qint64 nonNegative(const QJsonValue &value)
{
    return value.isDouble() ? qMax<qint64>(0, qint64(value.toDouble())) : 0;
}

QString toolResultText(const QJsonObject &block)
{
    const QJsonValue content = block.value(QStringLiteral("content"));
    if (content.isString())
        return content.toString();
    QString text;
    for (const QJsonValue &part : content.toArray())
        text += part.toObject().value(QStringLiteral("text")).toString();
    return text;
}

QJsonArray toJsonArray(const QVector<qint64> &values)
{
    QJsonArray array;
    for (qint64 value : values)
        array.append(double(value));
    return array;
}

bool fromJsonArray(const QJsonValue &value, QVector<qint64> *out, int cap)
{
    if (!value.isArray())
        return false;
    QVector<qint64> parsed;
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isDouble() || item.toDouble() < 0)
            return false;
        parsed.append(qint64(item.toDouble()));
    }
    if (parsed.size() > cap)
        parsed = parsed.mid(parsed.size() - cap);
    *out = parsed;
    return true;
}

QJsonObject toolBlocksOnly(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    const QString wanted = type == QLatin1String("assistant") ? QStringLiteral("tool_use")
                                                             : QStringLiteral("tool_result");
    QJsonArray blocks;
    for (const QJsonValue &value : event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray())
        if (value.toObject().value(QStringLiteral("type")).toString() == wanted)
            blocks.append(value);
    if (blocks.isEmpty())
        return {};
    return {{QStringLiteral("type"), type},
            {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), blocks}}}};
}

} // namespace

AgentToolEfficiencyLedger::AgentToolEfficiencyLedger(QString persistPath)
    : m_persistPath(std::move(persistPath))
{
}

QString AgentToolEfficiencyLedger::usageKey(const QString &runtimeToolName,
                                            const QString &family,
                                            const QJsonObject &input,
                                            QString *command)
{
    const QString tool =
        runtimeToolName.isEmpty() ? QStringLiteral("?") : runtimeToolName;
    QString shape;
    if (family == QLatin1String("bash")) {
        const QJsonValue value = input.value(QStringLiteral("command"));
        QString line = value.toString();
        if (value.isArray()) {
            QStringList parts;
            for (const QJsonValue &part : value.toArray())
                parts << part.toString();
            line = parts.join(QLatin1Char(' '));
        }
        shape = agentShellCommandShape(line).left(80);
    }
    if (command)
        *command = shape;
    return shape.isEmpty() ? tool : tool + QStringLiteral(" · ") + shape;
}

QVector<AgentToolUsageStats> AgentToolEfficiencyLedger::usage() const
{
    return QVector<AgentToolUsageStats>(m_usage.cbegin(), m_usage.cend());
}

QString AgentToolEfficiencyLedger::modelUsageKey(const QString &provider,
                                                 const QString &model)
{
    return provider.trimmed() + QLatin1Char('\x1f') + model.trimmed().toLower();
}

QVector<AgentToolModelStats> AgentToolEfficiencyLedger::modelUsage() const
{
    return QVector<AgentToolModelStats>(m_models.cbegin(), m_models.cend());
}

void AgentToolEfficiencyLedger::trimUsage()
{
    while (m_usage.size() > kMaxUsageRows) {
        auto oldest = m_usage.begin();
        for (auto it = m_usage.begin(); it != m_usage.end(); ++it)
            if (it->lastCallMs < oldest->lastCallMs)
                oldest = it;
        m_usage.erase(oldest);
    }
}

QString AgentToolEfficiencyLedger::pendingKey(int sessionId,
                                              const QString &toolUseId)
{
    return QString::number(sessionId) + QLatin1Char('\x1f') + toolUseId;
}

bool AgentToolEfficiencyLedger::ingest(const SessionContext &context,
                                       const QJsonObject &event, qint64 nowMs)
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        if (forkmesh::coreinsights::deliveringCoreEvent())
            return false;
        const QJsonObject reduced = toolBlocksOnly(event);
        if (!reduced.isEmpty())
            forkmesh::coreinsights::forward(QStringLiteral("core.insights.ingest"),
                                            {{QStringLiteral("sessionId"), context.sessionId},
                                             {QStringLiteral("provider"), context.provider},
                                             {QStringLiteral("model"), context.model},
                                             {QStringLiteral("workdir"), context.workdir},
                                             {QStringLiteral("event"), reduced}});
        return false;
    }
    const qint64 now = nowOr(nowMs);
    const QString type = event.value(QStringLiteral("type")).toString();
    const QJsonArray content = event.value(QStringLiteral("message"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    bool changed = false;
    if (type == QLatin1String("assistant")) {
        for (const QJsonValue &value : content) {
            const QJsonObject block = value.toObject();
            if (block.value(QStringLiteral("type")).toString() !=
                QLatin1String("tool_use"))
                continue;
            changed |= beginCall(context,
                                 block.value(QStringLiteral("id")).toString(),
                                 block.value(QStringLiteral("name")).toString(),
                                 block.value(QStringLiteral("input")).toObject(),
                                 now);
        }
    } else if (type == QLatin1String("user")) {
        for (const QJsonValue &value : content) {
            const QJsonObject block = value.toObject();
            if (block.value(QStringLiteral("type")).toString() !=
                QLatin1String("tool_result"))
                continue;
            changed |= completeCall(
                context.sessionId,
                block.value(QStringLiteral("tool_use_id")).toString(),
                toolResultText(block),
                block.value(QStringLiteral("is_error")).toBool(), now);
        }
    }
    return changed;
}

bool AgentToolEfficiencyLedger::beginCall(const SessionContext &context,
                                          const QString &toolUseId,
                                          const QString &runtimeToolName,
                                          const QJsonObject &input,
                                          qint64 nowMs)
{
    const QString id = toolUseId.trimmed();
    if (id.isEmpty() || id.size() > 256)
        return false;
    const QString key = pendingKey(context.sessionId, id);
    if (m_pending.contains(key))
        return false;
    if (m_pending.size() >= kMaxPendingCalls) {
        // A runaway runtime that never answers its calls must not grow this
        // table without bound. Drop the oldest in-flight entry.
        auto oldest = m_pending.begin();
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
            if (it->startedMs < oldest->startedMs)
                oldest = it;
        AgentToolFamilyStats &stale = m_families[oldest->family];
        stale.inFlight = qMax<qint64>(0, stale.inFlight - 1);
        m_pending.erase(oldest);
    }

    const qint64 now = nowOr(nowMs);
    PendingCall call;
    call.sessionId = context.sessionId;
    call.runtimeName = runtimeToolName.trimmed().left(200);
    call.family = agentToolFamilyFor(call.runtimeName, input);
    call.provider = context.provider;
    call.model = context.model;
    call.workdir = context.workdir;
    call.input = input;
    call.startedMs = now;
    call.inputTokens = estimateTokens(
        QJsonDocument(input).toJson(QJsonDocument::Compact).size());
    QString commandShape;
    call.usageKey = usageKey(call.runtimeName, call.family, input, &commandShape);

    AgentToolFamilyStats &stats = m_families[call.family];
    stats.family = call.family;
    stats.calls += 1;
    stats.inFlight += 1;
    stats.inputTokens += call.inputTokens;
    stats.lastCallMs = now;
    stats.runtimeNames[call.runtimeName.isEmpty() ? QStringLiteral("?")
                                                  : call.runtimeName] += 1;
    call.whileOff = m_disabled.contains(call.family);
    if (call.whileOff)
        stats.callsWhileOff += 1;
    const AgentTokenPrice price = agentTokenPriceFor(call.provider, call.model);
    stats.consumedUsd += call.inputTokens * price.outputPerMillion / 1e6;

    AgentToolUsageStats &usage = m_usage[call.usageKey];
    usage.key = call.usageKey;
    usage.tool = call.runtimeName.isEmpty() ? QStringLiteral("?") : call.runtimeName;
    usage.command = commandShape;
    usage.family = call.family;
    usage.calls += 1;
    usage.inputTokens += call.inputTokens;
    usage.consumedUsd += call.inputTokens * price.outputPerMillion / 1e6;
    usage.lastCallMs = now;
    trimUsage();

    AgentToolModelStats &model = m_models[modelUsageKey(call.provider, call.model)];
    model.provider = call.provider.trimmed();
    model.model = call.model.trimmed().toLower();
    model.calls += 1;
    model.inputTokens += call.inputTokens;
    model.consumedUsd += call.inputTokens * price.outputPerMillion / 1e6;
    model.lastCallMs = now;

    m_pending.insert(key, call);
    m_totalCalls += 1;
    if (m_firstCallMs == 0 || now < m_firstCallMs)
        m_firstCallMs = now;
    m_lastCallMs = qMax(m_lastCallMs, now);
    bumpActivity(now, 0);
    m_activityCalls.last() += 1;
    notify();
    return true;
}

bool AgentToolEfficiencyLedger::completeCall(int sessionId,
                                             const QString &toolUseId,
                                             const QString &resultText,
                                             bool isError, qint64 nowMs)
{
    const QString key = pendingKey(sessionId, toolUseId.trimmed());
    auto it = m_pending.find(key);
    if (it == m_pending.end())
        return false;
    const PendingCall call = it.value();
    m_pending.erase(it);

    const qint64 now = nowOr(nowMs);
    // A quick call batched after a slow one in the same turn reports only when
    // the slow one does; the previous result, still in flight when this call
    // began and released a moment ago, is what this call waited on.
    const auto previous = m_lastCompletion.constFind(call.sessionId);
    const bool heldBehindSibling =
        previous != m_lastCompletion.constEnd() &&
        previous->startedMs <= call.startedMs &&
        previous->completedMs > call.startedMs &&
        now - previous->completedMs <= kHeldBehindSiblingMs;
    m_lastCompletion.insert(call.sessionId, LastCompletion{call.startedMs, now});
    AgentToolFamilyStats &stats = m_families[call.family];
    stats.family = call.family;
    stats.inFlight = qMax<qint64>(0, stats.inFlight - 1);
    const qint64 latency = now - call.startedMs;
    if (latency >= 0) {
        stats.latencyMsTotal += latency;
        stats.latencyMsMax = qMax(stats.latencyMsMax, latency);
        stats.latencySamples += 1;
    }
    const qint64 resultTokens = estimateTokens(resultText.toUtf8().size());
    stats.resultTokens += resultTokens;
    const AgentTokenPrice price = agentTokenPriceFor(call.provider, call.model);
    stats.consumedUsd += resultTokens * price.inputPerMillion / 1e6;
    if (isError)
        stats.errors += 1;

    qint64 saved = 0;
    BaselineResult baseline;
    if (!isError) {
        baseline = measureBaseline(call, resultText, resultTokens);
        if (baseline.measured) {
            stats.measuredCalls += 1;
            stats.baselineTokens += baseline.baselineTokens;
            saved = qMax<qint64>(0, baseline.baselineTokens -
                                        baseline.actualTokens);
            stats.savedTokens += saved;
            stats.savedUsd += saved *
                              (baseline.outputSide ? price.outputPerMillion
                                                   : price.inputPerMillion) /
                              1e6;
        }
    }
    if (auto usage = m_usage.find(call.usageKey); usage != m_usage.end()) {
        if (latency >= 0) {
            usage->latencyMsTotal += latency;
            usage->latencyMsMax = qMax(usage->latencyMsMax, latency);
            usage->latencySamples += 1;
        }
        usage->resultTokens += resultTokens;
        usage->consumedUsd += resultTokens * price.inputPerMillion / 1e6;
        usage->savedTokens += saved;
        if (isError)
            usage->errors += 1;
        usage->lastCallMs = qMax(usage->lastCallMs, now);
    }
    if (auto model = m_models.find(modelUsageKey(call.provider, call.model));
        model != m_models.end()) {
        if (latency >= 0) {
            model->latencyMsTotal += latency;
            model->latencySamples += 1;
        }
        model->resultTokens += resultTokens;
        model->consumedUsd += resultTokens * price.inputPerMillion / 1e6;
        model->savedTokens += saved;
        if (isError)
            model->errors += 1;
        model->lastCallMs = qMax(model->lastCallMs, now);
    }
    stats.recentSaved.append(saved);
    if (stats.recentSaved.size() > kRecentSamples)
        stats.recentSaved.remove(0, stats.recentSaved.size() - kRecentSamples);
    stats.lastCallMs = qMax(stats.lastCallMs, now);
    m_lastCallMs = qMax(m_lastCallMs, now);
    bumpActivity(now, saved);
    notify();
    if (callObserved) {
        AgentToolCallObservation observation = observationFor(call);
        observation.completedMs = now;
        observation.resultTokens = resultTokens;
        observation.isError = isError;
        observation.heldBehindSibling = heldBehindSibling;
        if (isError)
            observation.resultText =
                resultText.size() <= 2048
                    ? resultText
                    : resultText.left(1792) + QStringLiteral("\n…\n") + resultText.right(256);
        observation.measured = baseline.measured;
        observation.baselineTokens = baseline.baselineTokens;
        observation.actualTokens = baseline.actualTokens;
        observation.outputSide = baseline.outputSide;
        callObserved(observation);
    }
    return true;
}

AgentToolCallObservation
AgentToolEfficiencyLedger::observationFor(const PendingCall &call)
{
    AgentToolCallObservation observation;
    observation.sessionId = call.sessionId;
    observation.family = call.family;
    observation.runtimeName = call.runtimeName;
    observation.provider = call.provider;
    observation.model = call.model;
    observation.workdir = call.workdir;
    observation.input = call.input;
    observation.startedMs = call.startedMs;
    observation.inputTokens = call.inputTokens;
    observation.whileOff = call.whileOff;
    return observation;
}

void AgentToolEfficiencyLedger::setDisabledFamilies(const QSet<QString> &families)
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        QStringList sorted(families.cbegin(), families.cend());
        sorted.sort();
        forkmesh::coreinsights::forward(QStringLiteral("core.insights.disabled"),
                                        {{QStringLiteral("families"), QJsonArray::fromStringList(sorted)}});
    }
    if (m_disabled == families)
        return;
    m_disabled = families;
    notify();
}

void AgentToolEfficiencyLedger::abandonSession(int sessionId, qint64 nowMs)
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        if (!forkmesh::coreinsights::deliveringCoreEvent())
            forkmesh::coreinsights::forward(QStringLiteral("core.insights.abandon"),
                                            {{QStringLiteral("sessionId"), sessionId}});
        return;
    }
    m_lastCompletion.remove(sessionId);
    QVector<AgentToolCallObservation> abandoned;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->sessionId != sessionId) {
            ++it;
            continue;
        }
        AgentToolFamilyStats &stats = m_families[it->family];
        stats.inFlight = qMax<qint64>(0, stats.inFlight - 1);
        AgentToolCallObservation observation = observationFor(it.value());
        observation.abandoned = true;
        abandoned.append(observation);
        it = m_pending.erase(it);
    }
    if (abandoned.isEmpty())
        return;
    notify();
    if (!callObserved)
        return;
    const qint64 now = nowOr(nowMs);
    std::sort(abandoned.begin(), abandoned.end(),
              [](const AgentToolCallObservation &a, const AgentToolCallObservation &b) {
                  return a.startedMs < b.startedMs;
              });
    for (AgentToolCallObservation &observation : abandoned) {
        observation.completedMs = now;
        callObserved(observation);
    }
}

AgentToolEfficiencyLedger::BaselineResult
AgentToolEfficiencyLedger::measureBaseline(const PendingCall &call,
                                           const QString &resultText,
                                           qint64 resultTokens) const
{
    BaselineResult result;
    const AgentToolFamily *family = agentToolFamily(call.family);
    if (!family)
        return result;
    const QJsonObject &input = call.input;
    auto pathOf = [&input]() {
        QString path = input.value(QStringLiteral("file_path")).toString();
        if (path.isEmpty())
            path = input.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            path = input.value(QStringLiteral("notebook_path")).toString();
        return path;
    };

    if (family->baseline == AgentToolBaseline::PartialRead) {
        const QString path = pathOf();
        if (path.isEmpty() || path.contains(QLatin1String("://")))
            return result;
        const qint64 bytes = fileBytes(call.workdir, path);
        if (bytes <= 0)
            return result;
        result.measured = true;
        result.baselineTokens = estimateTokens(bytes);
        result.actualTokens = resultTokens;
        return result;
    }

    if (family->baseline == AgentToolBaseline::AnchoredEdit) {
        qint64 bytes = 0;
        const QJsonArray changes = input.value(QStringLiteral("changes")).toArray();
        if (!changes.isEmpty()) {
            QSet<QString> seen;
            for (const QJsonValue &change : changes) {
                const QString path =
                    change.toObject().value(QStringLiteral("path")).toString();
                if (path.isEmpty() || seen.contains(path))
                    continue;
                seen.insert(path);
                bytes += fileBytes(call.workdir, path);
            }
        } else {
            const QString path = pathOf();
            if (path.isEmpty())
                return result;
            bytes = fileBytes(call.workdir, path);
        }
        if (bytes <= 0)
            return result;
        result.measured = true;
        result.outputSide = true;
        result.baselineTokens = estimateTokens(bytes);
        result.actualTokens = call.inputTokens;
        return result;
    }

    if (family->baseline == AgentToolBaseline::MatchedFiles) {
        const qint64 bytes = matchedFileBytes(call.workdir, resultText);
        if (bytes <= 0)
            return result;
        result.measured = true;
        result.baselineTokens = estimateTokens(bytes);
        result.actualTokens = resultTokens;
        return result;
    }
    return result;
}

qint64 AgentToolEfficiencyLedger::fileBytes(const QString &workdir,
                                            const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 1024 ||
        trimmed.contains(QLatin1Char('\n')) ||
        trimmed.contains(QLatin1Char('\0')))
        return 0;
    QString resolved;
    if (QDir::isAbsolutePath(trimmed))
        resolved = trimmed;
    else if (!workdir.isEmpty())
        resolved = QDir(workdir).filePath(trimmed);
    if (resolved.isEmpty())
        return 0;
    // Only a size lookup: file contents are never read by the ledger.
    const QFileInfo info(resolved);
    return info.isFile() ? info.size() : 0;
}

qint64 AgentToolEfficiencyLedger::matchedFileBytes(const QString &workdir,
                                                   const QString &resultText)
{
    qint64 total = 0;
    int files = 0;
    int lines = 0;
    QSet<QString> seen;
    const auto consider = [&](const QString &candidate) {
        const QString path = candidate.trimmed();
        if (path.isEmpty() || seen.contains(path))
            return;
        seen.insert(path);
        const qint64 bytes = fileBytes(workdir, path);
        if (bytes <= 0)
            return;
        total += bytes;
        files += 1;
    };
    for (const QStringView line : QStringView(resultText).split(QLatin1Char('\n'))) {
        if (++lines > kMaxResultLines || files >= kMaxMatchedFiles)
            break;
        if (line.trimmed().isEmpty())
            continue;
        consider(line.toString());
        // Content and count modes prefix every line with "path:".
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon > 0)
            consider(line.left(colon).toString());
    }
    return total;
}

void AgentToolEfficiencyLedger::bumpActivity(qint64 nowMs, qint64 savedTokens)
{
    const qint64 bucket = (nowMs / kActivityBucketMs) * kActivityBucketMs;
    if (m_activityCalls.size() != kActivityBuckets ||
        m_activitySaved.size() != kActivityBuckets) {
        m_activityCalls = QVector<qint64>(kActivityBuckets, 0);
        m_activitySaved = QVector<qint64>(kActivityBuckets, 0);
        m_activityBucketStartMs = bucket;
    }
    const qint64 shift = (bucket - m_activityBucketStartMs) / kActivityBucketMs;
    if (shift >= kActivityBuckets) {
        m_activityCalls.fill(0);
        m_activitySaved.fill(0);
        m_activityBucketStartMs = bucket;
    } else if (shift > 0) {
        m_activityCalls.remove(0, int(shift));
        m_activitySaved.remove(0, int(shift));
        while (m_activityCalls.size() < kActivityBuckets) {
            m_activityCalls.append(0);
            m_activitySaved.append(0);
        }
        m_activityBucketStartMs = bucket;
    }
    m_activitySaved.last() += savedTokens;
}

void AgentToolEfficiencyLedger::notify()
{
    if (!forkmesh::coreinsights::owns(m_persistPath))
        forkmesh::coreinsights::hooks().toolsLocalDirty = true;
    if (changed)
        changed();
}

AgentToolFamilyStats AgentToolEfficiencyLedger::stats(const QString &family) const
{
    AgentToolFamilyStats copy = m_families.value(family);
    copy.family = family;
    return copy;
}

QVector<AgentToolFamilyStats> AgentToolEfficiencyLedger::allStats() const
{
    QVector<AgentToolFamilyStats> all;
    for (const AgentToolFamily &family : agentToolFamilies())
        all.append(stats(family.id));
    return all;
}

AgentToolLedgerTotals AgentToolEfficiencyLedger::totals(qint64 nowMs) const
{
    AgentToolLedgerTotals totals;
    totals.familiesTotal = agentToolFamilies().size();
    for (const AgentToolFamilyStats &stats : m_families) {
        if (stats.calls <= 0)
            continue;
        totals.familiesObserved += 1;
        totals.calls += stats.calls;
        totals.errors += stats.errors;
        totals.inFlight += stats.inFlight;
        totals.consumedTokens += stats.consumedTokens();
        totals.savedTokens += stats.savedTokens;
        totals.savedUsd += stats.savedUsd;
        totals.consumedUsd += stats.consumedUsd;
    }
    totals.firstCallMs = m_firstCallMs;
    totals.lastCallMs = m_lastCallMs;

    // Shift the activity ring to "now" without mutating the ledger so an
    // idle board still shows the buckets sliding into the past.
    QVector<qint64> calls(kActivityBuckets, 0);
    QVector<qint64> saved(kActivityBuckets, 0);
    if (m_activityCalls.size() == kActivityBuckets &&
        m_activitySaved.size() == kActivityBuckets) {
        const qint64 now = nowOr(nowMs);
        const qint64 bucket = (now / kActivityBucketMs) * kActivityBucketMs;
        const qint64 rawShift =
            (bucket - m_activityBucketStartMs) / kActivityBucketMs;
        const qint64 shift =
            qBound<qint64>(0, rawShift, qint64(kActivityBuckets));
        for (int i = 0; i + shift < kActivityBuckets; ++i) {
            calls[i] = m_activityCalls.at(int(i + shift));
            saved[i] = m_activitySaved.at(int(i + shift));
        }
    }
    totals.recentCalls = calls;
    totals.recentSaved = saved;
    return totals;
}

QJsonObject AgentToolEfficiencyLedger::toJson() const
{
    QJsonObject families;
    for (auto it = m_families.cbegin(); it != m_families.cend(); ++it) {
        const AgentToolFamilyStats &s = it.value();
        QJsonObject names;
        for (auto n = s.runtimeNames.cbegin(); n != s.runtimeNames.cend(); ++n)
            names.insert(n.key(), double(n.value()));
        families.insert(
            it.key(),
            QJsonObject{
                {QStringLiteral("calls"), double(s.calls)},
                {QStringLiteral("errors"), double(s.errors)},
                {QStringLiteral("measuredCalls"), double(s.measuredCalls)},
                {QStringLiteral("latencyMsTotal"), double(s.latencyMsTotal)},
                {QStringLiteral("latencyMsMax"), double(s.latencyMsMax)},
                {QStringLiteral("latencySamples"), double(s.latencySamples)},
                {QStringLiteral("inputTokens"), double(s.inputTokens)},
                {QStringLiteral("resultTokens"), double(s.resultTokens)},
                {QStringLiteral("baselineTokens"), double(s.baselineTokens)},
                {QStringLiteral("savedTokens"), double(s.savedTokens)},
                {QStringLiteral("savedUsd"), s.savedUsd},
                {QStringLiteral("consumedUsd"), s.consumedUsd},
                {QStringLiteral("lastCallMs"), double(s.lastCallMs)},
                {QStringLiteral("callsWhileOff"), double(s.callsWhileOff)},
                {QStringLiteral("recentSaved"), toJsonArray(s.recentSaved)},
                {QStringLiteral("runtimeNames"), names},
            });
    }
    QJsonArray usage;
    for (const AgentToolUsageStats &u : m_usage)
        usage.append(QJsonObject{
            {QStringLiteral("tool"), u.tool},
            {QStringLiteral("command"), u.command},
            {QStringLiteral("family"), u.family},
            {QStringLiteral("calls"), double(u.calls)},
            {QStringLiteral("errors"), double(u.errors)},
            {QStringLiteral("latencyMsTotal"), double(u.latencyMsTotal)},
            {QStringLiteral("latencyMsMax"), double(u.latencyMsMax)},
            {QStringLiteral("latencySamples"), double(u.latencySamples)},
            {QStringLiteral("inputTokens"), double(u.inputTokens)},
            {QStringLiteral("resultTokens"), double(u.resultTokens)},
            {QStringLiteral("savedTokens"), double(u.savedTokens)},
            {QStringLiteral("consumedUsd"), u.consumedUsd},
            {QStringLiteral("lastCallMs"), double(u.lastCallMs)},
        });
    QJsonArray models;
    for (const AgentToolModelStats &m : m_models)
        models.append(QJsonObject{
            {QStringLiteral("provider"), m.provider},
            {QStringLiteral("model"), m.model},
            {QStringLiteral("calls"), double(m.calls)},
            {QStringLiteral("errors"), double(m.errors)},
            {QStringLiteral("latencyMsTotal"), double(m.latencyMsTotal)},
            {QStringLiteral("latencySamples"), double(m.latencySamples)},
            {QStringLiteral("inputTokens"), double(m.inputTokens)},
            {QStringLiteral("resultTokens"), double(m.resultTokens)},
            {QStringLiteral("savedTokens"), double(m.savedTokens)},
            {QStringLiteral("consumedUsd"), m.consumedUsd},
            {QStringLiteral("lastCallMs"), double(m.lastCallMs)},
        });
    return QJsonObject{
        {QStringLiteral("schema"), kSchemaVersion},
        {QStringLiteral("usage"), usage},
        {QStringLiteral("models"), models},
        {QStringLiteral("totalCalls"), double(m_totalCalls)},
        {QStringLiteral("firstCallMs"), double(m_firstCallMs)},
        {QStringLiteral("lastCallMs"), double(m_lastCallMs)},
        {QStringLiteral("activity"),
         QJsonObject{{QStringLiteral("bucketStartMs"),
                      double(m_activityBucketStartMs)},
                     {QStringLiteral("calls"), toJsonArray(m_activityCalls)},
                     {QStringLiteral("saved"), toJsonArray(m_activitySaved)}}},
        {QStringLiteral("families"), families},
    };
}

bool AgentToolEfficiencyLedger::fromJson(const QJsonObject &json, QString *error)
{
    if (json.value(QStringLiteral("schema")).toInt() != kSchemaVersion)
        return fail(error, QStringLiteral("unsupported ledger schema"));
    const QJsonValue familiesValue = json.value(QStringLiteral("families"));
    if (!familiesValue.isObject())
        return fail(error, QStringLiteral("ledger families missing"));

    QHash<QString, AgentToolFamilyStats> families;
    const QJsonObject familiesJson = familiesValue.toObject();
    for (auto it = familiesJson.constBegin(); it != familiesJson.constEnd(); ++it) {
        if (!agentToolFamily(it.key()))
            continue; // a family this build no longer knows
        if (!it.value().isObject())
            return fail(error, QStringLiteral("ledger family is not an object"));
        const QJsonObject o = it.value().toObject();
        for (const char *key :
             {"calls", "errors", "measuredCalls", "latencyMsTotal",
              "latencyMsMax", "latencySamples", "inputTokens", "resultTokens",
              "baselineTokens", "savedTokens", "lastCallMs"}) {
            const QJsonValue v = o.value(QLatin1String(key));
            if (!v.isDouble() || v.toDouble() < 0)
                return fail(error,
                            QStringLiteral("ledger field %1 invalid")
                                .arg(QLatin1String(key)));
        }
        AgentToolFamilyStats s;
        s.family = it.key();
        s.calls = nonNegative(o.value(QStringLiteral("calls")));
        s.errors = nonNegative(o.value(QStringLiteral("errors")));
        s.measuredCalls = nonNegative(o.value(QStringLiteral("measuredCalls")));
        s.latencyMsTotal = nonNegative(o.value(QStringLiteral("latencyMsTotal")));
        s.latencyMsMax = nonNegative(o.value(QStringLiteral("latencyMsMax")));
        s.latencySamples = nonNegative(o.value(QStringLiteral("latencySamples")));
        s.inputTokens = nonNegative(o.value(QStringLiteral("inputTokens")));
        s.resultTokens = nonNegative(o.value(QStringLiteral("resultTokens")));
        s.baselineTokens = nonNegative(o.value(QStringLiteral("baselineTokens")));
        s.savedTokens = nonNegative(o.value(QStringLiteral("savedTokens")));
        s.savedUsd = qMax(0.0, o.value(QStringLiteral("savedUsd")).toDouble());
        s.consumedUsd = qMax(0.0, o.value(QStringLiteral("consumedUsd")).toDouble());
        s.lastCallMs = nonNegative(o.value(QStringLiteral("lastCallMs")));
        if (const QJsonValue off = o.value(QStringLiteral("callsWhileOff"));
            !off.isUndefined()) {
            if (!off.isDouble() || off.toDouble() < 0)
                return fail(error, QStringLiteral("ledger callsWhileOff invalid"));
            s.callsWhileOff = nonNegative(off);
        }
        if (o.contains(QStringLiteral("recentSaved")) &&
            !fromJsonArray(o.value(QStringLiteral("recentSaved")), &s.recentSaved,
                           kRecentSamples))
            return fail(error, QStringLiteral("ledger recentSaved invalid"));
        if (const QJsonValue inFlight = o.value(QStringLiteral("inFlight")); inFlight.isDouble())
            s.inFlight = nonNegative(inFlight);
        const QJsonObject names = o.value(QStringLiteral("runtimeNames")).toObject();
        for (auto n = names.constBegin(); n != names.constEnd(); ++n)
            if (n.value().isDouble() && n.value().toDouble() > 0)
                s.runtimeNames.insert(n.key(), qint64(n.value().toDouble()));
        families.insert(it.key(), s);
    }

    const QJsonObject activity = json.value(QStringLiteral("activity")).toObject();
    QVector<qint64> calls;
    QVector<qint64> saved;
    qint64 bucketStart = 0;
    if (!activity.isEmpty()) {
        bucketStart = nonNegative(activity.value(QStringLiteral("bucketStartMs")));
        if (!fromJsonArray(activity.value(QStringLiteral("calls")), &calls,
                           kActivityBuckets) ||
            !fromJsonArray(activity.value(QStringLiteral("saved")), &saved,
                           kActivityBuckets) ||
            calls.size() != kActivityBuckets || saved.size() != kActivityBuckets)
            return fail(error, QStringLiteral("ledger activity invalid"));
    }

    // Older ledgers have no per-tool rows; a malformed row is skipped rather
    // than failing the whole ledger, since the family totals stand on their own.
    QHash<QString, AgentToolUsageStats> usage;
    for (const QJsonValue &value : json.value(QStringLiteral("usage")).toArray()) {
        const QJsonObject o = value.toObject();
        AgentToolUsageStats u;
        u.tool = o.value(QStringLiteral("tool")).toString().left(200);
        u.command = o.value(QStringLiteral("command")).toString().left(80);
        u.family = o.value(QStringLiteral("family")).toString();
        u.calls = nonNegative(o.value(QStringLiteral("calls")));
        if (u.tool.isEmpty() || u.calls <= 0)
            continue;
        u.key = u.command.isEmpty() ? u.tool : u.tool + QStringLiteral(" · ") + u.command;
        u.errors = qMin(u.calls, nonNegative(o.value(QStringLiteral("errors"))));
        u.latencyMsTotal = nonNegative(o.value(QStringLiteral("latencyMsTotal")));
        u.latencyMsMax = nonNegative(o.value(QStringLiteral("latencyMsMax")));
        u.latencySamples = nonNegative(o.value(QStringLiteral("latencySamples")));
        u.inputTokens = nonNegative(o.value(QStringLiteral("inputTokens")));
        u.resultTokens = nonNegative(o.value(QStringLiteral("resultTokens")));
        u.savedTokens = nonNegative(o.value(QStringLiteral("savedTokens")));
        u.consumedUsd = qMax(0.0, o.value(QStringLiteral("consumedUsd")).toDouble());
        u.lastCallMs = nonNegative(o.value(QStringLiteral("lastCallMs")));
        usage.insert(u.key, u);
    }

    QHash<QString, AgentToolModelStats> models;
    for (const QJsonValue &value : json.value(QStringLiteral("models")).toArray()) {
        const QJsonObject o = value.toObject();
        AgentToolModelStats m;
        m.provider = o.value(QStringLiteral("provider")).toString().trimmed().left(80);
        m.model = o.value(QStringLiteral("model")).toString().trimmed().toLower().left(200);
        m.calls = nonNegative(o.value(QStringLiteral("calls")));
        if (m.provider.isEmpty() || m.calls <= 0)
            continue;
        m.errors = qMin(m.calls, nonNegative(o.value(QStringLiteral("errors"))));
        m.latencyMsTotal = nonNegative(o.value(QStringLiteral("latencyMsTotal")));
        m.latencySamples = nonNegative(o.value(QStringLiteral("latencySamples")));
        m.inputTokens = nonNegative(o.value(QStringLiteral("inputTokens")));
        m.resultTokens = nonNegative(o.value(QStringLiteral("resultTokens")));
        m.savedTokens = nonNegative(o.value(QStringLiteral("savedTokens")));
        m.consumedUsd = qMax(0.0, o.value(QStringLiteral("consumedUsd")).toDouble());
        m.lastCallMs = nonNegative(o.value(QStringLiteral("lastCallMs")));
        models.insert(modelUsageKey(m.provider, m.model), m);
    }

    m_families = families;
    m_usage = usage;
    m_models = models;
    trimUsage();
    m_pending.clear();
    m_activityCalls = calls;
    m_activitySaved = saved;
    m_activityBucketStartMs = bucketStart;
    m_totalCalls = nonNegative(json.value(QStringLiteral("totalCalls")));
    m_firstCallMs = nonNegative(json.value(QStringLiteral("firstCallMs")));
    m_lastCallMs = nonNegative(json.value(QStringLiteral("lastCallMs")));
    return true;
}

bool AgentToolEfficiencyLedger::load(QString *error)
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
        return fail(error, QStringLiteral("ledger is not valid JSON"));
    return fromJson(doc.object(), error);
}

bool AgentToolEfficiencyLedger::save(QString *error) const
{
    if (m_persistPath.isEmpty())
        return fail(error, QStringLiteral("no ledger path"));
    if (forkmesh::coreinsights::owns(m_persistPath))
        return true;
    return forkmesh::diskwrite::writeNow(
        m_persistPath, QJsonDocument(toJson()).toJson(QJsonDocument::Compact),
        error);
}

void AgentToolEfficiencyLedger::saveLater() const
{
    if (forkmesh::coreinsights::owns(m_persistPath))
        return;
    forkmesh::diskwrite::writeLater(
        m_persistPath, QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

void AgentToolEfficiencyLedger::clear()
{
    if (forkmesh::coreinsights::owns(m_persistPath)) {
        forkmesh::coreinsights::forward(QStringLiteral("core.insights.clear"),
                                        {{QStringLiteral("what"), QStringLiteral("tools")}});
        return;
    }
    m_families.clear();
    m_usage.clear();
    m_models.clear();
    m_pending.clear();
    m_activityCalls.clear();
    m_activitySaved.clear();
    m_activityBucketStartMs = 0;
    m_totalCalls = 0;
    m_firstCallMs = 0;
    m_lastCallMs = 0;
    notify();
}

#include "AgentModelStats.h"

#include "AgentTokenPricing.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

struct GitCommit {
    QStringList parents;
    qint64 ms = 0;
    QStringList keys;
};

double ratio(double numerator, double denominator)
{
    return denominator > 0 ? numerator / denominator : 0.0;
}

bool agentEmailDomain(const QString &email)
{
    const QString domain = email.section(QLatin1Char('@'), 1).trimmed().toLower();
    for (const char *known : {"anthropic.com", "openai.com", "cursor.com"})
        if (domain == QLatin1String(known) ||
            domain.endsWith(QStringLiteral(".") + QLatin1String(known)))
            return true;
    return false;
}

QString nameKey(const QString &name)
{
    return QStringLiteral("name:") + name.toLower();
}

} // namespace

void AgentModelGitHistory::add(const AgentModelGitHistory &other)
{
    for (auto it = other.models.cbegin(); it != other.models.cend(); ++it) {
        AgentModelGitCredit &credit = models[it.key()];
        if (credit.key.isEmpty())
            credit = AgentModelGitCredit{it->key, it->provider, it->model, it->name};
        credit.merges += it->merges;
        credit.commits += it->commits;
        credit.lastMergeMs = qMax(credit.lastMergeMs, it->lastMergeMs);
    }
    mergesScanned += other.mergesScanned;
    mergesCredited += other.mergesCredited;
    commitsScanned += other.commitsScanned;
}

double AgentModelStatsRow::successRate() const
{
    return ratio(succeeded, finished());
}

double AgentModelStatsRow::failureRate() const
{
    return ratio(failed, finished());
}

double AgentModelStatsRow::mergeRate() const
{
    return ratio(merged, runs);
}

double AgentModelStatsRow::costPerRun() const
{
    return ratio(costUsd(), runs - retiredRuns);
}

double AgentModelStatsRow::costPerMerge() const
{
    return ratio(costUsd(), merged);
}

double AgentModelStatsRow::turnsPerRun() const
{
    return ratio(double(turns), runs - retiredRuns);
}

double AgentModelStatsRow::averageRunMs() const
{
    return ratio(double(runMs), timedRuns);
}

double AgentModelStatsRow::toolErrorRate() const
{
    return ratio(double(toolErrors), double(toolCalls));
}

double AgentModelStatsRow::tokensPerToolCall() const
{
    return ratio(double(toolTokens), double(toolCalls));
}

double AgentModelStatsRow::toolCallsPerRun() const
{
    return ratio(double(toolCalls), runs - retiredRuns);
}

double AgentModelStatsRow::averageToolLatencyMs() const
{
    return ratio(double(toolLatencyMsTotal), double(toolLatencySamples));
}

bool AgentModelStatsRow::hasActivity() const
{
    return runs > 0 || gitMerges > 0 || gitCommits > 0 || toolCalls > 0;
}

namespace AgentModelStats {

QString rowKey(const QString &provider, const QString &model)
{
    return provider.trimmed() + QLatin1Char('\x1f') + model.trimmed().toLower();
}

QStringList gitLogArguments(const QString &ref)
{
    return {QStringLiteral("log"), ref.isEmpty() ? QStringLiteral("HEAD") : ref,
            QStringLiteral("--format=%x1e%H%x1f%P%x1f%ct%x1f"
                           "%(trailers:key=Co-Authored-By,key=ForkMesh-Agent,"
                           "separator=%x1d)")};
}

QString trailerModelName(const QString &value)
{
    static const QRegularExpression address(
        QStringLiteral("^\\s*(.*?)\\s*<([^>]*)>\\s*$"));
    static const QRegularExpression parenthetical(QStringLiteral("\\s*\\([^)]*\\)"));
    const QRegularExpressionMatch match = address.match(value);
    if (!match.hasMatch() || !agentEmailDomain(match.captured(2)))
        return QString();
    QString name = match.captured(1);
    name.remove(parenthetical);
    name = name.simplified();
    if (name.startsWith(QLatin1String("Claude "), Qt::CaseInsensitive) &&
        name.size() > 7)
        name = name.mid(7);
    return name;
}

AgentModelGitHistory parseGitHistory(const QByteArray &log)
{
    AgentModelGitHistory history;
    QHash<QString, GitCommit> commits;
    QString head;
    const QList<QByteArray> records = log.split('\x1e');
    for (const QByteArray &record : records) {
        const QList<QByteArray> fields = record.split('\x1f');
        if (fields.size() < 4)
            continue;
        const QString hash = QString::fromLatin1(fields.at(0).trimmed());
        if (hash.isEmpty())
            continue;
        if (head.isEmpty())
            head = hash;
        GitCommit commit;
        commit.parents = QString::fromLatin1(fields.at(1).trimmed())
                             .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        commit.ms = fields.at(2).trimmed().toLongLong() * 1000;
        QStringList forkmesh;
        QStringList coauthors;
        const QString trailers = QString::fromUtf8(fields.mid(3).join('\x1f'));
        for (const QString &raw :
             trailers.split(QRegularExpression(QStringLiteral("[\\x1d\\n]")),
                            Qt::SkipEmptyParts)) {
            const int colon = raw.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                continue;
            const QString trailerKey = raw.left(colon).trimmed();
            const QString value = raw.mid(colon + 1).trimmed();
            if (trailerKey.compare(QLatin1String("ForkMesh-Agent"),
                                   Qt::CaseInsensitive) == 0) {
                const QString provider = value.section(QLatin1Char('/'), 0, 0).trimmed();
                const QString model = value.section(QLatin1Char('/'), 1).trimmed();
                if (provider.isEmpty())
                    continue;
                const QString key = rowKey(provider, model);
                AgentModelGitCredit &credit = history.models[key];
                if (credit.key.isEmpty())
                    credit = AgentModelGitCredit{key, provider, model.toLower(),
                                                 model.isEmpty() ? provider : model};
                if (!forkmesh.contains(key))
                    forkmesh.append(key);
            } else {
                const QString name = trailerModelName(value);
                if (name.isEmpty())
                    continue;
                const QString key = nameKey(name);
                AgentModelGitCredit &credit = history.models[key];
                if (credit.key.isEmpty())
                    credit = AgentModelGitCredit{key, QString(), QString(), name};
                if (!coauthors.contains(key))
                    coauthors.append(key);
            }
        }
        commit.keys = forkmesh.isEmpty() ? coauthors : forkmesh;
        for (const QString &key : std::as_const(commit.keys))
            history.models[key].commits += 1;
        history.commitsScanned += 1;
        commits.insert(hash, commit);
    }

    QSet<QString> mainline;
    QStringList chain;
    for (QString at = head; !at.isEmpty() && !mainline.contains(at);) {
        const auto it = commits.constFind(at);
        if (it == commits.constEnd())
            break;
        mainline.insert(at);
        chain.append(at);
        at = it->parents.value(0);
    }

    QSet<QString> claimed;
    for (const QString &hash : std::as_const(chain)) {
        QStringList pending{hash};
        bool onMainline = true;
        while (!pending.isEmpty()) {
            const GitCommit commit = commits.value(pending.takeFirst());
            QHash<QString, int> tally;
            QStringList order;
            const auto count = [&](const GitCommit &c) {
                for (const QString &key : c.keys) {
                    if (!tally.contains(key))
                        order.append(key);
                    tally[key] += 1;
                }
            };
            if (commit.parents.size() >= 2) {
                history.mergesScanned += 1;
                for (int p = 1; p < commit.parents.size(); ++p) {
                    for (QString at = commit.parents.at(p);
                         !at.isEmpty() && !mainline.contains(at) &&
                         !claimed.contains(at);) {
                        const auto it = commits.constFind(at);
                        if (it == commits.constEnd())
                            break;
                        claimed.insert(at);
                        count(*it);
                        if (it->parents.size() >= 2)
                            pending.append(at);
                        at = it->parents.value(0);
                    }
                }
            }
            if (onMainline)
                count(commit);
            onMainline = false;
            if (order.isEmpty())
                continue;
            QString best = order.first();
            for (const QString &key : std::as_const(order))
                if (tally.value(key) > tally.value(best))
                    best = key;
            AgentModelGitCredit &credit = history.models[best];
            credit.merges += 1;
            credit.lastMergeMs = qMax(credit.lastMergeMs, commit.ms);
            history.mergesCredited += 1;
        }
    }
    return history;
}

QVector<AgentModelStatsRow> build(const QList<AgentModelCatalogEntry> &catalog,
                                  const QList<AgentSession> &sessions,
                                  const QHash<QString, AgentModelOutcome> &retired,
                                  const AgentModelGitHistory &git,
                                  const QVector<AgentToolModelStats> &tools,
                                  qint64 nowMs)
{
    QVector<AgentModelStatsRow> rows;
    QHash<QString, int> byKey;
    QHash<QString, int> byLabel;
    const auto ensure = [&](const QString &provider, const QString &model,
                            const QString &label, const QString &agentName) -> int {
        const QString key = rowKey(provider, model);
        if (const auto it = byKey.constFind(key); it != byKey.constEnd())
            return it.value();
        AgentModelStatsRow row;
        row.key = key;
        row.provider = provider.trimmed();
        row.model = model.trimmed();
        row.label = label.isEmpty() ? (model.trimmed().isEmpty()
                                           ? row.provider
                                           : model.trimmed())
                                    : label;
        row.agentName = agentName.isEmpty() ? row.provider : agentName;
        rows.append(row);
        byKey.insert(key, rows.size() - 1);
        const QString labelKey = row.label.toLower();
        if (!byLabel.contains(labelKey))
            byLabel.insert(labelKey, rows.size() - 1);
        return rows.size() - 1;
    };

    for (int i = 0; i < catalog.size(); ++i) {
        const AgentModelCatalogEntry &entry = catalog.at(i);
        const int index = ensure(entry.provider, entry.model, entry.label, entry.agentName);
        if (rows[index].catalogIndex < 0)
            rows[index].catalogIndex = i;
    }

    for (const AgentSession &session : sessions) {
        AgentModelStatsRow &row =
            rows[ensure(session.provider, session.model, QString(), QString())];
        row.runs += 1;
        if (session.merged || session.status == AgentStatus::Success)
            row.succeeded += 1;
        else if (session.status == AgentStatus::Failed)
            row.failed += 1;
        else if (session.status == AgentStatus::Stopped ||
                 session.status == AgentStatus::Cleared)
            row.stopped += 1;
        else
            row.active += 1;
        if (session.merged)
            row.merged += 1;
        if (session.prNumber > 0)
            row.pullRequests += 1;
        row.promptTokens += qMax(0, session.promptTokens);
        row.completionTokens += qMax(0, session.completionTokens);
        row.peakContextTokens =
            qMax<qint64>(row.peakContextTokens, session.contextTokens);
        if (session.costUsd > 0) {
            row.reportedUsd += session.costUsd;
        } else {
            const AgentTokenPrice price =
                agentTokenPriceFor(session.provider, session.model);
            row.estimatedUsd +=
                (qMax(0, session.promptTokens) * price.inputPerMillion +
                 qMax(0, session.completionTokens) * price.outputPerMillion) /
                1e6;
        }
        row.turns += qMax(0, session.numTurns);
        const qint64 ran = session.totalRunMs(nowMs);
        if (ran > 0) {
            row.timedRuns += 1;
            row.runMs += ran;
        }
        row.lastUsedMs = std::max({row.lastUsedMs, session.createdAtMs,
                               session.startedAtMs, session.finishedAtMs});
    }

    for (auto it = retired.cbegin(); it != retired.cend(); ++it) {
        const QString provider = it.key().section(QLatin1Char('\x1f'), 0, 0);
        const QString model = it.key().section(QLatin1Char('\x1f'), 1);
        AgentModelStatsRow &row = rows[ensure(provider, model, QString(), QString())];
        row.runs += it->runs;
        row.retiredRuns += it->runs;
        row.merged += it->merged;
    }

    for (const AgentModelGitCredit &credit : git.models) {
        int index = -1;
        if (!credit.provider.isEmpty())
            index = ensure(credit.provider, credit.model, QString(), QString());
        else
            index = byLabel.value(credit.name.toLower(), -1);
        if (index < 0) {
            AgentModelStatsRow row;
            row.key = credit.key;
            row.label = credit.name;
            row.agentName = QStringLiteral("git history");
            rows.append(row);
            index = rows.size() - 1;
            byKey.insert(credit.key, index);
            byLabel.insert(credit.name.toLower(), index);
        }
        AgentModelStatsRow &row = rows[index];
        row.gitMerges += credit.merges;
        row.gitCommits += credit.commits;
        row.lastUsedMs = qMax(row.lastUsedMs, credit.lastMergeMs);
    }

    for (const AgentToolModelStats &tool : tools) {
        AgentModelStatsRow &row =
            rows[ensure(tool.provider, tool.model, QString(), QString())];
        row.toolCalls += tool.calls;
        row.toolErrors += tool.errors;
        row.toolTokens += tool.consumedTokens();
        row.toolSavedTokens += tool.savedTokens;
        row.toolLatencyMsTotal += tool.latencyMsTotal;
        row.toolLatencySamples += tool.latencySamples;
        row.lastUsedMs = qMax(row.lastUsedMs, tool.lastCallMs);
    }
    return rows;
}

} // namespace AgentModelStats

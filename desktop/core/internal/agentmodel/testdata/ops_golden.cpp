#include "AgentLogText.h"
#include "LocalControlAgentOps.h"
#include "extracted.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ops = forkmesh::localcontrol::agentops;

namespace {

QString compact(const QJsonValue &value)
{
    const QByteArray doc = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(doc.mid(1, doc.size() - 2));
}

QStringList strings(const QJsonArray &array)
{
    QStringList out;
    for (const QJsonValue &value : array)
        out << value.toString();
    return out;
}

QJsonArray ints(const QList<int> &list)
{
    QJsonArray out;
    for (int value : list)
        out.append(value);
    return out;
}

QList<ops::UsageWindow> windows(const QJsonArray &rows)
{
    QList<ops::UsageWindow> out;
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        ops::UsageWindow window;
        window.id = row.value(QStringLiteral("id")).toString();
        window.label = row.value(QStringLiteral("label")).toString();
        window.usedPercent = row.value(QStringLiteral("usedPercent")).toInt(-1);
        window.resetAtMs = qint64(row.value(QStringLiteral("resetAtMs")).toDouble());
        window.exhausted = row.value(QStringLiteral("exhausted")).toBool();
        out.append(window);
    }
    return out;
}

QJsonValue run(const QString &fn, const QJsonObject &a)
{
    const auto s = [&a](const char *key) { return a.value(QLatin1String(key)).toString(); };
    const auto i = [&a](const char *key) { return a.value(QLatin1String(key)).toInt(); };
    const auto b = [&a](const char *key) { return a.value(QLatin1String(key)).toBool(); };
    if (fn == QLatin1String("approvalSummary"))
        return ops::approvalSummary(b("allow"), s("lifetime"), s("question"), s("title"));
    if (fn == QLatin1String("mergeSummary"))
        return ops::mergeSummary(s("branch"), s("base"), s("repo"));
    if (fn == QLatin1String("deleteSummary"))
        return ops::deleteSummary(s("branch"), s("repo"), b("running"));
    if (fn == QLatin1String("outsideDeleteSummary"))
        return ops::outsideDeleteSummary(s("label"), s("cwd"));
    if (fn == QLatin1String("bulkSummary"))
        return ops::bulkSummary(s("action"), i("count"));
    if (fn == QLatin1String("purgeSummary"))
        return ops::purgeSummary(i("count"));
    if (fn == QLatin1String("revokeSummary"))
        return ops::revokeSummary(s("capability"), s("resource"));
    if (fn == QLatin1String("numstatRows"))
        return ops::numstatRows(s("text"));
    if (fn == QLatin1String("capDiff")) {
        bool truncated = false;
        const QString text = ops::capDiff(s("text"), i("budget"), &truncated);
        return QJsonObject{{QStringLiteral("text"), text}, {QStringLiteral("truncated"), truncated}};
    }
    if (fn == QLatin1String("untrackedPaths"))
        return QJsonArray::fromStringList(ops::untrackedPaths(s("text")));
    if (fn == QLatin1String("diffImageMime"))
        return ops::diffImageMime(s("path"));
    if (fn == QLatin1String("validDiffImagePath"))
        return ops::validDiffImagePath(s("path"));
    if (fn == QLatin1String("worktrees")) {
        QJsonArray rows;
        const QList<ops::WorktreeEntry> entries = ops::parseWorktreePorcelain(s("text"));
        const QJsonObject sessions = a.value(QStringLiteral("sessions")).toObject();
        for (int index = 0; index < entries.size(); ++index) {
            if (entries.at(index).bare)
                continue;
            rows.append(ops::worktreeRow(entries.at(index), index == 0, i("ahead"), i("behind"),
                                         sessions.value(entries.at(index).branch).toInt()));
        }
        return rows;
    }
    if (fn == QLatin1String("parseLeftRight")) {
        int behind = 0;
        int ahead = 0;
        const bool ok = ops::parseLeftRight(s("text"), &behind, &ahead);
        return QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("behind"), behind},
                           {QStringLiteral("ahead"), ahead}};
    }
    if (fn == QLatin1String("mergeShortstat")) {
        QJsonArray out;
        for (const ops::MergeShortstat &merge : ops::parseMergeShortstatLog(s("log")))
            out.append(QJsonObject{{QStringLiteral("sha"), merge.sha},
                                   {QStringLiteral("subject"), merge.subject},
                                   {QStringLiteral("files"), merge.files},
                                   {QStringLiteral("insertions"), merge.insertions},
                                   {QStringLiteral("deletions"), merge.deletions}});
        return out;
    }
    if (fn == QLatin1String("mergedBranch"))
        return mergedBranchFromMergeSubject(s("subject"));
    if (fn == QLatin1String("mergeStats")) {
        QList<qint64> timestamps;
        for (const QJsonValue &value : a.value(QStringLiteral("timestamps")).toArray())
            timestamps.append(qint64(value.toDouble()));
        const MergeVelocity velocity =
            mergeVelocityForTimestamps(timestamps, qint64(a.value(QStringLiteral("now")).toDouble()));
        const auto window = [&velocity](int period, const QString &id, const QString &label, int count,
                                        const QVector<int> &series) {
            ops::MergeVelocityWindow row;
            row.id = id;
            row.label = label;
            row.count = count;
            row.top = period < velocity.periodTop.size() ? velocity.periodTop.at(period) : count;
            row.avg = period < velocity.periodAvg.size() ? velocity.periodAvg.at(period) : count;
            row.series = series;
            return row;
        };
        ops::MergeStats stats;
        stats.base = s("base");
        stats.commit = s("commit");
        stats.commits = a.value(QStringLiteral("commits")).toInt(-1);
        stats.files = i("files");
        stats.insertions = i("insertions");
        stats.deletions = i("deletions");
        stats.velocity = {
            window(0, QStringLiteral("1h"), QStringLiteral("1 hour"), velocity.lastHour, velocity.minutely),
            window(1, QStringLiteral("24h"), QStringLiteral("24 hours"), velocity.last24Hours, velocity.hourly),
            window(2, QStringLiteral("7d"), QStringLiteral("7 days"), velocity.last7Days, velocity.daily),
            window(3, QStringLiteral("30d"), QStringLiteral("30 days"), velocity.last30Days, velocity.monthly),
            window(4, QStringLiteral("52w"), QStringLiteral("52 weeks"), velocity.last52Weeks, velocity.weekly),
        };
        return ops::mergeStatsJson(stats);
    }
    if (fn == QLatin1String("binTitle"))
        return ops::binTitle(i("issueNumber"), s("issueTitle"), s("prompt"));
    if (fn == QLatin1String("binPayload")) {
        QList<ops::BinEntry> entries;
        for (const QJsonValue &value : a.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject row = value.toObject();
            ops::BinEntry entry;
            entry.id = row.value(QStringLiteral("id")).toInt();
            entry.title = row.value(QStringLiteral("title")).toString();
            entry.owner = row.value(QStringLiteral("owner")).toString();
            entry.name = row.value(QStringLiteral("name")).toString();
            entry.branch = row.value(QStringLiteral("branch")).toString();
            entry.provider = row.value(QStringLiteral("provider")).toString();
            entry.deletedAtMs = qint64(row.value(QStringLiteral("deletedAtMs")).toDouble());
            entries.append(entry);
        }
        return ops::binPayload(entries, i("retentionHours"), i("minHours"), i("maxHours"),
                               qint64(a.value(QStringLiteral("now")).toDouble()));
    }
    if (fn == QLatin1String("usage")) {
        QList<ops::UsageProvider> providers;
        for (const QJsonValue &value : a.value(QStringLiteral("providers")).toArray()) {
            const QJsonObject row = value.toObject();
            ops::UsageProvider provider;
            provider.id = row.value(QStringLiteral("id")).toString();
            provider.label = row.value(QStringLiteral("label")).toString();
            provider.windows = windows(row.value(QStringLiteral("windows")).toArray());
            provider.creditsEnabled = row.value(QStringLiteral("creditsEnabled")).toBool();
            provider.creditsUsed = row.value(QStringLiteral("creditsUsed")).toDouble(-1);
            provider.creditsLimit = row.value(QStringLiteral("creditsLimit")).toDouble(-1);
            provider.creditsCurrency = row.value(QStringLiteral("creditsCurrency")).toString();
            for (const QJsonValue &item : row.value(QStringLiteral("accounts")).toArray()) {
                const QJsonObject acc = item.toObject();
                provider.accounts.append({acc.value(QStringLiteral("id")).toString(),
                                          acc.value(QStringLiteral("label")).toString(),
                                          acc.value(QStringLiteral("active")).toBool(),
                                          acc.value(QStringLiteral("signedIn")).toBool(),
                                          acc.value(QStringLiteral("identity")).toString(),
                                          windows(acc.value(QStringLiteral("windows")).toArray())});
            }
            providers.append(provider);
        }
        QJsonObject summary = ops::usageSummary(providers, a.value(QStringLiteral("claudeSpendUsd")).toDouble(-1),
                                                a.value(QStringLiteral("openAiSpendUsd")).toDouble(-1),
                                                qint64(a.value(QStringLiteral("now")).toDouble()));
        summary.insert(QStringLiteral("activeProvider"), s("activeProvider"));
        return summary;
    }
    if (fn == QLatin1String("codexWindows")) {
        QJsonArray out;
        for (const ops::UsageWindow &window : ops::codexWindows(a.value(QStringLiteral("rows")).toArray()))
            out.append(QJsonObject{{QStringLiteral("id"), window.id},
                                   {QStringLiteral("label"), window.label},
                                   {QStringLiteral("usedPercent"), window.usedPercent},
                                   {QStringLiteral("resetAtMs"), double(window.resetAtMs)},
                                   {QStringLiteral("exhausted"), window.exhausted}});
        return out;
    }
    if (fn == QLatin1String("grantRows"))
        return ops::grantRows(a.value(QStringLiteral("grants")).toArray());
    if (fn == QLatin1String("canonicalIds")) {
        QList<int> out;
        const bool ok = ops::canonicalIds(a.value(QStringLiteral("ids")).toArray(), &out,
                                          a.value(QStringLiteral("max")).toInt(ops::kMaxBulkIds));
        return QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("ids"), ok ? ints(out) : QJsonArray()}};
    }
    if (fn == QLatin1String("bulkTarget")) {
        QList<int> list;
        for (const QJsonValue &value : a.value(QStringLiteral("ids")).toArray())
            list.append(value.toInt());
        return ops::bulkTarget(s("action"), list);
    }
    if (fn == QLatin1String("answerTarget"))
        return ops::answerTarget(s("id"), b("allow"), s("lifetime"));
    if (fn == QLatin1String("approvalId"))
        return ops::approvalId(i("session"), s("request"), QByteArray::fromHex(s("salt").toLatin1()));
    if (fn == QLatin1String("approvalRow")) {
        ops::ApprovalFacts facts;
        facts.id = s("id");
        facts.sessionId = i("sessionId");
        facts.title = s("title");
        facts.repo = s("repo");
        facts.provider = s("provider");
        facts.capability = ops::capabilityFromQuestion(s("question"));
        facts.question = s("question");
        facts.askedAtMs = qint64(a.value(QStringLiteral("askedAtMs")).toDouble());
        return ops::approvalRow(facts);
    }
    if (fn == QLatin1String("commandFromQuestion"))
        return ops::commandFromQuestion(s("question"));
    if (fn == QLatin1String("validGrantId"))
        return ops::validGrantId(s("id"));
    if (fn == QLatin1String("validApprovalId"))
        return ops::validApprovalId(s("id"));
    if (fn == QLatin1String("outsideEntry"))
        return AgentLogText::outsideEntry(s("kind"), a.value(QStringLiteral("entry")).toObject());
    return QJsonValue();
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3)
        return 2;
    QFile in(QString::fromLocal8Bit(argv[1]));
    if (!in.open(QIODevice::ReadOnly))
        return 3;
    const QJsonArray cases = QJsonDocument::fromJson(in.readAll()).array();
    QJsonArray out;
    for (const QJsonValue &value : cases) {
        const QJsonObject item = value.toObject();
        out.append(compact(run(item.value(QStringLiteral("fn")).toString(),
                               item.value(QStringLiteral("args")).toObject())));
    }
    QFile file(QString::fromLocal8Bit(argv[2]));
    if (!file.open(QIODevice::WriteOnly))
        return 4;
    file.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
    return 0;
}

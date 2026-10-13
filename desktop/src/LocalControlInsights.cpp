#include "LocalControlInsights.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHostAddress>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>

namespace forkmesh::localcontrol::insights {

namespace {

QJsonArray recentArray(const QVector<qint64> &values)
{
    QJsonArray out;
    for (qint64 value : values)
        out.append(double(value));
    return out;
}

}

QString familyPrefix()
{
    return QStringLiteral("insights.");
}

QJsonObject familyRow(const AgentToolFamilyStats &stats, const AgentToolFamily *family,
                      bool disabled, const QString &enforcement)
{
    return {{QStringLiteral("family"), stats.family},
            {QStringLiteral("name"), family ? family->displayName : stats.family},
            {QStringLiteral("category"), family ? family->category : QString()},
            {QStringLiteral("summary"), family ? family->summary.left(300) : QString()},
            {QStringLiteral("availability"), family ? family->availability : QString()},
            {QStringLiteral("calls"), double(stats.calls)},
            {QStringLiteral("errors"), double(stats.errors)},
            {QStringLiteral("inFlight"), double(stats.inFlight)},
            {QStringLiteral("averageLatencyMs"), stats.averageLatencyMs()},
            {QStringLiteral("maxLatencyMs"), double(stats.latencyMsMax)},
            {QStringLiteral("consumedTokens"), double(stats.consumedTokens())},
            {QStringLiteral("savedTokens"), double(stats.savedTokens)},
            {QStringLiteral("savingsRatio"), stats.savingsRatio()},
            {QStringLiteral("savedUsd"), stats.savedUsd},
            {QStringLiteral("consumedUsd"), stats.consumedUsd},
            {QStringLiteral("lastCallMs"), double(stats.lastCallMs)},
            {QStringLiteral("callsWhileOff"), double(stats.callsWhileOff)},
            {QStringLiteral("disabled"), disabled},
            {QStringLiteral("enforcement"), enforcement.left(300)},
            {QStringLiteral("recentSaved"), recentArray(stats.recentSaved)}};
}

QJsonObject totalsRow(const AgentToolLedgerTotals &totals)
{
    return {{QStringLiteral("calls"), double(totals.calls)},
            {QStringLiteral("errors"), double(totals.errors)},
            {QStringLiteral("inFlight"), double(totals.inFlight)},
            {QStringLiteral("consumedTokens"), double(totals.consumedTokens)},
            {QStringLiteral("savedTokens"), double(totals.savedTokens)},
            {QStringLiteral("savedUsd"), totals.savedUsd},
            {QStringLiteral("consumedUsd"), totals.consumedUsd},
            {QStringLiteral("familiesObserved"), totals.familiesObserved},
            {QStringLiteral("familiesTotal"), totals.familiesTotal},
            {QStringLiteral("firstCallMs"), double(totals.firstCallMs)},
            {QStringLiteral("lastCallMs"), double(totals.lastCallMs)},
            {QStringLiteral("recentCalls"), recentArray(totals.recentCalls)},
            {QStringLiteral("recentSaved"), recentArray(totals.recentSaved)}};
}

QJsonObject usageRow(const AgentToolUsageStats &usage)
{
    return {{QStringLiteral("tool"), usage.tool.left(120)},
            {QStringLiteral("family"), usage.family},
            {QStringLiteral("calls"), double(usage.calls)},
            {QStringLiteral("errors"), double(usage.errors)},
            {QStringLiteral("errorRate"), usage.errorRate()},
            {QStringLiteral("averageLatencyMs"), usage.averageLatencyMs()},
            {QStringLiteral("tokensPerCall"), usage.tokensPerCall()},
            {QStringLiteral("consumedTokens"), double(usage.consumedTokens())},
            {QStringLiteral("savedTokens"), double(usage.savedTokens)},
            {QStringLiteral("consumedUsd"), usage.consumedUsd},
            {QStringLiteral("lastCallMs"), double(usage.lastCallMs)}};
}

QJsonObject improvementRow(const AgentToolImprovement &improvement)
{
    return {{QStringLiteral("key"), improvement.key.left(200)},
            {QStringLiteral("kind"), improvement.kind},
            {QStringLiteral("family"), improvement.family},
            {QStringLiteral("tool"), improvement.runtimeName.left(120)},
            {QStringLiteral("topic"), improvement.topic.left(200)},
            {QStringLiteral("detail"), improvement.detail.left(400)},
            {QStringLiteral("suggestion"), improvement.suggestion.left(600)},
            {QStringLiteral("occurrences"), double(improvement.occurrences)},
            {QStringLiteral("tokensAtStake"), double(improvement.tokensAtStake)},
            {QStringLiteral("lastMs"), double(improvement.lastMs)}};
}

QJsonObject guardrailRow(const AgentGuardrailRepoStats &stats, int maxTools, int maxPaths)
{
    QList<QPair<QString, qint64>> tools;
    for (auto it = stats.tools.cbegin(); it != stats.tools.cend(); ++it)
        tools.append({it.key(), it.value()});
    std::sort(tools.begin(), tools.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QJsonArray toolRows;
    for (const auto &tool : tools.mid(0, maxTools))
        toolRows.append(QJsonObject{{QStringLiteral("tool"), tool.first.left(120)},
                                    {QStringLiteral("blocked"), double(tool.second)}});
    QJsonArray paths;
    for (const QString &path : stats.recentPaths.mid(0, maxPaths))
        paths.append(path.left(300));
    return {{QStringLiteral("repository"), stats.repository},
            {QStringLiteral("blocked"), double(stats.blocked)},
            {QStringLiteral("blockedOutside"), double(stats.blockedOutside)},
            {QStringLiteral("approved"), double(stats.approved)},
            {QStringLiteral("denied"), double(stats.denied)},
            {QStringLiteral("agentsStopped"), double(stats.agentsStopped)},
            {QStringLiteral("lastEventMs"), double(stats.lastEventMs)},
            {QStringLiteral("tools"), toolRows},
            {QStringLiteral("recentPaths"), paths}};
}

QJsonObject guardrailTotalsRow(const AgentGuardrailTotals &totals)
{
    return {{QStringLiteral("blocked"), double(totals.blocked)},
            {QStringLiteral("blockedOutside"), double(totals.blockedOutside)},
            {QStringLiteral("approved"), double(totals.approved)},
            {QStringLiteral("denied"), double(totals.denied)},
            {QStringLiteral("agentsStopped"), double(totals.agentsStopped)},
            {QStringLiteral("lastEventMs"), double(totals.lastEventMs)},
            {QStringLiteral("repositoriesWithEvents"), totals.repositoriesWithEvents}};
}

QJsonObject usageSummary(const QList<SessionUsage> &sessions, int maxSessions)
{
    struct Tally {
        QString provider;
        QString model;
        int sessions = 0;
        qint64 tokens = 0;
        double cost = 0;
        int turns = 0;
    };
    QMap<QString, Tally> byModel;
    double totalCost = 0;
    qint64 totalTokens = 0;
    for (const SessionUsage &session : sessions) {
        const QString key = session.provider + QLatin1Char('|') + session.model;
        Tally &tally = byModel[key];
        tally.provider = session.provider;
        tally.model = session.model;
        ++tally.sessions;
        tally.tokens += session.tokens;
        tally.cost += session.costUsd;
        tally.turns += session.turns;
        totalCost += session.costUsd;
        totalTokens += session.tokens;
    }
    QList<Tally> tallies = byModel.values();
    std::sort(tallies.begin(), tallies.end(), [](const Tally &a, const Tally &b) {
        return a.cost != b.cost ? a.cost > b.cost : a.tokens > b.tokens;
    });
    QJsonArray models;
    for (const Tally &tally : tallies.mid(0, 30))
        models.append(QJsonObject{{QStringLiteral("provider"), tally.provider},
                                  {QStringLiteral("model"), tally.model},
                                  {QStringLiteral("sessions"), tally.sessions},
                                  {QStringLiteral("tokens"), double(tally.tokens)},
                                  {QStringLiteral("costUsd"), tally.cost},
                                  {QStringLiteral("turns"), tally.turns}});
    QList<SessionUsage> sorted = sessions;
    std::sort(sorted.begin(), sorted.end(), [](const SessionUsage &a, const SessionUsage &b) {
        return a.costUsd != b.costUsd ? a.costUsd > b.costUsd : a.tokens > b.tokens;
    });
    QJsonArray top;
    for (const SessionUsage &session : sorted.mid(0, maxSessions))
        top.append(QJsonObject{{QStringLiteral("id"), session.id},
                               {QStringLiteral("title"), session.title.left(160)},
                               {QStringLiteral("provider"), session.provider},
                               {QStringLiteral("model"), session.model},
                               {QStringLiteral("status"), session.status},
                               {QStringLiteral("tokens"), double(session.tokens)},
                               {QStringLiteral("costUsd"), session.costUsd},
                               {QStringLiteral("turns"), session.turns},
                               {QStringLiteral("finishedAtMs"), double(session.finishedAtMs)},
                               {QStringLiteral("teammate"), session.teammateId}});
    return {{QStringLiteral("sessions"), int(sessions.size())},
            {QStringLiteral("tokens"), double(totalTokens)},
            {QStringLiteral("costUsd"), totalCost},
            {QStringLiteral("models"), models},
            {QStringLiteral("top"), top}};
}

bool isLoopbackPeer(const QString &peer)
{
    QHostAddress address;
    return address.setAddress(peer) && address.isLoopback();
}

QJsonArray networkRows(const QList<PeerTraffic> &traffic, const QHash<int, QString> &labels,
                       const QHash<QString, QString> &names, int maxPeersPerAgent)
{
    QMap<int, QMap<QString, PeerTraffic>> grouped;
    for (const PeerTraffic &item : traffic) {
        if (item.session == 0 || item.peer.isEmpty() || isLoopbackPeer(item.peer))
            continue;
        PeerTraffic &slot = grouped[item.session][item.peer];
        slot.session = item.session;
        slot.peer = item.peer;
        slot.port = slot.port ? slot.port : item.port;
        slot.sent += item.sent;
        slot.received += item.received;
    }
    QJsonArray rows;
    for (auto it = grouped.cbegin(); it != grouped.cend(); ++it) {
        QList<PeerTraffic> peers = it.value().values();
        std::sort(peers.begin(), peers.end(), [](const PeerTraffic &a, const PeerTraffic &b) {
            return a.sent + a.received > b.sent + b.received;
        });
        QJsonArray hosts;
        for (const PeerTraffic &peer : peers.mid(0, maxPeersPerAgent))
            hosts.append(QJsonObject{{QStringLiteral("peer"), peer.peer},
                                     {QStringLiteral("name"), names.value(peer.peer).left(200)},
                                     {QStringLiteral("port"), int(peer.port)},
                                     {QStringLiteral("sent"), double(peer.sent)},
                                     {QStringLiteral("received"), double(peer.received)}});
        rows.append(QJsonObject{{QStringLiteral("session"), it.key()},
                                {QStringLiteral("label"), labels.value(it.key()).left(160)},
                                {QStringLiteral("hosts"), hosts},
                                {QStringLiteral("hostCount"), int(peers.size())}});
    }
    return rows;
}

ShortcutMeta parseShortcutMeta(const QString &fileName, const QString &text)
{
    ShortcutMeta meta;
    meta.name = fileName;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < 20 && i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.startsWith(QLatin1String("#!")))
            continue;
        if (line.startsWith(QLatin1String("# name:")))
            meta.name = line.mid(7).trimmed();
        else if (line.startsWith(QLatin1String("# description:")))
            meta.description = line.mid(14).trimmed();
        else if (line.startsWith(QLatin1String("# agent:")))
            meta.agent = line.mid(8).trimmed().toLower();
        else if (line.startsWith(QLatin1String("# model:")))
            meta.model = line.mid(8).trimmed();
        else if (meta.description.isEmpty() && line.startsWith(QLatin1Char('#')))
            meta.description = line.mid(1).trimmed();
    }
    return meta;
}

QString shortcutPrompt(const QString &text)
{
    QStringList kept;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1String("# name:")) ||
            trimmed.startsWith(QLatin1String("# description:")) ||
            trimmed.startsWith(QLatin1String("# agent:")) ||
            trimmed.startsWith(QLatin1String("# model:")))
            continue;
        kept.append(line);
    }
    return kept.join(QLatin1Char('\n')).trimmed();
}

QString shortcutKind(const QString &fileName, bool executable)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("sh") || suffix == QLatin1String("bash") ||
        (suffix.isEmpty() && executable))
        return QStringLiteral("script");
    if (suffix == QLatin1String("skill"))
        return QStringLiteral("skill");
    if (suffix == QLatin1String("md") || suffix == QLatin1String("txt") ||
        suffix == QLatin1String("prompt"))
        return QStringLiteral("prompt");
    return QStringLiteral("file");
}

bool validShortcutFile(const QString &fileName)
{
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9._ -]{0,127}$"));
    return allowed.match(fileName).hasMatch() && !fileName.contains(QLatin1String(".."));
}

QStringList shortcutScopes()
{
    return {QStringLiteral("shortcut"), QStringLiteral("repository"), QStringLiteral("app"),
            QStringLiteral("organization")};
}

QString appendShortcutOutput(const QString &output, const QString &chunk, int maxChars)
{
    QString joined = output + chunk;
    if (maxChars <= 0 || joined.size() <= maxChars)
        return joined;
    joined = joined.right(maxChars);
    const qsizetype newline = joined.indexOf(QLatin1Char('\n'));
    return newline >= 0 && newline + 1 < joined.size() ? joined.mid(newline + 1) : joined;
}

QJsonObject shortcutRunRow(const ShortcutRun &run, bool withOutput)
{
    QJsonObject row{{QStringLiteral("id"), run.id},
                    {QStringLiteral("repo"), run.repo},
                    {QStringLiteral("file"), run.file},
                    {QStringLiteral("name"), run.name},
                    {QStringLiteral("running"), run.running},
                    {QStringLiteral("ok"), run.ok},
                    {QStringLiteral("message"), run.message},
                    {QStringLiteral("startedAtMs"), double(run.startedAtMs)},
                    {QStringLiteral("expectedMs"), double(run.expectedMs)},
                    {QStringLiteral("elapsedMs"),
                     double(run.running ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() -
                                                               run.startedAtMs)
                                        : -1)}};
    if (withOutput)
        row.insert(QStringLiteral("output"), run.output);
    return row;
}

QString lastOutputLine(const QString &output, int maxChars)
{
    static const QRegularExpression escapes(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]"));
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        QString line = it->section(QLatin1Char('\r'), -1);
        line.remove(escapes);
        line = line.trimmed();
        if (!line.isEmpty())
            return maxChars > 0 && line.size() > maxChars ? line.left(maxChars) : line;
    }
    return {};
}

QStringList mutatingActions()
{
    return {QStringLiteral("toggleFamily"), QStringLiteral("runShortcut")};
}

QString subjectFor(const QString &action, const QJsonObject &params, QString *error)
{
    const auto failWith = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return QString();
    };
    if (action == QLatin1String("toggleFamily")) {
        const QString family = params.value(QStringLiteral("family")).toString().trimmed();
        const QJsonValue enabled = params.value(QStringLiteral("enabled"));
        static const QRegularExpression id(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
        if (!id.match(family).hasMatch() || !enabled.isBool())
            return failWith("bad_family");
        return family + (enabled.toBool() ? QStringLiteral("|on") : QStringLiteral("|off"));
    }
    if (action == QLatin1String("runShortcut")) {
        const QString repo = params.value(QStringLiteral("repo")).toString().trimmed();
        const QString scope = params.value(QStringLiteral("scope")).toString().trimmed();
        const QString file = params.value(QStringLiteral("file")).toString();
        static const QRegularExpression repoKey(QStringLiteral(
            "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}/[A-Za-z0-9._][A-Za-z0-9._-]{0,99}$"));
        if (!shortcutScopes().contains(scope) || !validShortcutFile(file))
            return failWith("bad_shortcut");
        if (!repoKey.match(repo).hasMatch() || repo.contains(QLatin1String("..")))
            return failWith("bad_repo");
        return repo + QLatin1Char('|') + scope + QLatin1Char('|') + file;
    }
    return failWith("unsupported_action");
}

QString actionSummary(const QString &action, const QString &subject, const QString &label,
                      bool script)
{
    if (action == QLatin1String("toggleFamily")) {
        const bool on = subject.endsWith(QLatin1String("|on"));
        const QString name = label.isEmpty() ? subject.section(QLatin1Char('|'), 0, 0) : label;
        return on ? QStringLiteral("Switch the %1 tool family back on for agents. Running agents "
                                   "get it from their next launch or resume.")
                        .arg(name)
                  : QStringLiteral("Switch the %1 tool family off for agents. Running agents keep "
                                   "what they launched with until their next launch or resume.")
                        .arg(name);
    }
    if (action == QLatin1String("runShortcut") && script)
        return QStringLiteral("Run the \"%1\" script on the desktop in %2. Its output streams "
                              "here while it runs.")
            .arg(label.isEmpty() ? subject.section(QLatin1Char('|'), 2, 2) : label,
                 subject.section(QLatin1Char('|'), 0, 0));
    if (action == QLatin1String("runShortcut"))
        return QStringLiteral("Start an agent on the desktop with the \"%1\" shortcut in %2.")
            .arg(label.isEmpty() ? subject.section(QLatin1Char('|'), 2, 2) : label,
                 subject.section(QLatin1Char('|'), 0, 0));
    return {};
}

} // namespace forkmesh::localcontrol::insights

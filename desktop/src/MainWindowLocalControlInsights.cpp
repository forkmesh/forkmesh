// The "insights." local control family (WS17): agent insights: tool efficiency, guardrails, the announcer.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.
// Gate 2 answers "unsupported" for every method; WS17 fills it in.

#include "AgentGuardrailLedger.h"
#include "AgentNetworkSampler.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyLedger.h"
#include "AgentToolImprovementLog.h"
#include "AgentToolPolicy.h"
#include "AppPaths.h"
#include "LocalControl.h"
#include "LocalControlInsights.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrent>

using namespace forkmesh::ui;
namespace insightctl = forkmesh::localcontrol::insights;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

constexpr int kMaxUsageRows = 60;
constexpr int kMaxImprovements = 40;
constexpr int kMaxGuardrailRepos = 60;
constexpr int kMaxUsageSessions = 25;
constexpr int kMaxShortcuts = 200;
constexpr qint64 kMaxShortcutBytes = 64 * 1024;
constexpr int kMaxPeersPerAgent = 12;

QString readShortcutText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.read(kMaxShortcutBytes));
}

}

QString MainWindow::localControlShortcutDir(const QString &repoKey, const QString &scope,
                                            int *repoIndex) const
{
    if (repoIndex)
        *repoIndex = -1;
    if (scope == QLatin1String("app"))
        return QDir(forkmesh::paths::dataRoot()).filePath(QStringLiteral("skills"));
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if ((repo.owner + QLatin1Char('/') + repo.name).compare(repoKey, Qt::CaseInsensitive) != 0)
            continue;
        if (repo.previewOnly || repo.localPath.isEmpty())
            return {};
        if (repoIndex)
            *repoIndex = i;
        if (scope == QLatin1String("shortcut"))
            return repo.localPath + QStringLiteral("/.forkmesh/shortcuts");
        if (scope == QLatin1String("repository"))
            return QDir(repo.localPath).filePath(QStringLiteral(".forkmesh/skills"));
        if (scope == QLatin1String("organization")) {
            QString owner = repo.owner.trimmed();
            owner.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                          QStringLiteral("-"));
            if (owner.isEmpty() || owner == QLatin1String(".") || owner == QLatin1String(".."))
                owner = QStringLiteral("personal");
            return QDir(forkmesh::paths::dataRoot())
                .filePath(QStringLiteral("organization-skills/%1").arg(owner));
        }
        return {};
    }
    return {};
}

void MainWindow::answerLocalControlNetwork(int connectionId, int requestId)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    QList<forkmesh::agentnet::Root> roots;
    QHash<int, QString> labels;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        const qint64 pid = agentSessionProcessId(session.id);
        if (pid <= 0)
            continue;
        forkmesh::agentnet::Root root;
        root.sessionId = session.id;
        root.pid = pid;
        roots.append(root);
        QString title = session.issueTitle.trimmed();
        if (title.isEmpty())
            title = session.prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
        labels.insert(session.id, title.isEmpty()
                                      ? QStringLiteral("Agent run #%1").arg(session.id)
                                      : title.left(160));
    }
    if (roots.isEmpty()) {
        server->respond(connectionId, requestId, {{QStringLiteral("agents"), QJsonArray()}});
        return;
    }
    const QHash<QString, QString> names;
    QPointer<LocalControlServer> guard(server);
    auto *watcher = new QFutureWatcher<forkmesh::agentnet::Sample>(this);
    connect(watcher, &QFutureWatcher<forkmesh::agentnet::Sample>::finished, this,
            [watcher, guard, connectionId, requestId, labels, names] {
                const forkmesh::agentnet::Sample sample = watcher->result();
                watcher->deleteLater();
                if (!guard)
                    return;
                QList<insightctl::PeerTraffic> traffic;
                for (const forkmesh::agentnet::Socket &socket : sample.sockets) {
                    const int session = sample.owner.value(socket.inode, 0);
                    if (session == 0)
                        continue;
                    traffic.append({session, socket.peer, socket.port, qint64(socket.sent),
                                    qint64(socket.received)});
                }
                guard->respond(connectionId, requestId,
                               {{QStringLiteral("agents"),
                                 insightctl::networkRows(traffic, labels, names,
                                                         kMaxPeersPerAgent)},
                                {QStringLiteral("sampledAtMs"),
                                 double(QDateTime::currentMSecsSinceEpoch())}});
            });
    watcher->setFuture(QtConcurrent::run([roots] { return forkmesh::agentnet::sample(roots); }));
}

void MainWindow::handleLocalControlInsightsRequest(int connectionId, int requestId,
                                                   const QString &method,
                                                   const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);

    if (method == QLatin1String("insights.tools")) {
        const QStringList disabled = disabledAgentToolFamilies();
        QJsonArray families;
        QJsonArray usage;
        QJsonArray improvements;
        QJsonObject totals;
        if (m_agentToolLedger) {
            for (const AgentToolFamilyStats &stats : m_agentToolLedger->allStats()) {
                const AgentToolFamily *family = agentToolFamily(stats.family);
                if (family && family->hidden)
                    continue;
                families.append(insightctl::familyRow(stats, family, disabled.contains(stats.family),
                                                      AgentToolPolicy::enforcementNote(stats.family)));
            }
            QVector<AgentToolUsageStats> tools = m_agentToolLedger->usage();
            std::sort(tools.begin(), tools.end(),
                      [](const AgentToolUsageStats &a, const AgentToolUsageStats &b) {
                          return a.calls > b.calls;
                      });
            for (const AgentToolUsageStats &tool : tools.mid(0, kMaxUsageRows))
                usage.append(insightctl::usageRow(tool));
            totals = insightctl::totalsRow(m_agentToolLedger->totals());
        }
        if (m_agentToolImprovements) {
            QVector<AgentToolImprovement> signals_ = m_agentToolImprovements->improvements();
            std::sort(signals_.begin(), signals_.end(),
                      [](const AgentToolImprovement &a, const AgentToolImprovement &b) {
                          return a.lastMs > b.lastMs;
                      });
            for (const AgentToolImprovement &item : signals_.mid(0, kMaxImprovements))
                improvements.append(insightctl::improvementRow(item));
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("families"), families},
                         {QStringLiteral("totals"), totals},
                         {QStringLiteral("usage"), usage},
                         {QStringLiteral("improvements"), improvements},
                         {QStringLiteral("disabled"), QJsonArray::fromStringList(disabled)}});
        return;
    }
    if (method == QLatin1String("insights.guardrails")) {
        QJsonArray repositories;
        QJsonObject totals;
        if (m_agentGuardrailLedger) {
            for (const QString &repo : m_agentGuardrailLedger->repositories().mid(0, kMaxGuardrailRepos))
                repositories.append(
                    insightctl::guardrailRow(m_agentGuardrailLedger->stats(repo), 5, 5));
            totals = insightctl::guardrailTotalsRow(m_agentGuardrailLedger->totals());
        }
        QJsonArray policies;
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (repo.localPath.isEmpty() || policies.size() >= kMaxGuardrailRepos)
                continue;
            policies.append(QJsonObject{
                {QStringLiteral("repository"), repo.owner + QLatin1Char('/') + repo.name},
                {QStringLiteral("enabled"), repo.agentGuardrailsEnabled},
                {QStringLiteral("allowedDirectories"),
                 QJsonArray::fromStringList(repo.agentAllowedDirectories.mid(0, 30))}});
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("totals"), totals},
                         {QStringLiteral("repositories"), repositories},
                         {QStringLiteral("policies"), policies}});
        return;
    }
    if (method == QLatin1String("insights.usage")) {
        QList<insightctl::SessionUsage> sessions;
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            insightctl::SessionUsage row;
            row.id = session.id;
            row.title = session.issueTitle.trimmed().isEmpty()
                            ? session.prompt.section(QLatin1Char('\n'), 0, 0).trimmed()
                            : session.issueTitle.trimmed();
            row.provider = session.provider;
            row.model = session.model;
            row.status = session.status;
            row.tokens = session.totalTokens;
            row.costUsd = session.costUsd;
            row.turns = session.numTurns;
            row.finishedAtMs = session.finishedAtMs;
            row.teammateId = session.teammateId;
            sessions.append(row);
        }
        server->respond(connectionId, requestId,
                        insightctl::usageSummary(sessions, kMaxUsageSessions));
        return;
    }
    if (method == QLatin1String("insights.network")) {
        answerLocalControlNetwork(connectionId, requestId);
        return;
    }
    if (method == QLatin1String("insights.shortcuts")) {
        const QString repo = params.value(QStringLiteral("repo")).toString().trimmed();
        QJsonArray entries;
        QJsonArray repos;
        for (const RepositoryRecord &record : std::as_const(m_repositories))
            if (!record.localPath.isEmpty() && !record.previewOnly && repos.size() < 300)
                repos.append(record.owner + QLatin1Char('/') + record.name);
        for (const QString &scope : insightctl::shortcutScopes()) {
            int repoIndex = -1;
            const QString dir = localControlShortcutDir(repo, scope, &repoIndex);
            if (dir.isEmpty())
                continue;
            const bool skill = scope != QLatin1String("shortcut");
            const QStringList filters =
                skill ? QStringList{QStringLiteral("*.skill"), QStringLiteral("*.md"),
                                    QStringLiteral("*.prompt"), QStringLiteral("*.txt")}
                      : QStringList();
            const QFileInfoList files =
                QDir(dir).entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Name);
            for (const QFileInfo &info : files) {
                if (entries.size() >= kMaxShortcuts)
                    break;
                if (!insightctl::validShortcutFile(info.fileName()))
                    continue;
                const QString text = readShortcutText(info.absoluteFilePath());
                const insightctl::ShortcutMeta meta =
                    insightctl::parseShortcutMeta(info.fileName(), text);
                const QString kind = skill ? QStringLiteral("skill")
                                           : insightctl::shortcutKind(info.fileName(),
                                                                      info.isExecutable());
                const QString repoKey =
                    repoIndex >= 0 && repoIndex < m_repositories.size()
                        ? m_repositories.at(repoIndex).owner + QLatin1Char('/') +
                              m_repositories.at(repoIndex).name
                        : repo;
                const QString usageKey =
                    scope == QLatin1String("app") || scope == QLatin1String("organization")
                        ? scope + QLatin1Char(':') + info.fileName()
                        : scope + QLatin1Char(':') + repoKey + QLatin1Char(':') + info.fileName();
                const ShortcutRunStats stats = shortcutRunStats(usageKey);
                const insightctl::ShortcutRun *live = nullptr;
                for (const auto &run : m_shortcutRuns)
                    if (!skill && run.running && run.file == info.fileName() &&
                        run.repo.compare(repoKey, Qt::CaseInsensitive) == 0)
                        live = &run;
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                qint64 elapsedMs = live ? nowMs - live->startedAtMs : -1;
                const qint64 expectedMs = live ? live->expectedMs : stats.lastMs;
                for (auto it = m_shortcutAgentRuns.cbegin();
                     !live && elapsedMs < 0 && it != m_shortcutAgentRuns.cend(); ++it) {
                    const AgentSession *session =
                        it.value() == usageKey ? findAgentSession(it.key()) : nullptr;
                    if (session &&
                        (session->runInFlight() || session->status == AgentStatus::Queued))
                        elapsedMs = session->totalRunMs(nowMs);
                }
                const bool running = live != nullptr || elapsedMs >= 0;
                entries.append(QJsonObject{
                    {QStringLiteral("file"), info.fileName()},
                    {QStringLiteral("scope"), scope},
                    {QStringLiteral("name"),
                     skill && meta.name == info.fileName() ? info.completeBaseName() : meta.name},
                    {QStringLiteral("description"), meta.description.left(300)},
                    {QStringLiteral("kind"), kind},
                    {QStringLiteral("agent"), meta.agent},
                    {QStringLiteral("model"), meta.model},
                    {QStringLiteral("uses"), shortcutUsageCount(usageKey)},
                    {QStringLiteral("runnable"),
                     kind == QLatin1String("script")
                         ? scope == QLatin1String("shortcut")
                         : kind != QLatin1String("file") &&
                               !insightctl::shortcutPrompt(text).isEmpty()},
                    {QStringLiteral("running"), running},
                    {QStringLiteral("queued"),
                     skill ? 0 : shortcutQueuePosition(info.absoluteFilePath())},
                    {QStringLiteral("lastOutput"),
                     running ? insightctl::lastOutputLine(live->output) : stats.lastOutput},
                    {QStringLiteral("lastOk"), stats.lastOk},
                    {QStringLiteral("finished"), stats.finished},
                    {QStringLiteral("lastRunAtMs"), double(stats.lastRunAtMs)},
                    {QStringLiteral("lastMs"), double(stats.lastMs)},
                    {QStringLiteral("elapsedMs"), double(running ? qMax<qint64>(0, elapsedMs) : -1)},
                    {QStringLiteral("expectedMs"), double(expectedMs)}});
            }
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("repo"), repo},
                         {QStringLiteral("repos"), repos},
                         {QStringLiteral("entries"), entries}});
        return;
    }
    if (method == QLatin1String("insights.shortcut")) {
        const QString repo = params.value(QStringLiteral("repo")).toString().trimmed();
        const QString scope = params.value(QStringLiteral("scope")).toString().trimmed();
        const QString file = params.value(QStringLiteral("file")).toString();
        const QString dir = insightctl::shortcutScopes().contains(scope) &&
                                    insightctl::validShortcutFile(file)
                                ? localControlShortcutDir(repo, scope, nullptr)
                                : QString();
        const QString path = dir.isEmpty() ? QString() : QDir(dir).filePath(file);
        if (path.isEmpty() || !QFileInfo(path).isFile()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_shortcut"));
            return;
        }
        const QString text = readShortcutText(path);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("file"), file},
                         {QStringLiteral("scope"), scope},
                         {QStringLiteral("text"), text},
                         {QStringLiteral("truncated"), QFileInfo(path).size() > kMaxShortcutBytes}});
        return;
    }
    if (method == QLatin1String("insights.shortcutRun")) {
        QJsonObject answer;
        const int wanted = params.value(QStringLiteral("id")).toInt();
        const insightctl::ShortcutRun *shown = nullptr;
        QJsonArray runs;
        for (const auto &run : m_shortcutRuns) {
            runs.append(insightctl::shortcutRunRow(run, false));
            if (wanted <= 0 || run.id == wanted)
                shown = &run;
        }
        if (shown)
            answer.insert(QStringLiteral("run"), insightctl::shortcutRunRow(*shown, true));
        answer.insert(QStringLiteral("runs"), runs);
        server->respond(connectionId, requestId, answer);
        return;
    }
    if (method == QLatin1String("insights.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = insightctl::subjectFor(action, params, &error);
        if (subject.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        QString label;
        bool script = false;
        if (action == QLatin1String("toggleFamily")) {
            const AgentToolFamily *family =
                agentToolFamily(subject.section(QLatin1Char('|'), 0, 0));
            if (!family) {
                server->respondError(connectionId, requestId, QStringLiteral("bad_family"));
                return;
            }
            label = family->displayName;
        } else {
            label = params.value(QStringLiteral("file")).toString();
            const QString scope = subject.section(QLatin1Char('|'), 1, 1);
            const QString dir =
                localControlShortcutDir(subject.section(QLatin1Char('|'), 0, 0), scope, nullptr);
            const QFileInfo info(dir.isEmpty() ? QString() : QDir(dir).filePath(label));
            script = scope == QLatin1String("shortcut") && info.isFile() &&
                     insightctl::shortcutKind(info.fileName(), info.isExecutable()) ==
                         QLatin1String("script");
        }
        const QString token = m_insightsControlConfirmations.issue(
            device.key, action, subject, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("summary"),
                          insightctl::actionSummary(action, subject, label, script)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("insights.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = insightctl::subjectFor(action, params, &error);
        const hostctl::ConfirmationStore::Result verdict = m_insightsControlConfirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, action, subject,
            QDateTime::currentMSecsSinceEpoch());
        if (subject.isEmpty() || verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = subject.isEmpty() ? error : hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused an insights %1 from %2: %3.")
                          .arg(action, localControlDeviceLabel(connectionId), why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        runLocalControlInsightsAction(connectionId, requestId, action, subject);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::runLocalControlInsightsAction(int connectionId, int requestId,
                                               const QString &action, const QString &subject)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString who = localControlDeviceLabel(connectionId);
    if (action == QLatin1String("toggleFamily")) {
        const QString family = subject.section(QLatin1Char('|'), 0, 0);
        const bool enabled = subject.endsWith(QLatin1String("|on"));
        if (!agentToolFamily(family)) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_family"));
            return;
        }
        setAgentToolFamilyEnabled(family, enabled);
        logSystem(QStringLiteral("Tool family %1 switched %2 by %3.")
                      .arg(family, enabled ? QStringLiteral("on") : QStringLiteral("off"), who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("message"),
                          enabled ? QStringLiteral("Switched on.") : QStringLiteral("Switched off.")}});
        server->broadcast(QStringLiteral("insights.changed"));
        return;
    }
    if (action == QLatin1String("queueShortcut") || action == QLatin1String("unqueueShortcut")) {
        const QString repo = subject.section(QLatin1Char('|'), 0, 0);
        const QString scope = subject.section(QLatin1Char('|'), 1, 1);
        const QString file = subject.section(QLatin1Char('|'), 2);
        int repoIndex = -1;
        const QString dir = localControlShortcutDir(repo, scope, &repoIndex);
        const QFileInfo info(dir.isEmpty() ? QString() : QDir(dir).filePath(file));
        if (repoIndex < 0 || !info.isFile()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_shortcut"));
            return;
        }
        if (scope != QLatin1String("shortcut") ||
            insightctl::shortcutKind(info.fileName(), info.isExecutable()) !=
                QLatin1String("script")) {
            server->respondError(connectionId, requestId, QStringLiteral("not_a_script"));
            return;
        }
        const QString name =
            insightctl::parseShortcutMeta(file, readShortcutText(info.absoluteFilePath())).name;
        if (action == QLatin1String("unqueueShortcut")) {
            const bool removed = unqueueShortcut(info.absoluteFilePath());
            logSystem(QStringLiteral("Shortcut %1 in %2 taken out of the queue by %3.")
                          .arg(file, repo, who));
            server->respond(connectionId, requestId,
                            {{QStringLiteral("started"), false},
                             {QStringLiteral("action"), action},
                             {QStringLiteral("subject"), subject},
                             {QStringLiteral("message"),
                              (removed ? QStringLiteral("Took %1 out of the queue.")
                                       : QStringLiteral("%1 was not queued."))
                                  .arg(name)}});
            return;
        }
        const int position = queueShortcut(info.absoluteFilePath(), repoIndex);
        if (position < 0) {
            server->respondError(connectionId, requestId, QStringLiteral("shortcut_not_started"));
            return;
        }
        logSystem(QStringLiteral("Shortcut %1 in %2 %3 on the desktop for %4.")
                      .arg(file, repo,
                           position > 0 ? QStringLiteral("queued") : QStringLiteral("started"),
                           who));
        QJsonObject answer{{QStringLiteral("started"), position == 0},
                           {QStringLiteral("queued"), position},
                           {QStringLiteral("action"), action},
                           {QStringLiteral("subject"), subject},
                           {QStringLiteral("message"),
                            position > 0
                                ? QStringLiteral("Queued %1: number %2 in line on the desktop.")
                                      .arg(name)
                                      .arg(position)
                                : QStringLiteral("Nothing was running, so %1 started on the "
                                                 "desktop.")
                                      .arg(name)}};
        if (position == 0)
            answer.insert(QStringLiteral("run"), m_shortcutRunCounter);
        server->respond(connectionId, requestId, answer);
        server->broadcast(QStringLiteral("insights.changed"));
        return;
    }
    if (action == QLatin1String("runShortcut")) {
        const QString repo = subject.section(QLatin1Char('|'), 0, 0);
        const QString scope = subject.section(QLatin1Char('|'), 1, 1);
        const QString file = subject.section(QLatin1Char('|'), 2);
        int repoIndex = -1;
        const QString dir = localControlShortcutDir(repo, scope, &repoIndex);
        const QString path = dir.isEmpty() ? QString() : QDir(dir).filePath(file);
        if (repoIndex < 0 || path.isEmpty() || !QFileInfo(path).isFile()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_shortcut"));
            return;
        }
        const QFileInfo info(path);
        if (scope == QLatin1String("shortcut") &&
            insightctl::shortcutKind(info.fileName(), info.isExecutable()) ==
                QLatin1String("script")) {
            const bool restart = shortcutRunning(path);
            if (!runShortcut(path, repoIndex)) {
                server->respondError(connectionId, requestId, QStringLiteral("shortcut_not_started"));
                return;
            }
            logSystem(QStringLiteral("Shortcut %1 in %2 %3 on the desktop for %4.")
                          .arg(file, repo,
                               restart ? QStringLiteral("restarted") : QStringLiteral("started"),
                               who));
            server->respond(connectionId, requestId,
                            {{QStringLiteral("started"), true},
                             {QStringLiteral("action"), action},
                             {QStringLiteral("subject"), subject},
                             {QStringLiteral("run"), m_shortcutRunCounter + (restart ? 1 : 0)},
                             {QStringLiteral("message"),
                              (restart ? QStringLiteral("Stopping %1 to run it again on the desktop.")
                                       : QStringLiteral("Running %1 on the desktop."))
                                  .arg(insightctl::parseShortcutMeta(file, readShortcutText(path)).name)}});
            server->broadcast(QStringLiteral("insights.changed"));
            return;
        }
        const QString text = readShortcutText(path);
        const insightctl::ShortcutMeta meta = insightctl::parseShortcutMeta(file, text);
        QString prompt = insightctl::shortcutPrompt(text);
        if (prompt.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("empty_shortcut"));
            return;
        }
        prompt.replace(QStringLiteral("{{appLog}}"), QDir::toNativeSeparators(networkLogPath()));
        const QString stallLog = stallLogPath();
        prompt.replace(QStringLiteral("{{stallLog}}"),
                       stallLog.isEmpty() ? QStringLiteral("(disabled in this build)")
                                          : QDir::toNativeSeparators(stallLog));
        QString provider = meta.agent.isEmpty() ? defaultAgentProvider() : meta.agent;
        if (provider == QLatin1String("claude"))
            provider = QStringLiteral("claude-code");
        if (!meta.agent.isEmpty() && !agentIsCodexProvider(provider) &&
            provider != QLatin1String("claude-code")) {
            server->respondError(connectionId, requestId, QStringLiteral("unsupported_agent"));
            return;
        }
        if (!meta.model.isEmpty() && !agentModelMatchesProvider(provider, meta.model)) {
            server->respondError(connectionId, requestId, QStringLiteral("model_mismatch"));
            return;
        }
        const int sessionId = startAdHocAgentForRepo(repoIndex, prompt, provider, false,
                                                     meta.model, meta.name, false, false);
        if (sessionId <= 0) {
            server->respondError(connectionId, requestId, QStringLiteral("agent_not_started"));
            return;
        }
        if (scope != QLatin1String("shortcut")) {
            if (AgentSession *session = findAgentSession(sessionId)) {
                session->skillsUsed = {meta.name};
                if (m_agentStore)
                    m_agentStore->saveSession(*session);
            }
        }
        logSystem(QStringLiteral("Shortcut %1 in %2 started agent #%3 for %4.")
                      .arg(file, repo)
                      .arg(sessionId)
                      .arg(who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("session"), sessionId},
                         {QStringLiteral("message"),
                          QStringLiteral("Started agent #%1 for %2.").arg(sessionId).arg(meta.name)}});
        server->broadcast(QStringLiteral("insights.changed"));
        server->broadcast(QStringLiteral("agents.changed"));
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
}

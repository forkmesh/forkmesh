// The "agentops." local control family (WS10): approvals, merge & clean up, bulk actions and usage for the owner's agents.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.

#include "AgentCapabilityBroker.h"
#include "AgentCapabilityStore.h"
#include "AgentPermissionController.h"
#include "ClaudePeerMessaging.h"
#include "CoreAgentModel.h"
#include "ClaudeSessionScan.h"
#include "LocalControl.h"
#include "LocalControlAgentOps.h"
#include "LocalControlHosts.h"
#include "LocalControlTerminals.h"
#include "MachineAgentScan.h"
#include "MainWindow.h"
#include "MainWindowAgentsInternal.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

using namespace forkmesh::ui;
namespace agentops = forkmesh::localcontrol::agentops;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

const QString kAgentApprovalSaltSetting = QStringLiteral("localControl/agentOpsApprovalSalt");
const QString kAgentApprovalPingsSetting = QStringLiteral("localControl/agentApprovalPings");
constexpr int kDeleteStopWaitMs = 30000;
constexpr int kDeleteStopPollMs = 250;
constexpr int kDeleteAnswerWaitMs = 75000;

struct OutsideAgent {
    QString terminal;
    QList<qint64> pids;
    QString uuid;
    QString label;
    QString cwd;
    bool otherUser = false;
};

class AgentOpsState : public QObject
{
public:
    explicit AgentOpsState(QObject *parent)
        : QObject(parent),
          pings(QSettings().value(kAgentApprovalPingsSetting).toStringList())
    {
        setObjectName(QStringLiteral("forkmeshAgentOpsState"));
        QSettings settings;
        salt = agentops::approvalSaltFromStored(
            settings.value(kAgentApprovalSaltSetting).toString());
        if (salt.isEmpty()) {
            salt = agentops::newApprovalSalt();
            settings.setValue(kAgentApprovalSaltSetting, QString::fromLatin1(salt.toHex()));
        }
    }

    void savePings() const
    {
        QSettings().setValue(kAgentApprovalPingsSetting, pings.ids());
    }

    QByteArray salt;
    agentops::ApprovalPingLedger pings;
    hostctl::ConfirmationStore confirmations;
    QString lastSignature;
    bool bellArmed = false;
    QSet<int> mergesInFlight;
};

AgentOpsState *agentOpsState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(QStringLiteral("forkmeshAgentOpsState"),
                                                  Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<AgentOpsState *>(found))
        return state;
    return new AgentOpsState(window);
}

bool agentOpsSessionActive(const AgentSession &session)
{
    return session.status == AgentStatus::Queued ||
           session.status == AgentStatus::Running ||
           session.status == AgentStatus::Waiting;
}

QString agentOpsSessionTitle(const AgentSession &session)
{
    if (!session.issueTitle.trimmed().isEmpty())
        return session.issueTitle.trimmed();
    const QString prompt = session.prompt.trimmed();
    const int newline = prompt.indexOf(QLatin1Char('\n'));
    return (newline > 0 ? prompt.left(newline) : prompt).left(200);
}

QString confirmationErrorCode(hostctl::ConfirmationStore::Result result)
{
    switch (result) {
    case hostctl::ConfirmationStore::Result::Ok:
        return {};
    case hostctl::ConfirmationStore::Result::Expired:
        return QStringLiteral("confirmation_expired");
    case hostctl::ConfirmationStore::Result::Mismatch:
        return QStringLiteral("confirmation_mismatch");
    case hostctl::ConfirmationStore::Result::Missing:
        break;
    }
    return QStringLiteral("confirmation_required");
}

struct GitRun {
    bool ok = false;
    int exitCode = -1;
    QString out;
};

GitRun agentOpsGit(const QString &dir, const QStringList &args, int timeoutMs = 15000)
{
    GitRun run;
    QProcess git;
    git.setWorkingDirectory(dir);
    git.start(QStringLiteral("git"), args);
    if (!git.waitForFinished(timeoutMs)) {
        git.kill();
        git.waitForFinished(1000);
        return run;
    }
    run.exitCode = git.exitStatus() == QProcess::NormalExit ? git.exitCode() : -1;
    run.ok = run.exitCode == 0;
    run.out = QString::fromUtf8(git.readAllStandardOutput());
    return run;
}

struct WorktreeScan {
    QString base;
    QJsonArray rows;
};

struct MergeCheck {
    QString error;
    QString into;
};

struct MergeStatsRead {
    QString branch;
    agentops::MergeStats stats;
};

agentops::MergeStats readLandedMergeStats(const QString &localPath, const QString &into,
                                          const QString &branch, bool newest = false,
                                          QString *landedBranch = nullptr)
{
    agentops::MergeStats stats;
    stats.base = into;
    if (localPath.isEmpty() || into.isEmpty())
        return stats;
    const GitRun log = agentOpsGit(
        localPath, {QStringLiteral("log"), QStringLiteral("--merges"),
                    QStringLiteral("--first-parent"), QStringLiteral("--diff-merges=first-parent"),
                    QStringLiteral("--shortstat"), QStringLiteral("-n"), QStringLiteral("20"),
                    QStringLiteral("--pretty=format:%x1e%H%x1f%s"), into});
    if (log.ok) {
        for (const agentops::MergeShortstat &merge : agentops::parseMergeShortstatLog(log.out)) {
            const QString merged = mergedBranchFromMergeSubject(merge.subject);
            if (!newest && merged != branch)
                continue;
            if (landedBranch)
                *landedBranch = merged;
            stats.commit = merge.sha;
            stats.files = merge.files;
            stats.insertions = merge.insertions;
            stats.deletions = merge.deletions;
            const GitRun count = agentOpsGit(
                localPath, {QStringLiteral("rev-list"), QStringLiteral("--count"),
                            QStringLiteral("%1^1..%1").arg(merge.sha)});
            if (count.ok)
                stats.commits = count.out.trimmed().toInt();
            break;
        }
    }
    const GitRun times = agentOpsGit(
        localPath, {QStringLiteral("log"), QStringLiteral("--merges"),
                    QStringLiteral("--first-parent"), QStringLiteral("--pretty=format:%ct"), into});
    if (!times.ok)
        return stats;
    QList<qint64> timestamps;
    for (const QString &line : times.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 timestamp = line.trimmed().toLongLong(&ok);
        if (ok)
            timestamps.append(timestamp);
    }
    const MergeVelocity velocity =
        mergeVelocityForTimestamps(timestamps, QDateTime::currentSecsSinceEpoch());
    const auto window = [&velocity](int period, const QString &id, const QString &label,
                                     int count, const QVector<int> &series) {
        agentops::MergeVelocityWindow row;
        row.id = id;
        row.label = label;
        row.count = count;
        row.top = period < velocity.periodTop.size() ? velocity.periodTop.at(period) : count;
        row.avg = period < velocity.periodAvg.size() ? velocity.periodAvg.at(period) : count;
        row.series = series;
        return row;
    };
    stats.velocity = {
        window(0, QStringLiteral("1h"), QStringLiteral("1 hour"), velocity.lastHour,
               velocity.minutely),
        window(1, QStringLiteral("24h"), QStringLiteral("24 hours"), velocity.last24Hours,
               velocity.hourly),
        window(2, QStringLiteral("7d"), QStringLiteral("7 days"), velocity.last7Days,
               velocity.daily),
        window(3, QStringLiteral("30d"), QStringLiteral("30 days"), velocity.last30Days,
               velocity.monthly),
        window(4, QStringLiteral("52w"), QStringLiteral("52 weeks"), velocity.last52Weeks,
               velocity.weekly),
    };
    return stats;
}

struct DiffRead {
    QString error;
    QString base;
    QString committed;
    QString uncommitted;
    QJsonArray files;
    QStringList untracked;
    QString worktree;
    bool truncated = false;
};

QString agentDiffBase(const QString &localPath, const QString &base)
{
    if (agentOpsGit(localPath, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                QStringLiteral("--quiet"), base})
            .ok)
        return base;
    return agentOpsGit(localPath, {QStringLiteral("symbolic-ref"), QStringLiteral("--short"),
                                   QStringLiteral("HEAD")})
        .out.trimmed();
}

QString agentBranchWorktree(const QString &localPath, const QString &branch)
{
    const GitRun list = agentOpsGit(localPath, {QStringLiteral("worktree"), QStringLiteral("list"),
                                                QStringLiteral("--porcelain")});
    if (!list.ok)
        return {};
    const QList<agentops::WorktreeEntry> entries = agentops::parseWorktreePorcelain(list.out);
    for (int i = 1; i < entries.size(); ++i) {
        if (entries.at(i).branch == branch && !entries.at(i).path.isEmpty())
            return entries.at(i).path;
    }
    return {};
}

DiffRead readAgentDiff(const QString &localPath, const QString &base, const QString &branch)
{
    DiffRead read;
    const QString into = agentDiffBase(localPath, base);
    read.base = into;
    const bool branchKnown =
        agentOpsGit(localPath, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                QStringLiteral("--quiet"),
                                QStringLiteral("refs/heads/") + branch})
            .ok;
    if (!into.isEmpty() && branchKnown) {
        const QString range = into + QStringLiteral("...") + branch;
        const GitRun numstat = agentOpsGit(
            localPath, {QStringLiteral("diff"), QStringLiteral("--numstat"), range}, 20000);
        if (numstat.ok)
            read.files = agentops::numstatRows(numstat.out);
        const GitRun patch = agentOpsGit(
            localPath, {QStringLiteral("diff"), QStringLiteral("--no-color"),
                        QStringLiteral("--no-ext-diff"), range},
            30000);
        if (patch.ok)
            read.committed =
                agentops::capDiff(patch.out, agentops::kMaxDiffChars, &read.truncated);
    }
    read.worktree = agentBranchWorktree(localPath, branch);
    if (!read.worktree.isEmpty()) {
        const GitRun pending = agentOpsGit(
            read.worktree, {QStringLiteral("diff"), QStringLiteral("--no-color"),
                            QStringLiteral("--no-ext-diff"), QStringLiteral("HEAD")},
            30000);
        if (pending.ok)
            read.uncommitted = agentops::capDiff(
                pending.out, agentops::kMaxDiffChars - read.committed.size(), &read.truncated);
        const GitRun status = agentOpsGit(
            read.worktree, {QStringLiteral("status"), QStringLiteral("--porcelain"),
                            QStringLiteral("--untracked-files=all")});
        if (status.ok)
            read.untracked = agentops::untrackedPaths(status.out);
    }
    if (!branchKnown && read.worktree.isEmpty())
        read.error = QStringLiteral("branch_gone");
    return read;
}

struct ImageRead {
    QString error;
    QByteArray data;
    qint64 size = 0;
};

ImageRead readGitImage(const QString &dir, const QString &spec)
{
    ImageRead read;
    const GitRun size = agentOpsGit(dir, {QStringLiteral("cat-file"), QStringLiteral("-s"), spec});
    if (!size.ok) {
        read.error = QStringLiteral("file_missing");
        return read;
    }
    read.size = size.out.trimmed().toLongLong();
    if (read.size > agentops::kMaxDiffImageBytes) {
        read.error = QStringLiteral("too_large");
        return read;
    }
    QProcess git;
    git.setWorkingDirectory(dir);
    git.start(QStringLiteral("git"), {QStringLiteral("cat-file"), QStringLiteral("blob"), spec});
    if (!git.waitForFinished(20000)) {
        git.kill();
        git.waitForFinished(1000);
        read.error = QStringLiteral("file_missing");
        return read;
    }
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0) {
        read.error = QStringLiteral("file_missing");
        return read;
    }
    read.data = git.readAllStandardOutput();
    return read;
}

ImageRead readWorktreeImage(const QString &worktree, const QString &path)
{
    ImageRead read;
    const QString root = QFileInfo(worktree).canonicalFilePath();
    const QFileInfo info(QDir(worktree).filePath(path));
    const QString target = info.canonicalFilePath();
    if (root.isEmpty() || target.isEmpty() || !info.isFile() ||
        !target.startsWith(root + QLatin1Char('/'))) {
        read.error = QStringLiteral("file_missing");
        return read;
    }
    read.size = info.size();
    if (read.size > agentops::kMaxDiffImageBytes) {
        read.error = QStringLiteral("too_large");
        return read;
    }
    QFile file(target);
    if (!file.open(QIODevice::ReadOnly)) {
        read.error = QStringLiteral("file_missing");
        return read;
    }
    read.data = file.read(agentops::kMaxDiffImageBytes + 1);
    if (read.data.size() > agentops::kMaxDiffImageBytes) {
        read.data.clear();
        read.error = QStringLiteral("too_large");
    }
    return read;
}

ImageRead readAgentDiffImage(const QString &localPath, const QString &base, const QString &branch,
                             const QString &path, bool uncommitted, bool after)
{
    const QString head = QStringLiteral("refs/heads/") + branch;
    if (!uncommitted) {
        if (after)
            return readGitImage(localPath, head + QLatin1Char(':') + path);
        const GitRun fork = agentOpsGit(
            localPath, {QStringLiteral("merge-base"), agentDiffBase(localPath, base), head});
        if (!fork.ok || fork.out.trimmed().isEmpty())
            return ImageRead{QStringLiteral("file_missing"), {}, 0};
        return readGitImage(localPath, fork.out.trimmed() + QLatin1Char(':') + path);
    }
    const QString worktree = agentBranchWorktree(localPath, branch);
    if (worktree.isEmpty())
        return ImageRead{QStringLiteral("file_missing"), {}, 0};
    return after ? readWorktreeImage(worktree, path)
                 : readGitImage(worktree, QStringLiteral("HEAD:") + path);
}

}

QJsonArray MainWindow::localControlAgentOpsApprovals(
    const QByteArray &salt, QHash<QString, QPair<int, QString>> *index)
{
    QJsonArray rows;
    if (index)
        index->clear();
    if (!m_agentPermissionController || m_agentPermissionController->pendingCount() == 0)
        return rows;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (rows.size() >= agentops::kMaxApprovals)
            break;
        if (session.cloud || isExternalSession(session.id) || session.deletedAtMs > 0 ||
            (session.status != AgentStatus::Running && session.status != AgentStatus::Waiting))
            continue;
        const auto events = m_streamEvents.constFind(session.id);
        if (events == m_streamEvents.constEnd())
            continue;
        const QString sid = QString::number(session.id);
        for (const agentops::PendingQuestion &question : agentops::questionsInEvents(*events)) {
            if (!m_agentPermissionController->isPending(sid, question.requestId))
                continue;
            agentops::ApprovalFacts facts;
            facts.id = agentops::approvalId(session.id, question.requestId, salt);
            facts.sessionId = session.id;
            facts.title = agentOpsSessionTitle(session);
            facts.repo = session.owner + QLatin1Char('/') + session.name;
            facts.provider = session.provider;
            facts.capability = agentops::capabilityFromQuestion(question.question);
            facts.question = question.question;
            facts.askedAtMs = question.askedAtMs;
            rows.append(agentops::approvalRow(facts));
            if (index)
                index->insert(facts.id, qMakePair(session.id, question.requestId));
            if (rows.size() >= agentops::kMaxApprovals)
                break;
        }
    }
    return rows;
}

namespace {

QList<agentops::UsageProvider> agentOpsUsageProviders()
{
    QSettings settings;
    auto window = [&settings](const QString &id, const QString &label, const QString &pctKey,
                              const QString &resetKey, const QString &exhaustedKey,
                              qint64 windowMs) {
        agentops::UsageWindow w;
        w.id = id;
        w.label = label;
        w.usedPercent = settings.contains(pctKey) ? qBound(0, settings.value(pctKey).toInt(), 100)
                                                  : -1;
        w.resetAtMs = settings.value(resetKey).toLongLong();
        w.exhausted = settings.value(exhaustedKey, false).toBool();
        w.windowMs = windowMs;
        return w;
    };
    auto accounts = [](const QString &provider) {
        QList<agentops::UsageAccount> out;
        const QString active = activeAgentAccount(provider).id;
        for (const AgentAccountProfile &profile : agentAccountProfiles(provider))
            out.append({profile.id, profile.label, profile.id == active,
                        agentAccountSignedIn(provider, profile.configDir),
                        agentAccountIdentity(provider, profile.configDir),
                        agentAccountUsageWindows(provider, profile.id)});
        return out;
    };

    agentops::UsageProvider claude;
    claude.id = QStringLiteral("claude-code");
    claude.label = QStringLiteral("Claude Code");
    claude.windows << window(QStringLiteral("5h"), QStringLiteral("5-hour"),
                             kClaudeUsage5hPctSetting, kClaudeUsage5hResetSetting,
                             kClaudeUsage5hExhaustedSetting, kAgentLimit5hMs)
                   << window(QStringLiteral("weekly"), QStringLiteral("Weekly"),
                             kClaudeUsageWeekPctSetting, kClaudeUsageWeekResetSetting,
                             kClaudeUsageWeekExhaustedSetting, kAgentLimitWeekMs)
                   << window(QStringLiteral("fable"), QStringLiteral("Fable weekly"),
                             kClaudeUsageFablePctSetting, kClaudeUsageFableResetSetting,
                             kClaudeUsageFableExhaustedSetting, kAgentLimitWeekMs);
    claude.accounts = accounts(QStringLiteral("claude-code"));
    claude.creditsEnabled = settings.value(kClaudeCreditsEnabledSetting, false).toBool();
    if (claude.creditsEnabled) {
        if (settings.contains(kClaudeCreditsUsedSetting))
            claude.creditsUsed = settings.value(kClaudeCreditsUsedSetting).toDouble() / 100.0;
        if (settings.contains(kClaudeCreditsLimitSetting))
            claude.creditsLimit = settings.value(kClaudeCreditsLimitSetting).toDouble() / 100.0;
        claude.creditsCurrency = settings.value(kClaudeCreditsCurrencySetting).toString();
    }

    agentops::UsageProvider codex;
    codex.id = QStringLiteral("codex");
    codex.label = QStringLiteral("Codex");
    if (settings.contains(kCodexUsageWindowsSetting))
        codex.windows = agentops::codexWindows(
            codexUsageWindows(settings.value(kCodexUsageWindowsSetting)));
    if (codex.windows.isEmpty())
        codex.windows << window(QStringLiteral("5h"), QStringLiteral("5-hour"),
                                kCodexUsage5hPctSetting, kCodexUsage5hResetSetting,
                                kCodexUsage5hExhaustedSetting, kAgentLimit5hMs)
                      << window(QStringLiteral("weekly"), QStringLiteral("Weekly"),
                                kCodexUsageWeekPctSetting, kCodexUsageWeekResetSetting,
                                kCodexUsageWeekExhaustedSetting, kAgentLimitWeekMs);
    codex.accounts = accounts(QStringLiteral("codex"));
    return {claude, codex};
}

QJsonArray usageWindowsJson(const QList<agentops::UsageWindow> &windows)
{
    QJsonArray out;
    for (const agentops::UsageWindow &w : windows)
        out.append(QJsonObject{{QStringLiteral("id"), w.id},
                               {QStringLiteral("label"), w.label},
                               {QStringLiteral("usedPercent"), w.usedPercent},
                               {QStringLiteral("resetAtMs"), double(w.resetAtMs)},
                               {QStringLiteral("exhausted"), w.exhausted}});
    return out;
}

} // namespace

QJsonObject MainWindow::localControlAgentOpsUsage()
{
    QJsonObject summary = agentops::usageSummary(
        agentOpsUsageProviders(), m_claudeSpendUsd, m_openAiSpendUsd,
        QDateTime::currentMSecsSinceEpoch());
    summary.insert(QStringLiteral("activeProvider"), defaultAgentProvider());
    return summary;
}

QJsonObject MainWindow::localControlAgentOpsContext()
{
    AgentOpsState *state = agentOpsState(this);
    QHash<QString, QPair<int, QString>> index;
    const QJsonArray approvals = localControlAgentOpsApprovals(state->salt, &index);
    QJsonArray approvalIndex;
    for (auto it = index.cbegin(); it != index.cend(); ++it)
        approvalIndex.append(QJsonObject{{QStringLiteral("id"), it.key()},
                                         {QStringLiteral("session"), it->first},
                                         {QStringLiteral("request"), it->second}});
    QJsonObject out{{QStringLiteral("approvals"), approvals},
                    {QStringLiteral("approvalIndex"), approvalIndex}};
    if (m_agentCapabilityBroker)
        out.insert(QStringLiteral("grants"), m_agentCapabilityBroker->persistentGrants());
    return out;
}

QJsonObject MainWindow::localControlAgentOpsUsageInputs()
{
    QJsonArray providers;
    for (const agentops::UsageProvider &provider : agentOpsUsageProviders()) {
        QJsonArray accounts;
        for (const agentops::UsageAccount &account : provider.accounts)
            accounts.append(QJsonObject{{QStringLiteral("id"), account.id},
                                        {QStringLiteral("label"), account.label},
                                        {QStringLiteral("active"), account.active},
                                        {QStringLiteral("signedIn"), account.signedIn},
                                        {QStringLiteral("identity"), account.identity},
                                        {QStringLiteral("windows"), usageWindowsJson(account.windows)}});
        providers.append(QJsonObject{{QStringLiteral("id"), provider.id},
                                     {QStringLiteral("label"), provider.label},
                                     {QStringLiteral("windows"), usageWindowsJson(provider.windows)},
                                     {QStringLiteral("accounts"), accounts},
                                     {QStringLiteral("creditsEnabled"), provider.creditsEnabled},
                                     {QStringLiteral("creditsUsed"), provider.creditsUsed},
                                     {QStringLiteral("creditsLimit"), provider.creditsLimit},
                                     {QStringLiteral("creditsCurrency"), provider.creditsCurrency}});
    }
    QJsonObject usage{{QStringLiteral("providers"), providers},
                      {QStringLiteral("activeProvider"), defaultAgentProvider()}};
    if (!std::isnan(m_claudeSpendUsd))
        usage.insert(QStringLiteral("claudeSpendUsd"), m_claudeSpendUsd);
    if (!std::isnan(m_openAiSpendUsd))
        usage.insert(QStringLiteral("openAiSpendUsd"), m_openAiSpendUsd);
    return usage;
}

void MainWindow::armLocalControlAgentOpsBell()
{
    AgentOpsState *state = agentOpsState(this);
    if (state->bellArmed || !m_localControl)
        return;
    if (!m_localControlAgentsTimer)
        notifyLocalControlAgentsChanged();
    if (!m_localControlAgentsTimer)
        return;
    state->bellArmed = true;
    state->lastSignature =
        agentops::approvalsSignature(QStringList());
    connect(m_localControlAgentsTimer, &QTimer::timeout, this,
            [this] { ringLocalControlAgentOpsBell(); });
    ringLocalControlAgentOpsBell();
}

void MainWindow::ringLocalControlAgentOpsBell()
{
    AgentOpsState *state = agentOpsState(this);
    QHash<QString, QPair<int, QString>> index;
    const QJsonArray approvals = localControlAgentOpsApprovals(state->salt, &index);
    pushAgentApprovalPings(approvals);
    if (!m_localControl)
        return;
    const QString signature = agentops::approvalsSignature(index.keys());
    if (signature == state->lastSignature)
        return;
    state->lastSignature = signature;
    m_localControl->broadcast(QStringLiteral("agentops.changed"),
                              {{QStringLiteral("what"), QStringLiteral("approvals")},
                               {QStringLiteral("count"), approvals.size()}});
}

bool MainWindow::prepareAgentApprovalPingRequest(const QString &sessionId,
                                                 const QString &approvalId,
                                                 const QString &agent, const QString &summary,
                                                 QNetworkRequest *request, QByteArray *body)
{
    const agentops::ApprovalPing ping{sessionId, approvalId, agent, summary};
    if (!request || !body || !agentops::validApprovalId(ping.approvalId) ||
        !agentops::validPingField(ping.sessionId))
        return false;
    QUrl url = catalogApiUrl();
    url.setPath(agentops::approvalPingPath());
    url.setQuery(QString());
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (!m_profileIdentity.isValid())
        m_profileIdentity.load();
    QString user, ts, sig;
    if (signOrgTaskProof(agentops::approvalPingProof(), agentops::approvalPingResource(ping),
                         &user, &ts, &sig)) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("user"), user);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"), sig);
        url.setQuery(query);
    }
    if (sessionToken.isEmpty() && !url.hasQuery())
        return false;
    *request = QNetworkRequest(url);
    request->setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request->setTransferTimeout(15000);
    if (!sessionToken.isEmpty())
        request->setRawHeader("Authorization",
                              (QStringLiteral("Bearer ") + sessionToken).toUtf8());
    *body = QJsonDocument(agentops::approvalPingBody(ping)).toJson(QJsonDocument::Compact);
    return true;
}

void MainWindow::pushAgentApprovalPings(const QJsonArray &approvals)
{
    if (approvals.isEmpty() || !m_networkAccess)
        return;
    AgentOpsState *state = agentOpsState(this);
    const QList<agentops::ApprovalPing> fresh = state->pings.unsent(approvals);
    if (fresh.isEmpty())
        return;
    bool changed = false;
    for (const agentops::ApprovalPing &ping : fresh) {
        QNetworkRequest request;
        QByteArray payload;
        if (!prepareAgentApprovalPingRequest(ping.sessionId, ping.approvalId, ping.agent,
                                             ping.summary, &request, &payload))
            break;
        state->pings.add(ping.approvalId);
        changed = true;
        QNetworkReply *reply = m_networkAccess->post(request, payload);
        const QString approvalId = ping.approvalId;
        const QString sessionId = ping.sessionId;
        connect(reply, &QNetworkReply::finished, this, [this, reply, approvalId, sessionId] {
            reply->deleteLater();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool networkError = reply->error() != QNetworkReply::NoError;
            if (!networkError && status >= 200 && status < 300)
                return;
            logSystem(QStringLiteral("Couldn't send the approval ping for agent %1 to your "
                                     "other devices (%2).")
                          .arg(sessionId, status > 0 ? QString::number(status)
                                                     : reply->errorString()));
            if (!agentops::approvalPingRetryable(status, networkError))
                return;
            AgentOpsState *state = agentOpsState(this);
            state->pings.remove(approvalId);
            state->savePings();
        });
    }
    if (changed)
        state->savePings();
}

void MainWindow::handleLocalControlAgentOpsRequest(int connectionId, int requestId,
                                                   const QString &method,
                                                   const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    AgentOpsState *state = agentOpsState(this);
    armLocalControlAgentOpsBell();
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString holder = agentops::confirmationHolder(device.key, connectionId);
    const bool coreConfirmed = forkmesh::coreagentmodel::confirmedByCore(connectionId, params);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto audit = [this, connectionId](const QString &what) {
        logSystem(QStringLiteral("%1, requested by %2.")
                      .arg(what, localControlDeviceLabel(connectionId)));
    };
    const auto fail = [server, connectionId, requestId](const QString &code) {
        server->respondError(connectionId, requestId, code);
    };
    const auto ownSession = [this](int id) -> AgentSession * {
        if (id <= 0 || isExternalSession(id))
            return nullptr;
        AgentSession *session = findAgentSession(id);
        return session && !session->cloud && session->deletedAtMs == 0 ? session : nullptr;
    };
    const auto outsideAgent = [this](int id, OutsideAgent *found) {
        if (id >= 0 || !isExternalSession(id) || isCloudSession(id))
            return false;
        found->terminal = localControlTerminalForRow(id);
        if (found->terminal.isEmpty())
            found->terminal = localControlTerminalForOutside(id);
        if (const auto machine = m_machineAgents.constFind(id);
            machine != m_machineAgents.constEnd()) {
            found->label = MachineAgentScan::label(machine->kind);
            found->cwd = machine->cwd;
            found->otherUser = !machine->otherUser.isEmpty();
            found->pids = {machine->pid};
            return true;
        }
        if (const auto surfaced = m_externalSurfaced.constFind(id);
            surfaced != m_externalSurfaced.constEnd() && externalIsLive(surfaced->uuid)) {
            found->label = MachineAgentScan::label(QStringLiteral("claude"));
            found->cwd = surfaced->cwd;
            found->uuid = surfaced->uuid;
            found->pids = m_externalSurfacedPids.value(surfaced->uuid);
            if (const ClaudePeerSession *peer =
                    ClaudePeerMessaging::find(m_claudePeerSessions, {}, surfaced->uuid))
                found->pids.append(peer->pid);
            if (found->pids.isEmpty())
                found->pids = ClaudeSessionScan::findSessionPids(surfaced->uuid, surfaced->cwd);
            return true;
        }
        const auto terminal = m_localControlTerminals.constFind(found->terminal);
        if (found->terminal.isEmpty() || terminal == m_localControlTerminals.constEnd())
            return false;
        found->label = forkmesh::localcontrol::terminals::cliLabel(terminal->provider);
        found->cwd = terminal->cwd;
        return true;
    };
    const auto branchBusy = [this](const AgentSession &target) {
        for (const AgentSession &other : std::as_const(m_agentSessions)) {
            if (other.merged || other.branchName != target.branchName ||
                other.owner != target.owner || other.name != target.name)
                continue;
            if (agentOpsSessionActive(other) && agentSessionWorkInFlight(other.id))
                return true;
        }
        return false;
    };
    const QString action = params.value(QStringLiteral("action")).toString();

    if (method == QLatin1String("agentops.approvals")) {
        const QJsonArray approvals = localControlAgentOpsApprovals(state->salt);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("approvals"), approvals},
                         {QStringLiteral("count"), approvals.size()}});
        return;
    }

    if (method == QLatin1String("agentops.prepare")) {
        if (!agentops::isKnownAction(action)) {
            fail(QStringLiteral("unknown_action"));
            return;
        }
        QString target;
        QString summary;
        if (action == QLatin1String("approve") || action == QLatin1String("deny")) {
            const QString id = params.value(QStringLiteral("id")).toString();
            const bool allow = action == QLatin1String("approve");
            const QString lifetime = allow ? params.value(QStringLiteral("lifetime"))
                                                 .toString(QStringLiteral("once"))
                                           : QStringLiteral("once");
            if (!agentops::validApprovalId(id) || !agentops::validLifetime(lifetime)) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            QHash<QString, QPair<int, QString>> index;
            const QJsonArray approvals = localControlAgentOpsApprovals(state->salt, &index);
            if (!index.contains(id)) {
                fail(QStringLiteral("unknown_approval"));
                return;
            }
            QString question;
            QString title;
            for (const QJsonValue &value : approvals) {
                const QJsonObject row = value.toObject();
                if (row.value(QStringLiteral("id")).toString() == id) {
                    question = row.value(QStringLiteral("question")).toString();
                    title = row.value(QStringLiteral("title")).toString();
                    break;
                }
            }
            target = agentops::answerTarget(id, allow, lifetime);
            summary = agentops::approvalSummary(allow, lifetime, question, title);
        } else if (action == QLatin1String("merge")) {
            const AgentSession *session = ownSession(params.value(QStringLiteral("id")).toInt());
            if (!session || session->branchName.isEmpty() || session->merged) {
                fail(QStringLiteral("unknown_session"));
                return;
            }
            if (repoIndexFor(session->owner, session->name) < 0) {
                fail(QStringLiteral("no_checkout"));
                return;
            }
            if (branchBusy(*session)) {
                fail(QStringLiteral("agent_running"));
                return;
            }
            target = QString::number(session->id);
            summary = agentops::mergeSummary(session->branchName, agentMergeBase(*session),
                                             session->owner + QLatin1Char('/') + session->name);
        } else if (action == QLatin1String("delete")) {
            const int id = params.value(QStringLiteral("id")).toInt();
            OutsideAgent outside;
            if (outsideAgent(id, &outside)) {
                if (outside.otherUser) {
                    fail(QStringLiteral("outside_other_user"));
                    return;
                }
                target = QString::number(id);
                summary = agentops::outsideDeleteSummary(outside.label, outside.cwd);
            } else {
                const AgentSession *session = ownSession(id);
                if (!session) {
                    fail(QStringLiteral("unknown_session"));
                    return;
                }
                target = QString::number(session->id);
                summary = agentops::deleteSummary(
                    session->merged ? QString() : session->branchName,
                    session->owner + QLatin1Char('/') + session->name,
                    isStoppableAgentSession(session->id));
            }
        } else if (action == QLatin1String("stop") || action == QLatin1String("continue")) {
            QList<int> ids;
            if (!agentops::canonicalIds(params.value(QStringLiteral("ids")).toArray(), &ids)) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            target = agentops::bulkTarget(action, ids);
            summary = agentops::bulkSummary(action, ids.size());
        } else if (action == QLatin1String("purge")) {
            QList<int> ids;
            if (!agentops::canonicalIds(params.value(QStringLiteral("ids")).toArray(), &ids,
                                        agentops::kMaxBinAgents)) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            target = agentops::bulkTarget(action, ids);
            summary = agentops::purgeSummary(ids.size());
        } else if (action == QLatin1String("revoke")) {
            const QString grantId = params.value(QStringLiteral("grantId")).toString();
            if (!agentops::validGrantId(grantId) || !m_agentCapabilityBroker) {
                fail(QStringLiteral("unknown_grant"));
                return;
            }
            for (const QJsonValue &value : m_agentCapabilityBroker->persistentGrants()) {
                const QJsonObject grant = value.toObject();
                if (grant.value(QStringLiteral("id")).toString() == grantId) {
                    target = grantId;
                    summary = agentops::revokeSummary(
                        grant.value(QStringLiteral("capability")).toString(),
                        grant.value(QStringLiteral("resource")).toString());
                    break;
                }
            }
            if (target.isEmpty()) {
                fail(QStringLiteral("unknown_grant"));
                return;
            }
        }
        const QString token = state->confirmations.issue(holder, action, target, now);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("summary"), summary},
                         {QStringLiteral("destructive"), agentops::isDestructiveAction(action)},
                         {QStringLiteral("expiresInMs"),
                          int(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }

    if (method == QLatin1String("agentops.answer")) {
        const QString id = params.value(QStringLiteral("id")).toString();
        const QString decision = params.value(QStringLiteral("decision")).toString();
        const bool allow = decision == QLatin1String("allow");
        const QString lifetime = allow ? params.value(QStringLiteral("lifetime"))
                                             .toString(QStringLiteral("once"))
                                       : QStringLiteral("once");
        if (!agentops::validApprovalId(id) ||
            (!allow && decision != QLatin1String("deny")) ||
            !agentops::validLifetime(lifetime)) {
            fail(QStringLiteral("bad_request"));
            return;
        }
        const auto result = coreConfirmed ? hostctl::ConfirmationStore::Result::Ok : state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), holder,
            allow ? QStringLiteral("approve") : QStringLiteral("deny"),
            agentops::answerTarget(id, allow, lifetime), now);
        if (result != hostctl::ConfirmationStore::Result::Ok) {
            fail(confirmationErrorCode(result));
            return;
        }
        QHash<QString, QPair<int, QString>> index;
        localControlAgentOpsApprovals(state->salt, &index);
        const auto found = index.constFind(id);
        if (found == index.constEnd()) {
            fail(QStringLiteral("unknown_approval"));
            return;
        }
        const int sessionId = found->first;
        const QString request = found->second;
        audit(allow ? QStringLiteral("Allowed a permission request of agent %1 (%2)")
                          .arg(sessionId)
                          .arg(lifetime)
                    : QStringLiteral("Denied a permission request of agent %1").arg(sessionId));
        answerAgentQuestion(sessionId, request, agentops::answerText(allow, lifetime),
                            false, false);
        const bool resolved =
            !m_agentPermissionController ||
            !m_agentPermissionController->isPending(QString::number(sessionId), request);
        if (!m_localControl)
            return;
        if (!resolved) {
            fail(QStringLiteral("not_delivered"));
            return;
        }
        m_localControl->respond(connectionId, requestId,
                                {{QStringLiteral("id"), id},
                                 {QStringLiteral("decision"), decision},
                                 {QStringLiteral("lifetime"), lifetime}});
        ringLocalControlAgentOpsBell();
        notifyLocalControlAgentsChanged();
        return;
    }

    if (method == QLatin1String("agentops.merge")) {
        const int sessionId = params.value(QStringLiteral("id")).toInt();
        const auto result = coreConfirmed ? hostctl::ConfirmationStore::Result::Ok : state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), holder, QStringLiteral("merge"),
            QString::number(sessionId), now);
        if (result != hostctl::ConfirmationStore::Result::Ok) {
            fail(confirmationErrorCode(result));
            return;
        }
        const AgentSession *session = ownSession(sessionId);
        if (!session || session->branchName.isEmpty() || session->merged) {
            fail(QStringLiteral("unknown_session"));
            return;
        }
        const int repoIndex = repoIndexFor(session->owner, session->name);
        if (repoIndex < 0 || m_repositories.at(repoIndex).localPath.isEmpty()) {
            fail(QStringLiteral("no_checkout"));
            return;
        }
        if (branchBusy(*session)) {
            fail(QStringLiteral("agent_running"));
            return;
        }
        if (state->mergesInFlight.contains(sessionId)) {
            fail(QStringLiteral("already_running"));
            return;
        }
        state->mergesInFlight.insert(sessionId);
        const QString branch = session->branchName;
        const QString base = agentMergeBase(*session);
        const QString localPath = m_repositories.at(repoIndex).localPath;
        runOffThread<MergeCheck>(
            [localPath, base, branch] {
                MergeCheck check;
                QString into = base;
                if (!agentOpsGit(localPath, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                             QStringLiteral("--quiet"), into})
                         .ok) {
                    into = agentOpsGit(localPath, {QStringLiteral("symbolic-ref"),
                                                   QStringLiteral("--short"), QStringLiteral("HEAD")})
                               .out.trimmed();
                }
                if (into.isEmpty()) {
                    check.error = QStringLiteral("no_base_branch");
                    return check;
                }
                check.into = into;
                const GitRun tree = agentOpsGit(
                    localPath, {QStringLiteral("merge-tree"), QStringLiteral("--write-tree"),
                                QStringLiteral("--name-only"), into, branch});
                if (tree.exitCode == 1)
                    check.error = QStringLiteral("conflicts");
                else if (!tree.ok)
                    check.error = QStringLiteral("conflict_check_unavailable");
                return check;
            },
            [this, connectionId, requestId, sessionId, branch, audit](MergeCheck check) {
                AgentOpsState *st = agentOpsState(this);
                st->mergesInFlight.remove(sessionId);
                if (!m_localControl)
                    return;
                if (!check.error.isEmpty()) {
                    m_localControl->respondError(connectionId, requestId, check.error);
                    return;
                }
                const AgentSession *live = findAgentSession(sessionId);
                const int ri = live ? repoIndexFor(live->owner, live->name) : -1;
                if (!live || live->merged || live->branchName != branch || ri < 0) {
                    m_localControl->respondError(connectionId, requestId,
                                                 QStringLiteral("unknown_session"));
                    return;
                }
                const QString path = m_repositories.at(ri).localPath;
                audit(QStringLiteral("Merging and cleaning up agent %1 (%2)")
                          .arg(sessionId)
                          .arg(branch));
                if (!bindRepoDetailToRepo(ri)) {
                    if (m_localControl)
                        m_localControl->respondError(connectionId, requestId,
                                                     QStringLiteral("repo_unavailable"));
                    return;
                }
                const quint64 noticesBefore = m_operationNoticeSerial;
                mergeWorktreeIntoMain(
                    branch, worktreePathForBranch(path, branch, WorktreeLookup::Any),
                    true,
                    [this, connectionId, requestId, sessionId, branch, path,
                     into = check.into, noticesBefore](bool merged) {
                        if (!m_localControl)
                            return;
                        if (!merged) {
                            m_localControl->respondError(
                                connectionId, requestId,
                                QStringLiteral("merge_failed"),
                                m_operationNoticeSerial != noticesBefore
                                    ? m_lastOperationNotice
                                    : QString());
                            return;
                        }
                        notifyLocalControlAgentsChanged();
                        runOffThread<agentops::MergeStats>(
                            [path, into, branch] {
                                return readLandedMergeStats(path, into, branch);
                            },
                            [this, connectionId, requestId, sessionId,
                             branch](agentops::MergeStats stats) {
                                if (!m_localControl)
                                    return;
                                m_localControl->respond(
                                    connectionId, requestId,
                                    {{QStringLiteral("id"), sessionId},
                                     {QStringLiteral("branch"), branch},
                                     {QStringLiteral("merged"), true},
                                     {QStringLiteral("stats"), agentops::mergeStatsJson(stats)}});
                            });
                    });
            });
        return;
    }

    if (method == QLatin1String("agentops.delete")) {
        const int sessionId = params.value(QStringLiteral("id")).toInt();
        const auto result = coreConfirmed ? hostctl::ConfirmationStore::Result::Ok : state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), holder, QStringLiteral("delete"),
            QString::number(sessionId), now);
        if (result != hostctl::ConfirmationStore::Result::Ok) {
            fail(confirmationErrorCode(result));
            return;
        }
        OutsideAgent outside;
        if (outsideAgent(sessionId, &outside)) {
            if (outside.otherUser) {
                fail(QStringLiteral("outside_other_user"));
                return;
            }
            audit(QStringLiteral("Ending %1 started outside ForkMesh in %2 and removing it")
                      .arg(outside.label, outside.cwd));
            server->respond(connectionId, requestId, {{QStringLiteral("id"), sessionId}});
            if (!outside.terminal.isEmpty())
                endLocalControlTerminal(outside.terminal, -1);
            QList<qint64> pids;
            for (const qint64 pid : std::as_const(outside.pids))
                if (pid > 0 && !pids.contains(pid))
                    pids.append(pid);
            if (!pids.isEmpty())
                killExternalSessionPids(pids);
            m_machineAgents.remove(sessionId);
            if (!outside.uuid.isEmpty()) {
                m_externalStopped.insert(outside.uuid);
                unsurfaceExternalSession(sessionId);
            }
            notifyLocalControlAgentsChanged();
            return;
        }
        const AgentSession *session = ownSession(sessionId);
        if (!session) {
            fail(QStringLiteral("unknown_session"));
            return;
        }
        const bool running = isStoppableAgentSession(sessionId);
        audit(running ? QStringLiteral("Stopping and deleting agent %1").arg(sessionId)
                      : QStringLiteral("Deleting agent %1").arg(sessionId));
        const int repoIndex = repoIndexFor(session->owner, session->name);
        const RepositoryRecord repo =
            repoIndex >= 0 ? m_repositories.at(repoIndex) : RepositoryRecord();
        agentops::DeleteFacts before;
        before.branch = session->branchName;
        before.issue = session->issueNumber;
        before.pull = session->prNumber;
        before.pullMerged = session->merged;
        before.pullReachable = repoIndex >= 0;
        before.task = session->orgTaskId.trimmed();
        if (!repo.localPath.isEmpty() && !before.branch.isEmpty()) {
            GitKeepAlive keepAlive;
            before.branchExisted = localBranchExists(repo.localPath, before.branch);
            before.worktree =
                worktreePathForBranch(repo.localPath, before.branch, WorktreeLookup::Any);
            before.worktreeExisted =
                !before.worktree.isEmpty() && QDir(before.worktree).exists();
        }
        auto trace = std::make_shared<agentops::DeleteTrace>();
        auto answered = std::make_shared<bool>(false);
        const auto answer = [this, connectionId, requestId, sessionId, repo, before, trace,
                             answered](bool settled) {
            if (*answered)
                return;
            *answered = true;
            trace->done = nullptr;
            if (!m_localControl)
                return;
            agentops::DeleteFacts after = before;
            after.settled = settled;
            const AgentSession *left = findAgentSession(sessionId);
            after.agentGone = !left || left->deletedAtMs > 0;
            after.branchLeft = after.branchExisted &&
                               localBranchExists(repo.localPath, after.branch);
            after.worktreeLeft = after.worktreeExisted && QDir(after.worktree).exists();
            after.taskStatus = trace->taskStatus;
            after.taskReason = trace->taskReason;
            if (after.issue > 0 && !repo.localPath.isEmpty()) {
                const RepositoryRecord &writable = writableRecordFor(repo);
                IssueStore issues(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                                  m_userName);
                for (const Issue &issue : issues.loadAll()) {
                    if (issue.number == after.issue) {
                        after.issueFound = true;
                        after.issueStatus = issue.status;
                        break;
                    }
                }
            }
            m_localControl->respond(
                connectionId, requestId,
                {{QStringLiteral("id"), sessionId},
                 {QStringLiteral("settled"), settled},
                 {QStringLiteral("parts"), agentops::deletePartsJson(agentops::deleteParts(after))}});
        };
        trace->done = [answer](bool) { answer(true); };
        QTimer::singleShot(kDeleteAnswerWaitMs, this, [answer] { answer(false); });
        if (running)
            stopAgentSessionById(sessionId);
        auto deleteOnceStopped = std::make_shared<std::function<void(int)>>();
        const std::weak_ptr<std::function<void(int)>> again = deleteOnceStopped;
        *deleteOnceStopped = [this, sessionId, again, trace](int waited) {
            AgentRunner *runner = runnerForSession(sessionId);
            if (runner && runner->busy() && waited < kDeleteStopWaitMs) {
                if (const auto next = again.lock())
                    QTimer::singleShot(kDeleteStopPollMs, this, [next, waited] {
                        (*next)(waited + kDeleteStopPollMs);
                    });
                return;
            }
            deleteAgentSessionAndCleanup(sessionId, trace);
            notifyLocalControlAgentsChanged();
        };
        QTimer::singleShot(0, this, [deleteOnceStopped] { (*deleteOnceStopped)(0); });
        return;
    }

    if (method == QLatin1String("agentops.diff")) {
        const AgentSession *session = ownSession(params.value(QStringLiteral("id")).toInt());
        if (!session || session->branchName.isEmpty()) {
            fail(QStringLiteral("unknown_session"));
            return;
        }
        const int repoIndex = repoIndexFor(session->owner, session->name);
        if (repoIndex < 0 || m_repositories.at(repoIndex).localPath.isEmpty()) {
            fail(QStringLiteral("no_checkout"));
            return;
        }
        const int sessionId = session->id;
        const QString branch = session->branchName;
        const QString base = agentMergeBase(*session);
        const bool merged = session->merged;
        const QString localPath = m_repositories.at(repoIndex).localPath;
        runOffThread<DiffRead>(
            [localPath, base, branch] { return readAgentDiff(localPath, base, branch); },
            [this, connectionId, requestId, sessionId, branch, merged](DiffRead read) {
                if (!m_localControl)
                    return;
                if (!read.error.isEmpty()) {
                    m_localControl->respondError(connectionId, requestId, read.error);
                    return;
                }
                const QStringList credentials = localProviderCredentialValues();
                m_localControl->respond(
                    connectionId, requestId,
                    {{QStringLiteral("id"), sessionId},
                     {QStringLiteral("branch"), branch},
                     {QStringLiteral("base"), read.base},
                     {QStringLiteral("merged"), merged},
                     {QStringLiteral("files"), read.files},
                     {QStringLiteral("committed"),
                      redactProviderCredentials(read.committed, credentials)},
                     {QStringLiteral("uncommitted"),
                      redactProviderCredentials(read.uncommitted, credentials)},
                     {QStringLiteral("untracked"), QJsonArray::fromStringList(read.untracked)},
                     {QStringLiteral("hasWorktree"), !read.worktree.isEmpty()},
                     {QStringLiteral("truncated"), read.truncated}});
            });
        return;
    }

    if (method == QLatin1String("agentops.file")) {
        const AgentSession *session = ownSession(params.value(QStringLiteral("id")).toInt());
        const QString path = params.value(QStringLiteral("path")).toString();
        const QString change = params.value(QStringLiteral("change")).toString();
        const QString side = params.value(QStringLiteral("side")).toString();
        if (!agentops::validDiffImagePath(path) ||
            (change != QLatin1String("committed") && change != QLatin1String("uncommitted")) ||
            (side != QLatin1String("old") && side != QLatin1String("new"))) {
            fail(QStringLiteral("bad_request"));
            return;
        }
        if (!session || session->branchName.isEmpty()) {
            fail(QStringLiteral("unknown_session"));
            return;
        }
        const int repoIndex = repoIndexFor(session->owner, session->name);
        if (repoIndex < 0 || m_repositories.at(repoIndex).localPath.isEmpty()) {
            fail(QStringLiteral("no_checkout"));
            return;
        }
        const QString branch = session->branchName;
        const QString base = agentMergeBase(*session);
        const QString localPath = m_repositories.at(repoIndex).localPath;
        const bool uncommitted = change == QLatin1String("uncommitted");
        const bool after = side == QLatin1String("new");
        runOffThread<ImageRead>(
            [localPath, base, branch, path, uncommitted, after] {
                return readAgentDiffImage(localPath, base, branch, path, uncommitted, after);
            },
            [this, connectionId, requestId, path](ImageRead read) {
                if (!m_localControl)
                    return;
                if (!read.error.isEmpty()) {
                    m_localControl->respondError(connectionId, requestId, read.error);
                    return;
                }
                m_localControl->respond(
                    connectionId, requestId,
                    {{QStringLiteral("path"), path},
                     {QStringLiteral("mime"), agentops::diffImageMime(path)},
                     {QStringLiteral("size"), double(read.data.size())},
                     {QStringLiteral("data"), QString::fromLatin1(read.data.toBase64())}});
            });
        return;
    }

    if (method == QLatin1String("agentops.bulk")) {
        QList<int> ids;
        if ((action != QLatin1String("stop") && action != QLatin1String("continue")) ||
            !agentops::canonicalIds(params.value(QStringLiteral("ids")).toArray(), &ids)) {
            fail(QStringLiteral("bad_request"));
            return;
        }
        const auto result = coreConfirmed ? hostctl::ConfirmationStore::Result::Ok : state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), holder, action,
            agentops::bulkTarget(action, ids), now);
        if (result != hostctl::ConfirmationStore::Result::Ok) {
            fail(confirmationErrorCode(result));
            return;
        }
        QStringList named;
        for (const int id : std::as_const(ids))
            named << QString::number(id);
        audit(QStringLiteral("%1 agents %2")
                  .arg(action == QLatin1String("stop") ? QStringLiteral("Stopping")
                                                       : QStringLiteral("Continuing"),
                       named.join(QStringLiteral(", "))));
        QJsonArray done;
        QJsonArray skipped;
        for (const int id : std::as_const(ids)) {
            const AgentSession *session = ownSession(id);
            bool ok = false;
            if (session && action == QLatin1String("stop"))
                ok = isStoppableAgentSession(id) && stopAgentSessionById(id);
            else if (session && !agentOpsSessionActive(*session)) {
                continueAgentSession(id);
                ok = true;
            }
            (ok ? done : skipped).append(id);
        }
        if (!m_localControl)
            return;
        m_localControl->respond(connectionId, requestId,
                                {{QStringLiteral("action"), action},
                                 {QStringLiteral("done"), done},
                                 {QStringLiteral("skipped"), skipped}});
        notifyLocalControlAgentsChanged();
        return;
    }

    if (method == QLatin1String("agentops.mergestats")) {
        int repoIndex = -1;
        QString base;
        const int sessionId = params.value(QStringLiteral("id")).toInt();
        const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        if (sessionId > 0) {
            const AgentSession *session = ownSession(sessionId);
            if (!session) {
                fail(QStringLiteral("unknown_session"));
                return;
            }
            repoIndex = repoIndexFor(session->owner, session->name);
            base = agentMergeBase(*session);
        } else if (!owner.isEmpty() || !name.isEmpty()) {
            repoIndex = repoIndexFor(owner, name);
        } else {
            const auto hasCheckout = [this](int index) {
                return index >= 0 && index < m_repositories.size() &&
                       !m_repositories.at(index).localPath.isEmpty();
            };
            if (hasCheckout(m_repoDetailIndex)) {
                repoIndex = m_repoDetailIndex;
            } else {
                qint64 newest = -1;
                for (const AgentSession &session : std::as_const(m_agentSessions)) {
                    if (session.cloud || isExternalSession(session.id) ||
                        session.deletedAtMs > 0 || session.createdAtMs <= newest)
                        continue;
                    const int index = repoIndexFor(session.owner, session.name);
                    if (!hasCheckout(index))
                        continue;
                    newest = session.createdAtMs;
                    repoIndex = index;
                }
            }
        }
        if (repoIndex < 0 || m_repositories.at(repoIndex).localPath.isEmpty()) {
            fail(QStringLiteral("no_checkout"));
            return;
        }
        const QString localPath = m_repositories.at(repoIndex).localPath;
        const QString repo = m_repositories.at(repoIndex).owner + QLatin1Char('/') +
                             m_repositories.at(repoIndex).name;
        runOffThread<MergeStatsRead>(
            [localPath, base] {
                QString into = base;
                if (into.isEmpty() ||
                    !agentOpsGit(localPath, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                             QStringLiteral("--quiet"), into})
                         .ok) {
                    into = agentOpsGit(localPath, {QStringLiteral("symbolic-ref"),
                                                   QStringLiteral("--short"), QStringLiteral("HEAD")})
                               .out.trimmed();
                }
                MergeStatsRead read;
                read.stats = readLandedMergeStats(localPath, into, QString(), /*newest=*/true,
                                                  &read.branch);
                return read;
            },
            [this, connectionId, requestId, repo](MergeStatsRead read) {
                if (!m_localControl)
                    return;
                if (read.stats.base.isEmpty()) {
                    m_localControl->respondError(connectionId, requestId,
                                                 QStringLiteral("no_base_branch"));
                    return;
                }
                m_localControl->respond(connectionId, requestId,
                                        {{QStringLiteral("repo"), repo},
                                         {QStringLiteral("branch"), read.branch},
                                         {QStringLiteral("stats"),
                                          agentops::mergeStatsJson(read.stats)}});
            });
        return;
    }

    if (method == QLatin1String("agentops.worktrees")) {
        const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        const int repoIndex = repoIndexFor(owner, name);
        if (repoIndex < 0 || m_repositories.at(repoIndex).localPath.isEmpty()) {
            fail(QStringLiteral("no_checkout"));
            return;
        }
        const QString localPath = m_repositories.at(repoIndex).localPath;
        const QString repo = m_repositories.at(repoIndex).owner + QLatin1Char('/') +
                             m_repositories.at(repoIndex).name;
        QHash<QString, int> sessions;
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            if (session.cloud || isExternalSession(session.id) || session.deletedAtMs > 0 ||
                session.branchName.isEmpty() ||
                session.owner.compare(owner, Qt::CaseInsensitive) != 0 ||
                session.name.compare(name, Qt::CaseInsensitive) != 0)
                continue;
            if (sessions.value(session.branchName) < session.id)
                sessions.insert(session.branchName, session.id);
        }
        runOffThread<WorktreeScan>(
            [localPath, sessions] {
                WorktreeScan scan;
                const GitRun list = agentOpsGit(
                    localPath, {QStringLiteral("worktree"), QStringLiteral("list"),
                                QStringLiteral("--porcelain")});
                if (!list.ok)
                    return scan;
                const QList<agentops::WorktreeEntry> entries =
                    agentops::parseWorktreePorcelain(list.out);
                if (!entries.isEmpty())
                    scan.base = entries.first().branch;
                for (int i = 0; i < entries.size(); ++i) {
                    const agentops::WorktreeEntry &entry = entries.at(i);
                    if (entry.bare)
                        continue;
                    int ahead = 0;
                    int behind = 0;
                    if (i > 0 && !scan.base.isEmpty() && !entry.branch.isEmpty()) {
                        const GitRun counts = agentOpsGit(
                            localPath,
                            {QStringLiteral("rev-list"), QStringLiteral("--left-right"),
                             QStringLiteral("--count"),
                             scan.base + QStringLiteral("...") + entry.branch},
                            8000);
                        if (counts.ok)
                            agentops::parseLeftRight(counts.out, &behind, &ahead);
                    }
                    scan.rows.append(agentops::worktreeRow(entry, i == 0, ahead, behind,
                                                           sessions.value(entry.branch)));
                }
                return scan;
            },
            [this, connectionId, requestId, repo](WorktreeScan scan) {
                if (!m_localControl)
                    return;
                m_localControl->respond(connectionId, requestId,
                                        {{QStringLiteral("repo"), repo},
                                         {QStringLiteral("base"), scan.base},
                                         {QStringLiteral("worktrees"), scan.rows}});
            });
        return;
    }

    if (method == QLatin1String("agentops.usage")) {
        server->respond(connectionId, requestId, localControlAgentOpsUsage());
        return;
    }

    if (method.startsWith(QLatin1String("agentops.accounts."))) {
        const QString provider = params.value(QStringLiteral("provider")).toString();
        if (provider != QLatin1String("claude-code") && provider != QLatin1String("codex")) {
            fail(QStringLiteral("unsupported_provider"));
            return;
        }
        const QString accountId = params.value(QStringLiteral("account")).toString();
        const QString label = params.value(QStringLiteral("label")).toString().trimmed().left(80);
        const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
        const auto profile = std::find_if(profiles.cbegin(), profiles.cend(),
                                          [&accountId](const AgentAccountProfile &candidate) {
                                              return candidate.id == accountId;
                                          });
        const QString providerName = provider == QLatin1String("codex")
                                         ? QStringLiteral("Codex")
                                         : QStringLiteral("Claude Code");
        QString created;
        if (method == QLatin1String("agentops.accounts.select")) {
            if (profile == profiles.cend()) {
                fail(QStringLiteral("unknown_account"));
                return;
            }
            selectAgentAccount(provider, accountId);
            audit(QStringLiteral("Selected the %1 account %2 for new agents")
                      .arg(providerName, profile->label));
        } else if (method == QLatin1String("agentops.accounts.rename")) {
            if (profile == profiles.cend()) {
                fail(QStringLiteral("unknown_account"));
                return;
            }
            if (label.isEmpty() || !renameAgentAccount(provider, accountId, label)) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            refreshAgentAccountUsageMenu(provider);
            refreshAgentToolbarAccountButtons();
            audit(QStringLiteral("Renamed the %1 account %2 to %3")
                      .arg(providerName, profile->label, label));
        } else if (method == QLatin1String("agentops.accounts.add")) {
            if (label.isEmpty()) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            created = createAgentAccount(provider, label);
            if (created.isEmpty()) {
                fail(QStringLiteral("account_failed"));
                return;
            }
            selectAgentAccount(provider, created);
            audit(QStringLiteral("Added the %1 account %2").arg(providerName, label));
        } else if (method == QLatin1String("agentops.accounts.delete")) {
            if (profile == profiles.cend()) {
                fail(QStringLiteral("unknown_account"));
                return;
            }
            if (profile->builtIn) {
                fail(QStringLiteral("default_account"));
                return;
            }
            const QString deletedLabel = profile->label;
            if (!deleteAgentAccount(provider, accountId)) {
                fail(QStringLiteral("account_failed"));
                return;
            }
            refreshAgentAccountUsageMenu(provider);
            audit(QStringLiteral("Deleted the %1 account %2")
                      .arg(providerName, deletedLabel));
        } else {
            fail(QStringLiteral("unsupported"));
            return;
        }
        QJsonObject answer = localControlAgentOpsUsage();
        if (!created.isEmpty())
            answer.insert(QStringLiteral("account"), created);
        server->respond(connectionId, requestId, answer);
        return;
    }

    if (method == QLatin1String("agentops.grants")) {
        const QJsonArray grants = m_agentCapabilityBroker
                                      ? agentops::grantRows(
                                            m_agentCapabilityBroker->persistentGrants())
                                      : QJsonArray();
        server->respond(connectionId, requestId, {{QStringLiteral("grants"), grants}});
        return;
    }

    if (method == QLatin1String("agentops.revoke")) {
        const QString grantId = params.value(QStringLiteral("grantId")).toString();
        if (!agentops::validGrantId(grantId) || !m_agentCapabilityBroker) {
            fail(QStringLiteral("unknown_grant"));
            return;
        }
        const auto result = coreConfirmed ? hostctl::ConfirmationStore::Result::Ok : state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), holder, QStringLiteral("revoke"),
            grantId, now);
        if (result != hostctl::ConfirmationStore::Result::Ok) {
            fail(confirmationErrorCode(result));
            return;
        }
        if (!m_agentCapabilityBroker->revoke(grantId)) {
            fail(QStringLiteral("unknown_grant"));
            return;
        }
        audit(QStringLiteral("Revoked a saved agent permission"));
        QString error;
        if (!AgentCapabilityStore::save(AgentCapabilityStore::defaultPath(),
                                        *m_agentCapabilityBroker, &error)) {
            logSystem(QStringLiteral("Agent permissions could not be saved: %1").arg(error));
            fail(QStringLiteral("not_saved"));
            return;
        }
        server->respond(connectionId, requestId, {{QStringLiteral("grantId"), grantId}});
        return;
    }

    if (method.startsWith(QLatin1String("agentops.bin"))) {
        if (!m_agentStore) {
            fail(QStringLiteral("unavailable"));
            return;
        }
        const auto binReply = [this, now] {
            m_agentRecycled = m_agentStore->loadRecycledSessions();
            updateAgentRecycleBinBar();
            QList<agentops::BinEntry> entries;
            for (const AgentSession &session : std::as_const(m_agentRecycled)) {
                agentops::BinEntry entry;
                entry.id = session.id;
                entry.title = agentops::binTitle(session.issueNumber, session.issueTitle,
                                                 session.prompt);
                entry.owner = session.owner;
                entry.name = session.name;
                entry.branch = session.branchName;
                entry.provider = session.provider;
                entry.deletedAtMs = session.deletedAtMs;
                entries.append(entry);
            }
            return agentops::binPayload(entries, agentRecycleRetentionHours(),
                                        kAgentRecycleRetentionMinHours,
                                        kAgentRecycleRetentionMaxHours, now);
        };
        const auto named = [](const QList<int> &ids) {
            QStringList parts;
            for (const int id : ids)
                parts << QString::number(id);
            return parts.join(QStringLiteral(", "));
        };

        if (method == QLatin1String("agentops.bin")) {
            server->respond(connectionId, requestId, binReply());
            return;
        }

        if (method == QLatin1String("agentops.bin.restore")) {
            QList<int> ids;
            if (!agentops::canonicalIds(params.value(QStringLiteral("ids")).toArray(), &ids,
                                        agentops::kMaxBinAgents)) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            audit(QStringLiteral("Restoring deleted agents %1").arg(named(ids)));
            QJsonArray restored;
            QJsonArray failed;
            for (const int id : std::as_const(ids))
                (m_agentStore->restoreRecycledSession(id) ? restored : failed).append(id);
            if (!restored.isEmpty())
                reloadAgents();
            QJsonObject reply = binReply();
            reply.insert(QStringLiteral("restored"), restored);
            reply.insert(QStringLiteral("failed"), failed);
            if (!m_localControl)
                return;
            m_localControl->respond(connectionId, requestId, reply);
            notifyLocalControlAgentsChanged();
            return;
        }

        if (method == QLatin1String("agentops.bin.purge")) {
            QList<int> ids;
            if (!agentops::canonicalIds(params.value(QStringLiteral("ids")).toArray(), &ids,
                                        agentops::kMaxBinAgents)) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            const auto result = coreConfirmed ? hostctl::ConfirmationStore::Result::Ok : state->confirmations.consume(
                params.value(QStringLiteral("token")).toString(), holder,
                QStringLiteral("purge"), agentops::bulkTarget(QStringLiteral("purge"), ids), now);
            if (result != hostctl::ConfirmationStore::Result::Ok) {
                fail(confirmationErrorCode(result));
                return;
            }
            audit(QStringLiteral("Permanently deleting deleted agents %1").arg(named(ids)));
            QJsonArray purged;
            QJsonArray skipped;
            for (const int id : std::as_const(ids))
                (m_agentStore->purgeRecycledSession(id) ? purged : skipped).append(id);
            QJsonObject reply = binReply();
            reply.insert(QStringLiteral("purged"), purged);
            reply.insert(QStringLiteral("skipped"), skipped);
            server->respond(connectionId, requestId, reply);
            return;
        }

        if (method == QLatin1String("agentops.bin.retention")) {
            const QJsonValue raw = params.value(QStringLiteral("hours"));
            const double hours = raw.toDouble(-1);
            if (!raw.isDouble() || hours != std::floor(hours) ||
                hours < kAgentRecycleRetentionMinHours || hours > kAgentRecycleRetentionMaxHours) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            audit(QStringLiteral("Keeping deleted agents for %1 hours").arg(int(hours)));
            QSettings().setValue(kAgentRecycleRetentionSetting, int(hours));
            purgeExpiredAgentRecycleBin();
            server->respond(connectionId, requestId, binReply());
            return;
        }
    }

    fail(QStringLiteral("unsupported"));
}

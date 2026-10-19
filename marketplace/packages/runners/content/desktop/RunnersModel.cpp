#include "RunnersModel.h"

#include <QJsonDocument>
#include <QUrl>

#include <algorithm>

namespace forkmesh::runnerspage {
namespace {

using forkmesh::runners::RunnerAccountInfo;
using forkmesh::runners::RunnerInfo;
using forkmesh::runners::RunnerProviderState;

const QString kApiPrefix = QStringLiteral("/api/runners");

QString dot()
{
    return QString::fromUtf8(" \xC2\xB7 ");
}

QString ellipsis()
{
    return QString::fromUtf8("\xE2\x80\xA6");
}

}

bool runnerOnline(const RunnerInfo &runner, qint64 nowMs)
{
    return runner.status != QLatin1String("revoked") && runner.lastSeenAtMs > 0 &&
           nowMs - runner.lastSeenAtMs <= kOnlineWindowMs;
}

bool runnerUsable(const RunnerInfo &runner)
{
    return runner.status == QLatin1String("active") ||
           runner.status == QLatin1String("paused");
}

QString providerTitle(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("Claude Code");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("Codex");
    return forkmesh::runners::runnerProviderLabel(provider);
}

QString runnerStatusLabel(const QString &status)
{
    if (status == QLatin1String("paused"))
        return QStringLiteral("Paused");
    if (status == QLatin1String("revoked"))
        return QStringLiteral("Revoked");
    if (status.isEmpty() || status == QLatin1String("active"))
        return {};
    QString text = status;
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text[0] = text.at(0).toUpper();
    return text;
}

QString runnerStatusTone(const QString &status)
{
    if (status == QLatin1String("paused"))
        return QStringLiteral("warning");
    return QStringLiteral("neutral");
}

QString scopeLabel(const QString &scope)
{
    if (scope == QLatin1String("pool"))
        return QStringLiteral("Pool");
    if (scope == QLatin1String("personal"))
        return QStringLiteral("Personal");
    return scope;
}

QString jobsText(int activeLeases)
{
    if (activeLeases < 0)
        return {};
    if (activeLeases == 0)
        return QStringLiteral("No active jobs");
    if (activeLeases == 1)
        return QStringLiteral("1 active job");
    return QStringLiteral("%1 active jobs").arg(activeLeases);
}

QString lastSeenText(qint64 lastSeenAtMs, qint64 nowMs)
{
    if (lastSeenAtMs <= 0)
        return QStringLiteral("Never seen");
    const qint64 seconds = qMax<qint64>(0, (nowMs - lastSeenAtMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("Seen just now");
    if (seconds < 3600)
        return QStringLiteral("Seen %1 min ago").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("Seen %1 h ago").arg(seconds / 3600);
    return QStringLiteral("Seen %1 d ago").arg(seconds / 86400);
}

QString runnerDetail(const RunnerInfo &runner, qint64 nowMs)
{
    QStringList parts;
    if (runner.status != QLatin1String("revoked"))
        parts.append(runnerOnline(runner, nowMs) ? QStringLiteral("Online")
                                                 : QStringLiteral("Offline"));
    parts.append(lastSeenText(runner.lastSeenAtMs, nowMs));
    const QString jobs = jobsText(runner.activeLeases);
    if (!jobs.isEmpty() && runner.status != QLatin1String("revoked"))
        parts.append(jobs);
    return parts.join(dot());
}

QString providerStateText(const RunnerProviderState &state)
{
    if (state.loginExpiring())
        return QStringLiteral("Sign-in expires soon");
    if (state.loginExpired())
        return QStringLiteral("Sign-in expired");
    return forkmesh::runners::runnerProviderStateText(state);
}

QString providerTone(const RunnerProviderState &state)
{
    if (state.loginExpired())
        return QStringLiteral("danger");
    if (state.loginExpiring())
        return QStringLiteral("warning");
    if (state.signedIn())
        return QStringLiteral("success");
    if (state.reported && state.binaryFound)
        return QStringLiteral("warning");
    return QStringLiteral("neutral");
}

bool signInOffered(const RunnerInfo &runner, const QString &provider)
{
    if (!runnerUsable(runner))
        return false;
    const RunnerProviderState state = runner.provider(provider);
    if (state.reported && !state.binaryFound)
        return false;
    return !state.signedIn() || state.loginExpired() || state.loginExpiring() ||
           !runner.openLoginId(provider).isEmpty();
}

QString signInText(const RunnerInfo &runner, const QString &provider)
{
    const QString title = providerTitle(provider);
    if (!runner.openLoginId(provider).isEmpty())
        return QStringLiteral("Open %1 sign-in").arg(title);
    return QStringLiteral("Sign in %1").arg(title);
}

bool accountsOffered(const RunnerInfo &runner, const QString &provider)
{
    return runnerUsable(runner) && !runner.provider(provider).accounts.isEmpty();
}

ProviderRow providerRow(const RunnerInfo &runner, const QString &provider)
{
    const RunnerProviderState state = runner.provider(provider);
    ProviderRow row;
    row.provider = provider;
    row.title = providerTitle(provider);
    row.state = providerStateText(state);
    row.tone = providerTone(state);
    if (const RunnerAccountInfo *active = state.activeAccount())
        row.account = active->displayName();
    row.signInOffered = signInOffered(runner, provider);
    row.signInText = signInText(runner, provider);
    row.accountsOffered = accountsOffered(runner, provider);
    for (const RunnerAccountInfo &account : state.accounts)
        row.accounts.append({account.id, account.displayName(), account.active,
                             runner.accountRequestPending(provider, account.id)});
    return row;
}

QList<RunnerAction> actionsFor(const RunnerInfo &runner)
{
    if (runner.status == QLatin1String("active"))
        return {RunnerAction::Pause, RunnerAction::Revoke};
    if (runner.status == QLatin1String("paused"))
        return {RunnerAction::Resume, RunnerAction::Revoke};
    return {};
}

QList<RunnerInfo> sortedRunners(QList<RunnerInfo> runners)
{
    std::stable_sort(runners.begin(), runners.end(),
                     [](const RunnerInfo &a, const RunnerInfo &b) {
        const bool aRevoked = a.status == QLatin1String("revoked");
        const bool bRevoked = b.status == QLatin1String("revoked");
        if (aRevoked != bRevoked)
            return bRevoked;
        return a.name < b.name;
    });
    return runners;
}

RunnerRow runnerRow(const RunnerInfo &runner, qint64 nowMs)
{
    RunnerRow row;
    row.name = runner.name;
    row.scope = scopeLabel(runner.scope);
    row.status = runner.status;
    row.statusLabel = runnerStatusLabel(runner.status);
    row.statusTone = runnerStatusTone(runner.status);
    row.detail = runnerDetail(runner, nowMs);
    row.online = runnerOnline(runner, nowMs);
    row.revoked = runner.status == QLatin1String("revoked");
    if (!row.revoked)
        for (const QString &provider : forkmesh::runners::runnerProviders())
            row.providers.append(providerRow(runner, provider));
    row.actions = actionsFor(runner);
    return row;
}

QList<RunnerRow> runnerRows(const QJsonObject &payload, qint64 nowMs)
{
    QList<RunnerRow> rows;
    for (const RunnerInfo &runner : sortedRunners(forkmesh::runners::parseRunners(payload)))
        rows.append(runnerRow(runner, nowMs));
    return rows;
}

int attentionCount(const QJsonObject &payload)
{
    return int(forkmesh::runners::parseRunnerAttention(payload).size());
}

QString actionName(RunnerAction action)
{
    switch (action) {
    case RunnerAction::Pause:
        return QStringLiteral("pause");
    case RunnerAction::Resume:
        return QStringLiteral("resume");
    case RunnerAction::Revoke:
        return QStringLiteral("revoke");
    }
    return {};
}

QString actionLabel(RunnerAction action)
{
    switch (action) {
    case RunnerAction::Pause:
        return QStringLiteral("Pause");
    case RunnerAction::Resume:
        return QStringLiteral("Resume");
    case RunnerAction::Revoke:
        return QStringLiteral("Revoke") + ellipsis();
    }
    return {};
}

QString actionProgressText(RunnerAction action, const QString &runner)
{
    switch (action) {
    case RunnerAction::Pause:
        return QStringLiteral("Pausing %1").arg(runner) + ellipsis();
    case RunnerAction::Resume:
        return QStringLiteral("Resuming %1").arg(runner) + ellipsis();
    case RunnerAction::Revoke:
        return QStringLiteral("Revoking %1").arg(runner) + ellipsis();
    }
    return {};
}

QString actionDoneText(RunnerAction action, const QString &runner)
{
    switch (action) {
    case RunnerAction::Pause:
        return QStringLiteral("%1 is paused. It finishes the jobs it holds and takes no new ones.")
            .arg(runner);
    case RunnerAction::Resume:
        return QStringLiteral("%1 takes new jobs again.").arg(runner);
    case RunnerAction::Revoke:
        return QStringLiteral("%1 is revoked.").arg(runner);
    }
    return {};
}

QString actionPath(RunnerAction action)
{
    return kApiPrefix + QLatin1Char('/') + actionName(action);
}

QJsonObject actionBody(const QString &runner)
{
    return {{QStringLiteral("name"), runner.trimmed().toLower()}};
}

QString accountsPath(const QString &runner)
{
    return forkmesh::runners::runnerAccountsUrl(QUrl(), runner).path();
}

QJsonObject accountBody(const QString &provider, const QString &account, const QString &action)
{
    return QJsonDocument::fromJson(
               forkmesh::runners::runnerAccountRequestBody(provider, account, action))
        .object();
}

QString accountDoneText(const QString &action, const QString &account, const QString &runner)
{
    if (action == QLatin1String("remove"))
        return QStringLiteral("Removing %1 from %2; the runner applies it on its next check-in.")
            .arg(account, runner);
    return QStringLiteral("Switching %2 to %1; the runner applies it on its next check-in.")
        .arg(account, runner);
}

QString addRunnerPath()
{
    return QStringLiteral("/dashboard/runners");
}

QString connectPath(const QString &runner, const QString &provider)
{
    forkmesh::runners::RunnerAttentionItem item;
    item.runner = runner.trimmed().toLower();
    item.provider = provider.trimmed().toLower();
    return forkmesh::runners::runnerConnectPageUrl(QUrl(), item).toString(QUrl::FullyEncoded);
}

QString revokeTitle(const QString &runner)
{
    return QStringLiteral("Revoke %1?").arg(runner);
}

QString revokeQuestion(const QString &runner)
{
    return QStringLiteral("%1 stops taking jobs and its key stops working. This cannot be "
                          "undone; to use the machine again, add it as a new runner.")
        .arg(runner);
}

QString removeAccountQuestion(const QString &account, const QString &runner)
{
    return QStringLiteral("Remove %1 from %2? The runner deletes its saved login; sign it in "
                          "again to use it later.")
        .arg(account, runner);
}

bool signedOutError(const QString &error)
{
    const QString text = error.trimmed().toLower();
    return text == QLatin1String("unauthorized") || text == QLatin1String("http 401") ||
           text.startsWith(QLatin1String("sign in to your forkmesh account"));
}

QString loadErrorText(const QString &error)
{
    return QStringLiteral("Runners could not load: %1")
        .arg(error.trimmed().isEmpty() ? QStringLiteral("no answer") : error.trimmed());
}

QString actionErrorText(const QString &error)
{
    const QString text = error.trimmed();
    if (text == QLatin1String("runner inactive"))
        return QStringLiteral("That runner is revoked.");
    if (text == QLatin1String("not found") || text == QLatin1String("HTTP 404"))
        return QStringLiteral("That runner is not one of yours, or it is gone.");
    return QStringLiteral("The runner could not be changed: %1")
        .arg(text.isEmpty() ? QStringLiteral("no answer") : text);
}

}

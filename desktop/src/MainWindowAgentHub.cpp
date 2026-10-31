#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "AgentHub.h"
#include "AgentIntake.h"
#include "AgentWindows.h"
#include "RunnerLogin.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

using namespace forkmesh::ui;

namespace intake = forkmesh::agentintake;
namespace hub = forkmesh::agenthub;
namespace windows = forkmesh::agentwindows;

namespace {

bool hubSessionActive(const AgentSession &session)
{
    return session.status == AgentStatus::Queued || session.status == AgentStatus::Running ||
           session.status == AgentStatus::Waiting;
}

QString hubErrorMessage(const QString &code)
{
    if (code == QLatin1String("bad_repository"))
        return QStringLiteral("repository must be owner/name.");
    if (code == QLatin1String("bad_task"))
        return QStringLiteral("prompt is required and must be at most %1 characters.")
            .arg(intake::kMaxPromptChars);
    if (code == QLatin1String("bad_title"))
        return QStringLiteral("title is required and must be short.");
    if (code == QLatin1String("bad_provider"))
        return QStringLiteral("provider is not an agent provider this desktop knows.");
    if (code == QLatin1String("bad_model"))
        return QStringLiteral("model is not a valid model id.");
    if (code == QLatin1String("bad_reason"))
        return QStringLiteral("reason must be at most %1 characters.").arg(hub::kMaxReasonChars);
    if (code == QLatin1String("bad_limit"))
        return QStringLiteral("limit must be a whole number from 1 to %1.").arg(hub::kMaxListed);
    if (code == QLatin1String("bad_agent"))
        return QStringLiteral("agent_id must be the id list_agents returned.");
    if (code == QLatin1String("bad_message"))
        return QStringLiteral("message is required and must be at most %1 characters.")
            .arg(hub::kMaxMessageChars);
    if (code == QLatin1String("bad_body"))
        return QStringLiteral("body must be at most %1 characters.").arg(hub::kMaxNotifyBodyChars);
    return QStringLiteral("The request is malformed.");
}

}

windows::Registry *MainWindow::agentWindows()
{
    if (!m_agentWindows) {
        m_agentWindows = std::make_unique<windows::Registry>(this);
        m_agentWindows->setAnswerHandler(
            [this](const QString &windowId) { answerAgentSuggestion(windowId); });
    }
    return m_agentWindows.get();
}

QString MainWindow::agentWindowLabel(int sessionId) const
{
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.id != sessionId)
            continue;
        QString label = session.issueTitle.simplified();
        if (label.isEmpty())
            label = session.name.simplified();
        if (label.size() > 60)
            label = label.left(59) + QStringLiteral("…");
        return label;
    }
    return QString();
}

QString MainWindow::agentEffectiveModeLabel(const AgentSession &session) const
{
    if (!session.mode.trimmed().isEmpty())
        return session.mode.trimmed();
    return QSettings()
        .value(kAgentModeSetting, QSettings().value(kClaudeAutoModeSetting, true).toBool()
                                      ? kClaudeAutoModeLabel
                                      : kAgentAskModeLabel)
        .toString()
        .trimmed();
}

QJsonObject MainWindow::handleAgentHubRequest(int sessionId, const intake::Request &request)
{
    if (request.method == hub::SuggestMethod)
        return suggestAgentSession(sessionId, request.params);
    if (request.method == hub::ListMethod)
        return listAgentSessionsForAgent(sessionId, request.params);
    if (request.method == hub::MessageMethod)
        return messageAgentSessionFromAgent(sessionId, request.params);
    if (request.method == hub::NotifyMethod)
        return notifyFromAgent(sessionId, request.params);
    return intake::errorReply(QStringLiteral("unknown_method"), QStringLiteral("Unknown method."));
}

QJsonObject MainWindow::suggestAgentSession(int sessionId, const QJsonObject &params)
{
    hub::SuggestRequest suggestion;
    QString code;
    if (!hub::parseSuggestRequest(params, &suggestion, &code))
        return intake::errorReply(code, hubErrorMessage(code));
    const AgentSession *caller = findAgentSession(sessionId);
    if (suggestion.owner.isEmpty()) {
        if (!caller || caller->owner.isEmpty() || caller->name.isEmpty())
            return intake::errorReply(
                QStringLiteral("bad_repository"),
                QStringLiteral("Pass repository: this agent has no repository of its own."));
        suggestion.owner = caller->owner;
        suggestion.name = caller->name;
    }
    if (!suggestion.provider.isEmpty() && !agentProviderIsKnown(suggestion.provider))
        return intake::errorReply(QStringLiteral("bad_provider"),
                                  hubErrorMessage(QStringLiteral("bad_provider")));
    QString slug;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.owner.compare(suggestion.owner, Qt::CaseInsensitive) == 0 &&
            repo.name.compare(suggestion.name, Qt::CaseInsensitive) == 0 && !repo.previewOnly &&
            !repo.localPath.trimmed().isEmpty()) {
            slug = repo.owner + QLatin1Char('/') + repo.name;
            break;
        }
    }
    if (slug.isEmpty())
        return intake::errorReply(
            QStringLiteral("no_checkout"),
            QStringLiteral("%1/%2 is not a repository this desktop has checked out.")
                .arg(suggestion.owner, suggestion.name));

    const QString label = agentWindowLabel(sessionId);
    windows::ShowRequest show;
    show.title = QStringLiteral("Start a new session?");
    show.body = hub::suggestionBody(
        suggestion, slug,
        label.isEmpty() ? QStringLiteral("agent #%1").arg(sessionId)
                        : QStringLiteral("agent #%1 (%2)").arg(sessionId).arg(label));
    show.buttons = {hub::StartButton, hub::DismissButton};
    show.width = 480;
    show.height = 360;
    QJsonObject reply = agentWindows()->show(sessionId, label, QString(), show);
    if (!reply.value(QStringLiteral("ok")).toBool())
        return reply;
    const QString windowId = reply.value(QStringLiteral("window_id")).toString();
    agentWindows()->setKind(windowId, windows::SuggestionKind);
    const QJsonObject offered{{QStringLiteral("suggestion"), QStringLiteral("offered")},
                              {QStringLiteral("repository"), slug},
                              {QStringLiteral("session_title"),
                               hub::suggestionTitle(suggestion)}};
    agentWindows()->setExtra(windowId, offered);

    for (auto it = m_agentSuggestions.begin(); it != m_agentSuggestions.end();) {
        const windows::WindowState *old = agentWindows()->state(it.key());
        if (!old || !old->open)
            it = m_agentSuggestions.erase(it);
        else
            ++it;
    }
    AgentSuggestion pending;
    pending.requester = sessionId;
    pending.create.owner = slug.section(QLatin1Char('/'), 0, 0);
    pending.create.name = slug.section(QLatin1Char('/'), 1);
    pending.create.prompt = suggestion.prompt;
    pending.create.title = suggestion.title;
    pending.create.provider = suggestion.provider;
    pending.create.model = suggestion.model;
    m_agentSuggestions.insert(windowId, pending);
    if (AgentSession *session = findAgentSession(sessionId); session && m_agentStore)
        m_agentStore->appendLog(*session,
                                QStringLiteral("==> Suggested a new session: %1\n")
                                    .arg(hub::suggestionTitle(suggestion)));
    return agentWindows()->read(sessionId, windowId);
}

void MainWindow::answerAgentSuggestion(const QString &windowId)
{
    const auto pending = m_agentSuggestions.constFind(windowId);
    if (pending == m_agentSuggestions.constEnd() || !m_agentWindows)
        return;
    const windows::WindowState *state = m_agentWindows->state(windowId);
    if (!state || !state->answered)
        return;
    const AgentSuggestion suggestion = pending.value();
    m_agentSuggestions.erase(pending);
    QJsonObject extra = state->extra;
    if (state->button != hub::StartButton) {
        extra.insert(QStringLiteral("suggestion"), QStringLiteral("dismissed"));
        m_agentWindows->setExtra(windowId, extra);
        m_agentWindows->close(suggestion.requester, windowId);
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject created;
    if (m_agentIntakeLimiter.used(now) >= intake::kRateLimitCount)
        created = intake::errorReply(
            QStringLiteral("rate_limited"),
            QStringLiteral("Agents may create at most %1 agents every %2 minutes on this "
                           "desktop.")
                .arg(intake::kRateLimitCount)
                .arg(intake::kRateWindowMs / 60000));
    else
        created = createAgentFromIntake(suggestion.requester, suggestion.create);
    if (created.value(QStringLiteral("ok")).toBool()) {
        m_agentIntakeLimiter.allow(now);
        const int id = created.value(QStringLiteral("id")).toInt();
        extra.insert(QStringLiteral("suggestion"), QStringLiteral("started"));
        extra.insert(QStringLiteral("session_id"), id);
        extra.insert(QStringLiteral("branch"), created.value(QStringLiteral("branch")));
        noteAgentSessionNotice(suggestion.requester,
                               QStringLiteral("You started agent #%1 from this agent's "
                                              "suggestion.")
                                   .arg(id),
                               false);
    } else {
        const QString message = created.value(QStringLiteral("message")).toString();
        extra.insert(QStringLiteral("suggestion"), QStringLiteral("failed"));
        extra.insert(QStringLiteral("error"), created.value(QStringLiteral("error")));
        extra.insert(QStringLiteral("message"), message);
        flashMessage(QStringLiteral("Could not start the suggested session: %1").arg(message),
                     true);
    }
    m_agentWindows->setExtra(windowId, extra);
    m_agentWindows->close(suggestion.requester, windowId);
}

QJsonObject MainWindow::listAgentSessionsForAgent(int sessionId, const QJsonObject &params)
{
    hub::ListRequest request;
    QString code;
    if (!hub::parseListRequest(params, &request, &code))
        return intake::errorReply(code, hubErrorMessage(code));
    QString senderMode;
    for (const AgentSession &s : std::as_const(m_agentSessions))
        if (s.id == sessionId)
            senderMode = agentEffectiveModeLabel(s);
    QList<const AgentSession *> sessions;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (isExternalSession(s.id) || s.deletedAtMs > 0 || s.associationOnly)
            continue;
        if (!request.includeFinished && !hubSessionActive(s) && !s.awaitingStart() &&
            s.id != sessionId)
            continue;
        sessions.append(&s);
    }
    std::sort(sessions.begin(), sessions.end(), [](const AgentSession *a, const AgentSession *b) {
        const bool aActive = hubSessionActive(*a);
        const bool bActive = hubSessionActive(*b);
        if (aActive != bActive)
            return aActive;
        return a->createdAtMs > b->createdAtMs;
    });
    const int total = sessions.size();
    if (sessions.size() > request.limit)
        sessions = sessions.mid(0, request.limit);
    const QStringList credentials = localProviderCredentialValues();
    QJsonArray rows;
    for (const AgentSession *s : std::as_const(sessions)) {
        const QString mode = agentEffectiveModeLabel(*s);
        const bool cloud = isCloudSession(s->id) || s->cloud;
        QString blocked;
        if (s->id == sessionId)
            blocked = QStringLiteral("self");
        else if (cloud)
            blocked = QStringLiteral("cloud");
        else if (s->awaitingStart())
            blocked = QStringLiteral("paused");
        else if (s->merged)
            blocked = QStringLiteral("merged");
        else if (!hub::messageAllowed(senderMode, mode))
            blocked = QStringLiteral("more_permissive");
        QJsonObject row{
            {QStringLiteral("id"), s->id},
            {QStringLiteral("title"), s->issueTitle.isEmpty() ? s->name : s->issueTitle},
            {QStringLiteral("repository"), s->owner + QLatin1Char('/') + s->name},
            {QStringLiteral("branch"), s->branchName},
            {QStringLiteral("state"), agentIntakeSessionState(s->id)},
            {QStringLiteral("provider"), s->provider},
            {QStringLiteral("model"), s->model},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("where"), cloud ? QStringLiteral("cloud") : QStringLiteral("desktop")},
            {QStringLiteral("self"), s->id == sessionId},
            {QStringLiteral("running"), isStoppableAgentSession(s->id)},
            {QStringLiteral("can_message"), blocked.isEmpty()},
            {QStringLiteral("created_at_ms"), double(s->createdAtMs)},
        };
        if (!blocked.isEmpty())
            row.insert(QStringLiteral("cannot_message"), blocked);
        if (s->prNumber > 0)
            row.insert(QStringLiteral("pull_request"), s->prNumber);
        if (!s->orgTaskId.isEmpty())
            row.insert(QStringLiteral("task"), s->orgTaskId);
        if (!s->lastError.isEmpty())
            row.insert(QStringLiteral("last_error"), s->lastError.left(300));
        rows.append(redactProviderCredentials(QJsonValue(row), credentials));
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("agents"), rows},
        {QStringLiteral("total"), total},
        {QStringLiteral("self"), sessionId},
        {QStringLiteral("running_limit"), maxRunningAgents()},
        {QStringLiteral("running"), runningAgentCount()},
    };
}

QJsonObject MainWindow::messageAgentSessionFromAgent(int sessionId, const QJsonObject &params)
{
    hub::MessageRequest request;
    QString code;
    if (!hub::parseMessageRequest(params, &request, &code))
        return intake::errorReply(code, hubErrorMessage(code));
    if (request.target == sessionId)
        return intake::errorReply(QStringLiteral("self"),
                                  QStringLiteral("An agent cannot message itself."));
    AgentSession *target = findAgentSession(request.target);
    if (!target || isExternalSession(target->id) || target->deletedAtMs > 0 ||
        target->associationOnly)
        return intake::errorReply(
            QStringLiteral("not_found"),
            QStringLiteral("No agent #%1 on this desktop.").arg(request.target));
    if (isCloudSession(target->id) || target->cloud)
        return intake::errorReply(
            QStringLiteral("cloud"),
            QStringLiteral("Agent #%1 runs in the cloud; messages reach only agents on this "
                           "desktop.")
                .arg(request.target));
    if (target->awaitingStart())
        return intake::errorReply(
            QStringLiteral("paused"),
            QStringLiteral("Agent #%1 is paused until the user starts it.").arg(request.target));
    if (target->merged)
        return intake::errorReply(
            QStringLiteral("merged"),
            QStringLiteral("Agent #%1 finished and its work was merged; suggest a new "
                           "session instead.")
                .arg(request.target));
    QString senderMode;
    QString senderTitle;
    if (const AgentSession *sender = findAgentSession(sessionId)) {
        senderMode = agentEffectiveModeLabel(*sender);
        senderTitle = sender->issueTitle.isEmpty() ? sender->name : sender->issueTitle;
    }
    const QString targetMode = agentEffectiveModeLabel(*target);
    if (!hub::messageAllowed(senderMode, targetMode))
        return intake::errorReply(
            QStringLiteral("more_permissive"),
            QStringLiteral("Agent #%1 runs in %2 mode, which allows more than this agent's %3 "
                           "mode, so this agent cannot direct it.")
                .arg(request.target)
                .arg(targetMode, senderMode.isEmpty() ? QStringLiteral("unknown") : senderMode));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_agentMessageLimiter.allow(now))
        return intake::errorReply(
            QStringLiteral("rate_limited"),
            QStringLiteral("Agents may send at most %1 messages every %2 minutes on this "
                           "desktop.")
                .arg(hub::kMessageLimit)
                .arg(hub::kMessageWindowMs / 60000));
    const bool running = isStoppableAgentSession(target->id);
    const int targetId = target->id;
    if (AgentSession *sender = findAgentSession(sessionId); sender && m_agentStore)
        m_agentStore->appendLog(*sender, QStringLiteral("==> Sent a message to agent #%1.\n")
                                             .arg(targetId));
    if (m_agentStore)
        m_agentStore->appendLog(*target, QStringLiteral("==> Message from agent #%1.\n")
                                             .arg(sessionId));
    sendPromptToAgentSession(targetId,
                             hub::messageEnvelope(sessionId, senderTitle, request.text));
    notifyLocalControlAgentsChanged();
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("agent_id"), targetId},
        {QStringLiteral("delivered"),
         running ? QStringLiteral("to_running_agent") : QStringLiteral("resumed_agent")},
        {QStringLiteral("state"), agentIntakeSessionState(targetId)},
    };
}

QJsonObject MainWindow::notifyFromAgent(int sessionId, const QJsonObject &params)
{
    hub::NotifyRequest request;
    QString code;
    if (!hub::parseNotifyRequest(params, &request, &code))
        return intake::errorReply(code, hubErrorMessage(code));
    if (!m_agentNotifyLimiter.allow(QDateTime::currentMSecsSinceEpoch()))
        return intake::errorReply(
            QStringLiteral("rate_limited"),
            QStringLiteral("Agents may send at most %1 notifications every %2 minutes on "
                           "this desktop.")
                .arg(hub::kNotifyLimit)
                .arg(hub::kNotifyWindowMs / 60000));
    const QString href = QStringLiteral("fm:agent:%1").arg(sessionId);
    const QString ping = request.body.isEmpty()
                             ? QStringLiteral("Agent #%1: %2").arg(sessionId).arg(request.title)
                             : QStringLiteral("Agent #%1: %2 — %3")
                                   .arg(sessionId)
                                   .arg(request.title, request.body.simplified().left(200));
    flashPingMessage(ping, false, href);
    notifyIfInactive(QStringLiteral("%1 — Agent #%2").arg(request.title).arg(sessionId),
                     request.body, href, agentNotificationIconPath(sessionId));
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("shown"), true},
                       {QStringLiteral("window_active"), isActiveWindow()}};
}

void MainWindow::listRunnersForAgent(int sessionId, intake::Server::Reply reply)
{
    Q_UNUSED(sessionId);
    refreshRunners([this, reply] {
        if (!m_runnersLoaded) {
            reply(intake::errorReply(
                QStringLiteral("runners_unavailable"),
                QStringLiteral("The desktop could not load the account's runners; it may "
                               "not be signed in to a ForkMesh account.")));
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QJsonArray rows;
        for (const forkmesh::runners::RunnerInfo &runner : std::as_const(m_runners)) {
            QJsonArray providers;
            for (const forkmesh::runners::RunnerProviderState &provider : runner.providers) {
                QJsonObject item{
                    {QStringLiteral("provider"), provider.provider},
                    {QStringLiteral("installed"), provider.binaryFound},
                    {QStringLiteral("signed_in"), provider.signedIn()},
                    {QStringLiteral("state"),
                     forkmesh::runners::runnerProviderStateText(provider)},
                };
                if (const forkmesh::runners::RunnerAccountInfo *account =
                        provider.activeAccount())
                    item.insert(QStringLiteral("account"), account->displayName());
                providers.append(item);
            }
            QJsonObject row{
                {QStringLiteral("name"), runner.name},
                {QStringLiteral("scope"), runner.scope},
                {QStringLiteral("status"), runner.status},
                {QStringLiteral("providers"), providers},
            };
            if (runner.lastSeenAtMs > 0)
                row.insert(QStringLiteral("last_seen_seconds_ago"),
                           double(qMax<qint64>(0, now - runner.lastSeenAtMs) / 1000));
            if (runner.activeLeases >= 0)
                row.insert(QStringLiteral("active_jobs"), runner.activeLeases);
            rows.append(row);
        }
        reply(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("runners"), rows}});
    });
}

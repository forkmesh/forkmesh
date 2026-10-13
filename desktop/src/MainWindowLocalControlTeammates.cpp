// The "teammates." local control family (WS17): the Team page's teammates.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.
// Gate 2 answers "unsupported" for every method; WS17 fills it in.

#include "LocalControl.h"
#include "LocalControlTeammates.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "TeammateStore.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

namespace matectl = forkmesh::localcontrol::teammates;
namespace hostctl = forkmesh::localcontrol::hosts;
using forkmesh::teammates::Config;
using forkmesh::teammates::LoadResult;
using forkmesh::teammates::Store;

namespace {

constexpr int kMaxTeammates = 100;
constexpr int kMaxRoomMessages = 200;
constexpr int kMaxStatsTasks = 60;
constexpr int kBriefingOutputChars = 4000;

}

QString MainWindow::localControlTeammateRepoKey(int repoIndex) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    return repo.owner + QLatin1Char('/') + repo.name;
}

QString MainWindow::localControlTeammateStatus(const Config &config) const
{
    const AgentSession *session = nullptr;
    if (config.lastSessionId > 0) {
        for (const AgentSession &candidate : m_agentSessions)
            if (candidate.id == config.lastSessionId) {
                session = &candidate;
                break;
            }
    }
    const bool nativeInFlight =
        config.workflow == forkmesh::teammates::kWorkflowContributorPayouts &&
        m_contributorPayoutRunActive && m_contributorPayoutTeammateId == config.id;
    return matectl::statusFor(config, session ? session->status : QString(), nativeInFlight);
}

QJsonObject MainWindow::localControlTeammatesPayload()
{
    const int repoIndex = activeTeammateRepoIndex();
    const QString directory = teammatesDirPath(repoIndex);
    QJsonArray repos;
    for (int index : teammateRepoChoices()) {
        const QString key = localControlTeammateRepoKey(index);
        if (!key.isEmpty())
            repos.append(key);
    }
    QJsonObject payload{{QStringLiteral("repo"), localControlTeammateRepoKey(repoIndex)},
                        {QStringLiteral("repos"), repos},
                        {QStringLiteral("available"), !directory.isEmpty()}};
    if (directory.isEmpty()) {
        payload.insert(QStringLiteral("teammates"), QJsonArray());
        return payload;
    }
    const LoadResult loaded = Store(directory).load();
    const QDateTime now = QDateTime::currentDateTime();
    QList<Config> configs;
    for (const Config &saved : loaded.teammates)
        configs.append(teammateWithRuntime(saved, repoIndex));
    const QHash<QString, forkmesh::teammates::Mood> moods =
        assessTeammateMoods(configs, now.toMSecsSinceEpoch());
    QJsonArray rows;
    int working = 0;
    int scheduled = 0;
    int idle = 0;
    for (const Config &config : std::as_const(configs)) {
        const QString status = localControlTeammateStatus(config);
        if (matectl::statusIsWorking(status))
            ++working;
        else if (status == QLatin1String("Scheduled"))
            ++scheduled;
        else
            ++idle;
        if (rows.size() < kMaxTeammates) {
            QJsonObject row = matectl::teammateRow(config, status, now);
            const auto mood = moods.constFind(config.id);
            if (mood != moods.constEnd())
                row.insert(QStringLiteral("mood"), forkmesh::teammates::moodJson(mood.value()));
            rows.append(row);
        }
    }
    QJsonArray errors;
    for (const QString &error : loaded.errors.mid(0, 20))
        errors.append(error.left(300));
    payload.insert(QStringLiteral("teammates"), rows);
    payload.insert(QStringLiteral("errors"), errors);
    payload.insert(QStringLiteral("summary"),
                   QJsonObject{{QStringLiteral("total"), int(loaded.teammates.size())},
                               {QStringLiteral("working"), working},
                               {QStringLiteral("scheduled"), scheduled},
                               {QStringLiteral("idle"), idle}});
    return payload;
}

bool MainWindow::localControlFindTeammate(const QString &id, Config *config)
{
    const QString directory = teammatesDirPath();
    if (directory.isEmpty())
        return false;
    const LoadResult loaded = Store(directory).load();
    for (const Config &saved : loaded.teammates) {
        if (saved.id == id) {
            if (config)
                *config = saved;
            return true;
        }
    }
    return false;
}

void MainWindow::handleLocalControlTeammatesRequest(int connectionId, int requestId,
                                                    const QString &method,
                                                    const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString id = params.value(QStringLiteral("id")).toString().trimmed();

    if (method == QLatin1String("teammates.list")) {
        server->respond(connectionId, requestId, localControlTeammatesPayload());
        return;
    }
    if (method == QLatin1String("teammates.get")) {
        Config saved;
        if (!localControlFindTeammate(id, &saved)) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_teammate"));
            return;
        }
        const Config config = teammateWithRuntime(saved);
        const AgentSession *session = ownTeammateSession(config);
        server->respond(connectionId, requestId,
                        matectl::teammateDetail(config, localControlTeammateStatus(config),
                                                teammateLatestOutput(session),
                                                QDateTime::currentDateTime()));
        return;
    }
    if (method == QLatin1String("teammates.options")) {
        QJsonArray taskTypes;
        for (const QString &type : forkmesh::teammates::knownTaskTypes())
            taskTypes.append(QJsonObject{{QStringLiteral("id"), type},
                                         {QStringLiteral("label"),
                                          forkmesh::teammates::taskTypeLabel(type)}});
        server->respond(
            connectionId, requestId,
            {{QStringLiteral("providers"),
              QJsonArray{QStringLiteral("codex"), QStringLiteral("claude-code"),
                         QStringLiteral("openai"), QStringLiteral("claude-api")}},
             {QStringLiteral("modes"),
              QJsonArray{QString(), QStringLiteral("Auto"), QStringLiteral("Plan"),
                         QStringLiteral("Ask")}},
             {QStringLiteral("scheduleModes"),
              QJsonArray{forkmesh::teammates::kScheduleOnDemand,
                         forkmesh::teammates::kScheduleContinuous,
                         forkmesh::teammates::kScheduleHourly, forkmesh::teammates::kScheduleDaily,
                         forkmesh::teammates::kScheduleWeekly,
                         forkmesh::teammates::kScheduleWeekdays,
                         forkmesh::teammates::kScheduleCustom,
                         forkmesh::teammates::kScheduleEvents}},
             {QStringLiteral("taskTypes"), taskTypes},
             {QStringLiteral("editable"), QJsonArray::fromStringList(matectl::editableFields())}});
        return;
    }
    if (method == QLatin1String("teammates.briefing")) {
        const int repoIndex = activeTeammateRepoIndex();
        const QString directory = teammatesDirPath(repoIndex);
        if (directory.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("no_repository"));
            return;
        }
        const QString repoKey = localControlTeammateRepoKey(repoIndex);
        const QString owner = repoKey.section(QLatin1Char('/'), 0, 0);
        const QString name = repoKey.section(QLatin1Char('/'), 1);
        const QDate today = QDate::currentDate();
        QJsonArray entries;
        int updates = 0;
        int completed = 0;
        int inProgress = 0;
        for (const Config &saved : Store(directory).load().teammates) {
            const Config config = teammateWithRuntime(saved, repoIndex);
            const AgentSession *daily = nullptr;
            qint64 newest = 0;
            for (const AgentSession &session : m_agentSessions) {
                if ((session.teammateId != config.id && !session.teammateIds.contains(config.id)) ||
                    session.owner != owner || session.name != name)
                    continue;
                const qint64 stamp = session.finishedAtMs > 0 ? session.finishedAtMs
                                     : session.startedAtMs > 0 ? session.startedAtMs
                                                               : session.createdAtMs;
                if (stamp <= 0 || QDateTime::fromMSecsSinceEpoch(stamp).date() != today ||
                    stamp < newest)
                    continue;
                newest = stamp;
                daily = &session;
            }
            if (daily) {
                ++updates;
                if (daily->status == AgentStatus::Success)
                    ++completed;
                else if (daily->status == AgentStatus::Running ||
                         daily->status == AgentStatus::Queued ||
                         daily->status == AgentStatus::Waiting)
                    ++inProgress;
            }
            entries.append(QJsonObject{
                {QStringLiteral("id"), config.id},
                {QStringLiteral("name"), config.name},
                {QStringLiteral("avatar"), config.avatar},
                {QStringLiteral("status"), localControlTeammateStatus(config)},
                {QStringLiteral("sessionId"), daily ? daily->id : 0},
                {QStringLiteral("sessionStatus"), daily ? daily->status : QString()},
                {QStringLiteral("updatedAtMs"), double(newest)},
                {QStringLiteral("output"),
                 daily ? matectl::boundedOutput(teammateLatestOutput(daily), kBriefingOutputChars)
                       : QString()}});
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("repo"), repoKey},
                         {QStringLiteral("date"), today.toString(Qt::ISODate)},
                         {QStringLiteral("updates"), updates},
                         {QStringLiteral("completed"), completed},
                         {QStringLiteral("inProgress"), inProgress},
                         {QStringLiteral("entries"), entries}});
        return;
    }
    if (method == QLatin1String("teammates.stats")) {
        const int repoIndex = activeTeammateRepoIndex();
        const QString directory = teammatesDirPath(repoIndex);
        if (directory.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("no_repository"));
            return;
        }
        const QString repoKey = localControlTeammateRepoKey(repoIndex);
        QList<matectl::TaskRecord> tasks;
        for (const AgentSession &session : m_agentSessions) {
            if (!session.hasTeammateAttribution() ||
                session.owner + QLatin1Char('/') + session.name != repoKey)
                continue;
            matectl::TaskRecord task;
            task.session = session.id;
            task.teammateId = session.teammateId;
            task.teammateIds = session.teammateIds;
            task.title = session.issueTitle.trimmed().isEmpty()
                             ? session.prompt.section(QLatin1Char('\n'), 0, 0).trimmed()
                             : session.issueTitle.trimmed();
            task.status = session.status;
            task.grade = session.taskGrade;
            task.finishedAtMs = session.finishedAtMs > 0 ? session.finishedAtMs : session.createdAtMs;
            task.costUsd = session.costUsd;
            task.repo = repoKey;
            tasks.append(task);
        }
        QList<Config> teammates;
        for (const Config &saved : Store(directory).load().teammates)
            teammates.append(saved);
        QJsonObject stats = matectl::teamStats(teammates, tasks, kMaxStatsTasks);
        stats.insert(QStringLiteral("repo"), repoKey);
        server->respond(connectionId, requestId, stats);
        return;
    }
    if (method == QLatin1String("teammates.room")) {
        const int limit = qBound(1, params.value(QStringLiteral("limit")).toInt(100),
                                 kMaxRoomMessages);
        const QList<ChatMessage> history = m_history.value(QStringLiteral("#team"));
        QJsonArray messages;
        for (qsizetype i = qMax<qsizetype>(0, history.size() - limit); i < history.size(); ++i) {
            const ChatMessage &message = history.at(i);
            if (message.deleted)
                continue;
            messages.append(matectl::roomMessage(message.id, message.senderName, message.text,
                                                 message.timestampMs, message.self,
                                                 message.threadRootId));
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("conversation"), QStringLiteral("#team")},
                         {QStringLiteral("messages"), messages}});
        return;
    }
    if (method == QLatin1String("teammates.selectRepo")) {
        const QString repo = params.value(QStringLiteral("repo")).toString();
        for (int index : teammateRepoChoices()) {
            if (localControlTeammateRepoKey(index).compare(repo, Qt::CaseInsensitive) == 0) {
                setActiveTeammateRepoIndex(index);
                server->respond(connectionId, requestId, localControlTeammatesPayload());
                server->broadcast(QStringLiteral("teammates.changed"));
                return;
            }
        }
        server->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
        return;
    }
    if (method == QLatin1String("teammates.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = matectl::subjectFor(action, params, &error);
        if (subject.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        if (teammatesDirPath().isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("no_repository"));
            return;
        }
        QString name;
        if (action == QLatin1String("create")) {
            name = params.value(QStringLiteral("changes")).toObject()
                       .value(QStringLiteral("name")).toString().trimmed();
        } else if (!id.isEmpty()) {
            Config saved;
            if (!localControlFindTeammate(id, &saved)) {
                server->respondError(connectionId, requestId, QStringLiteral("unknown_teammate"));
                return;
            }
            name = saved.name;
        }
        const QString token = m_teammateControlConfirmations.issue(
            device.key, action, subject, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("summary"), matectl::actionSummary(action, subject, name)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("teammates.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = matectl::subjectFor(action, params, &error);
        const hostctl::ConfirmationStore::Result verdict = m_teammateControlConfirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, action, subject,
            QDateTime::currentMSecsSinceEpoch());
        if (subject.isEmpty() || verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = subject.isEmpty() ? error : hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused a special agent %1 from %2: %3.")
                          .arg(action, localControlDeviceLabel(connectionId), why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        runLocalControlTeammateAction(connectionId, requestId, action, subject, params);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::runLocalControlTeammateAction(int connectionId, int requestId,
                                               const QString &action, const QString &subject,
                                               const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString who = localControlDeviceLabel(connectionId);
    const QString id = params.value(QStringLiteral("id")).toString().trimmed();
    const QString directory = teammatesDirPath();
    if (directory.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("no_repository"));
        return;
    }
    Config saved;
    const bool needsTeammate = action != QLatin1String("runTeam") &&
                               action != QLatin1String("disableSchedules") &&
                               action != QLatin1String("grade") &&
                               action != QLatin1String("create") &&
                               !(action == QLatin1String("briefing") && id.isEmpty());
    if (needsTeammate && !localControlFindTeammate(id, &saved)) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_teammate"));
        return;
    }
    const auto done = [this, server, connectionId, requestId, action, subject,
                       who](const QString &message) {
        logSystem(QStringLiteral("Special agent %1 (%2) from %3.").arg(action, subject, who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("message"), message}});
        server->broadcast(QStringLiteral("teammates.changed"));
    };

    if (action == QLatin1String("runTeam")) {
        runTeammateTeamNow();
        done(QStringLiteral("The special agents are running."));
        return;
    }
    if (action == QLatin1String("run")) {
        const int session = runTeammate(id);
        if (session <= 0) {
            server->respondError(connectionId, requestId, QStringLiteral("not_started"));
            return;
        }
        done(QStringLiteral("%1 is running · session #%2").arg(saved.name).arg(session));
        return;
    }
    if (action == QLatin1String("briefing")) {
        generateTeammateBriefing(id);
        done(id.isEmpty() ? QStringLiteral("Generating today's briefing.")
                          : QStringLiteral("Refreshing %1's briefing.").arg(saved.name));
        return;
    }
    if (action == QLatin1String("enableSchedule")) {
        enableTeammateSchedule(id);
        done(QStringLiteral("%1's schedule is on.").arg(saved.name));
        return;
    }
    if (action == QLatin1String("disableSchedules")) {
        disableAllTeammateSchedules();
        done(QStringLiteral("Every automatic schedule is paused."));
        return;
    }
    if (action == QLatin1String("grade")) {
        const int session = subject.section(QLatin1Char(':'), 0, 0).toInt();
        const int grade = subject.section(QLatin1Char(':'), 1, 1).toInt();
        if (!setTeammateTaskGrade(session, grade)) {
            server->respondError(connectionId, requestId, QStringLiteral("not_gradable"));
            return;
        }
        done(grade > 0 ? QStringLiteral("Graded %1 out of 5.").arg(grade)
                       : QStringLiteral("Grade cleared."));
        return;
    }
    if (action == QLatin1String("prompt")) {
        if (!saved.workflow.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("native_workflow"));
            return;
        }
        sendTeammatePrompt(id, params.value(QStringLiteral("text")).toString());
        done(QStringLiteral("Sent to %1.").arg(saved.name));
        return;
    }
    if (action == QLatin1String("stop")) {
        stopTeammate(id);
        done(QStringLiteral("Stopping %1.").arg(saved.name));
        return;
    }
    if (action == QLatin1String("continue")) {
        QString text = params.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty())
            text = QStringLiteral("Continue.");
        if (!continueTeammateSession(teammateWithRuntime(saved), QStringLiteral("From the phone"),
                                     text)) {
            server->respondError(connectionId, requestId, QStringLiteral("no_session"));
            return;
        }
        loadTeammatesPanel();
        done(QStringLiteral("%1 continues.").arg(saved.name));
        return;
    }
    if (action == QLatin1String("edit") || action == QLatin1String("create")) {
        const QJsonObject changes = params.value(QStringLiteral("changes")).toObject();
        Config config;
        if (action == QLatin1String("edit")) {
            config = saved;
        } else {
            const QString newId = Store::safeId(changes.value(QStringLiteral("name")).toString());
            if (newId.isEmpty() || localControlFindTeammate(newId, nullptr)) {
                server->respondError(connectionId, requestId, QStringLiteral("teammate_exists"));
                return;
            }
            config.id = newId;
            config.createdAtMs = QDateTime::currentMSecsSinceEpoch();
        }
        QString error;
        if (!matectl::applyEdit(&config, changes, &error)) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        config.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
        if (!Store(directory).save(&config, &error)) {
            server->respondError(connectionId, requestId,
                                 error.isEmpty() ? QStringLiteral("save_failed") : error.left(300));
            return;
        }
        loadTeammatesPanel();
        done(action == QLatin1String("edit") ? QStringLiteral("Saved %1.").arg(config.name)
                                             : QStringLiteral("Added %1.").arg(config.name));
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
}

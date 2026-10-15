#include "ActionRunner.h"
#include "ActionStore.h"
#include "AgentResources.h"
#include "CoreActions.h"
#include "CoreClient.h"
#include "CoreGit.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MirrorActionsConfiguration.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QTimer>

using namespace forkmesh::ui;

namespace {

constexpr qint64 kCoreQueueCallTimeoutMs = 150000;
constexpr int kCoreLostGraceMs = 20000;
const QSet<QString> kDirectRunEvents{
    QStringLiteral("actions.log"),      QStringLiteral("actions.step"),
    QStringLiteral("actions.status"),   QStringLiteral("actions.release"),
    QStringLiteral("actions.finished"), QStringLiteral("actions.interrupted"),
};

QJsonArray idArray(const QList<int> &ids)
{
    QJsonArray out;
    for (int id : ids)
        out.append(id);
    return out;
}

}

bool MainWindow::coreActionQueueActive() const
{
    return m_coreActionQueueAttached && m_coreClient && m_coreClient->isConnected();
}

QJsonObject MainWindow::coreActionQueueSnapshot() const
{
    QJsonArray repos;
    for (const RepositoryRecord &repo : m_repositories) {
        repos.append(QJsonObject{
            {QStringLiteral("owner"), repo.owner},
            {QStringLiteral("name"), repo.name},
            {QStringLiteral("description"), repo.description.left(300)},
            {QStringLiteral("mirrorPath"), repo.mirrorPath},
            {QStringLiteral("localPath"), repo.localPath},
            {QStringLiteral("writablePath"), writableRecordFor(repo).localPath},
            {QStringLiteral("preview"), repo.previewOnly},
            {QStringLiteral("relay"), repositoryBelongsToActiveRelay(repo)},
            {QStringLiteral("enabled"), repo.actionsEnabled},
            {QStringLiteral("autoApprove"), repo.actionsAutoApprove},
            {QStringLiteral("externallyManaged"), repo.externallyManagedActions},
            {QStringLiteral("externalSource"), repo.externalActionsSource},
            {QStringLiteral("externalRef"), repo.externalActionsRef},
            {QStringLiteral("disabledWorkflows"), QJsonArray::fromStringList(repo.disabledWorkflows)},
            {QStringLiteral("workflowNodes"), QJsonArray::fromStringList(repo.workflowNodes)},
            {QStringLiteral("pinned"), pinActionMirror(repo, nullptr) != nullptr},
            {QStringLiteral("private"), repo.isPrivate},
            {QStringLiteral("published"), repo.publishToNetwork},
            {QStringLiteral("lastSyncMs"), double(repo.lastSyncMs)},
        });
    }
    QList<int> native;
    for (const ActionRunner *runner : m_actionRunners) {
        if (runner->busy() && !forkmesh::coreactions::follows(runner, runner->currentRunId()))
            native.append(runner->currentRunId());
    }
    return {
        {QStringLiteral("repos"), repos},
        {QStringLiteral("nodeLabels"), QJsonArray::fromStringList(actionNodeLabels())},
        {QStringLiteral("machine"), machineNodeName()},
        {QStringLiteral("slice"), forkmesh::agentresources::status().available
                                      ? forkmesh::agentresources::sliceName()
                                      : QString()},
        {QStringLiteral("mirrorGeneration"), m_mirrorActionsConfigGeneration},
        {QStringLiteral("mirrorSuspended"),
         forkmesh::mirror_actions::actionsSuspendedByRecoveryFailure()},
        {QStringLiteral("native"), idArray(native)},
    };
}

void MainWindow::pushCoreActionQueueSnapshot(bool force)
{
    if (!coreActionQueueActive())
        return;
    const QJsonObject snapshot = coreActionQueueSnapshot();
    const QByteArray fingerprint = QCryptographicHash::hash(
        QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (!force && fingerprint == m_coreActionQueueFingerprint)
        return;
    m_coreActionQueueFingerprint = fingerprint;
    m_coreClient->request(QStringLiteral("core.actions.queue.repos"), snapshot);
}

void MainWindow::attachCoreActionQueue()
{
    if (!m_coreClient || !m_coreClient->isConnected() ||
        !m_coreClient->hasNativeRoute(QStringLiteral("core.actions.queue.attach")))
        return;
    const QJsonObject snapshot = coreActionQueueSnapshot();
    const QByteArray fingerprint = QCryptographicHash::hash(
        QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    QPointer<MainWindow> self(this);
    m_coreClient->request(
        QStringLiteral("core.actions.queue.attach"), snapshot,
        [self, fingerprint](bool ok, const QJsonValue &result, const QString &error) {
            if (!self)
                return;
            if (!ok) {
                self->logSystem(QStringLiteral("Actions: forkmesh-core did not take the run queue "
                                               "(%1); this desktop keeps running it.")
                                    .arg(error));
                return;
            }
            const QJsonObject reply = result.toObject();
            self->m_coreActionQueueAttached = true;
            self->m_coreActionQueueFingerprint = fingerprint;
            self->m_actionQueue.clear();
            self->m_actionWaitingRuns.clear();
            if (self->m_actionStore) {
                for (const QJsonValue &value : reply.value(QStringLiteral("runs")).toArray())
                    self->m_actionStore->adoptRun(ActionRun::fromJson(value.toObject()));
                self->m_actionRuns = self->m_actionStore->loadAllRuns();
            }
            self->onCoreActionQueueEvent(QStringLiteral("actions.queue"), reply);
            self->logSystem(QStringLiteral("Actions: the run queue is in forkmesh-core (pid %1).")
                                .arg(reply.value(QStringLiteral("pid")).toInteger()));
            self->reconcileCoreActionRuns();
            self->refreshActionsTable();
            self->updateNotificationButton();
        });
}

bool MainWindow::coreActionQueueCall(const QString &method, const QJsonObject &params,
                                     QJsonObject *result, QString *error)
{
    QString failure;
    const bool ok = forkmesh::coregit::callCore(method, params, kCoreQueueCallTimeoutMs, result,
                                                &failure);
    if (error)
        *error = failure;
    return ok;
}

void MainWindow::coreActionQueueRequest(const QString &method, const QJsonObject &params)
{
    if (!m_coreClient)
        return;
    QPointer<MainWindow> self(this);
    m_coreClient->request(method, params,
                          [self, method](bool ok, const QJsonValue &, const QString &error) {
                              if (self && !ok && !error.isEmpty())
                                  self->flashMessage(error, true);
                          });
}

void MainWindow::onCoreActionQueueEvent(const QString &event, const QJsonObject &data)
{
    if (!event.startsWith(QLatin1String("actions.")) || !m_actionStore)
        return;
    const int directId = data.value(QStringLiteral("run")).toInt();
    if (directId > 0 && kDirectRunEvents.contains(event) &&
        forkmesh::coreactions::followsDirect(directId)) {
        const ActionRun record =
            ActionRun::fromJson(data.value(QStringLiteral("record")).toObject());
        if (record.id == directId)
            m_actionStore->adoptRun(record);
        if (event == QLatin1String("actions.log")) {
            onRunLog(directId, data.value(QStringLiteral("text")).toString());
        } else if (event == QLatin1String("actions.step")) {
            onRunStepChanged(directId, data.value(QStringLiteral("step")).toInt(),
                             data.value(QStringLiteral("status")).toString());
        } else if (event == QLatin1String("actions.status")) {
            const QString status = data.value(QStringLiteral("status")).toString();
            if (status == ActionStatus::Running || status == ActionStatus::Queued)
                onRunStatusChanged(directId, status);
        } else if (event == QLatin1String("actions.release")) {
            if (data.value(QStringLiteral("phase")).toString() == QLatin1String("prepared"))
                onReleaseMetadataPrepared(directId);
        } else if (event == QLatin1String("actions.finished")) {
            forkmesh::coreactions::dropDirect(directId);
            forkmesh::coreactions::markFinishHandled(directId);
            forkmesh::coreactions::noteFinished(data);
            if (m_coreClient)
                m_coreClient->request(QStringLiteral("core.actions.ack"),
                                      {{QStringLiteral("runs"), QJsonArray{directId}}});
            QPointer<MainWindow> self(this);
            onRunStatusChanged(directId, record.id == directId ? record.status : ActionStatus::Failed);
            if (!self)
                return;
            onRunFinished(directId, data.value(QStringLiteral("ok")).toBool());
            if (self && data.value(QStringLiteral("landed")).toBool())
                onReleaseMetadataLanded(directId);
        } else if (event == QLatin1String("actions.interrupted")) {
            forkmesh::coreactions::dropDirect(directId);
            onRunInterrupted(directId);
        }
        return;
    }
    if (!m_coreActionQueueAttached)
        return;
    if (event == QLatin1String("actions.queue")) {
        m_coreActionQueueReasons.clear();
        const QJsonObject reasons = data.value(QStringLiteral("reasons")).toObject();
        for (auto it = reasons.constBegin(); it != reasons.constEnd(); ++it)
            m_coreActionQueueReasons.insert(it.key().toInt(), it.value().toString());
        refreshQueuedRunReasons();
        return;
    }
    if (event == QLatin1String("actions.started")) {
        const ActionRun record =
            ActionRun::fromJson(data.value(QStringLiteral("run")).toObject());
        for (const ActionRunner *runner : std::as_const(m_actionRunners))
            if (forkmesh::coreactions::follows(runner, record.id))
                return;
        if (record.id <= 0)
            return;
        forkmesh::coreactions::followDirect(record.id);
        m_actionStore->adoptRun(record);
        m_actionRuns = m_actionStore->loadAllRuns();
        refreshActionsTable();
        return;
    }
    if (event == QLatin1String("actions.run")) {
        const ActionRun record =
            ActionRun::fromJson(data.value(QStringLiteral("record")).toObject());
        if (record.id <= 0)
            return;
        m_actionStore->adoptRun(record);
        m_actionRuns = m_actionStore->loadAllRuns();
        refreshActionsTable();
        refreshCommitStatusGlyphs();
        updateNotificationButton();
        refreshOpenPullChecks();
        if (record.id == m_selectedRunId)
            showRun(record.id);
        else
            refreshParallelRunColumn(record.id);
        if (data.value(QStringLiteral("created")).toBool() && m_actionWorkflowList &&
            repoIndexFor(record.owner, record.name) == m_repoDetailIndex)
            refreshRepoActions();
        const QJsonObject ping = data.value(QStringLiteral("ping")).toObject();
        if (ping.value(QStringLiteral("event")).toString() == QLatin1String("approval"))
            notifyActionEvent(ActionPingEvent::Approval,
                              ping.value(QStringLiteral("title")).toString(),
                              ping.value(QStringLiteral("body")).toString(), false, record.id);
        return;
    }
    if (event == QLatin1String("actions.deleted")) {
        QList<ActionRun> doomed;
        QSet<int> ids;
        for (const QJsonValue &value : data.value(QStringLiteral("runs")).toArray()) {
            const ActionRun run = ActionRun::fromJson(value.toObject());
            if (run.id > 0) {
                doomed.append(run);
                ids.insert(run.id);
            }
        }
        if (doomed.isEmpty())
            return;
        m_actionStore->deleteRunsInBackground(doomed);
        m_actionRuns = m_actionStore->loadAllRuns();
        if (ids.contains(m_selectedRunId))
            showRun(-1);
        QList<int> kept = parallelRunIds();
        kept.removeIf([&](int id) { return ids.contains(id); });
        if (kept.size() != parallelRunIds().size())
            setParallelRunIds(kept);
        refreshActionsTable();
        refreshCommitStatusGlyphs();
        refreshOpenPullChecks();
        updateNotificationButton();
        return;
    }
    if (event == QLatin1String("actions.push")) {
        const QString owner = data.value(QStringLiteral("owner")).toString();
        const QString name = data.value(QStringLiteral("name")).toString();
        const QString branch = data.value(QStringLiteral("branch")).toString();
        const QString commit = data.value(QStringLiteral("commit")).toString();
        const QString subject = data.value(QStringLiteral("subject")).toString();
        if (QSettings().value(kPushAlertSetting, false).toBool()) {
            postNotification(QStringLiteral("Push received"),
                             QString::fromUtf8("%1/%2 \xC2\xB7 %3 \xC2\xB7 %4%5")
                                 .arg(owner, name, branch, commit.left(8),
                                      subject.isEmpty() ? QString()
                                                        : QStringLiteral("\n") + subject),
                             false, QStringLiteral("emblem-synchronizing"));
        }
        if (repoIndexFor(owner, name) == m_repoDetailIndex && m_repoDetailIndex >= 0)
            scheduleOpenRepoDetailRefresh();
        return;
    }
    if (event == QLatin1String("actions.pushed")) {
        const int index = repoIndexFor(data.value(QStringLiteral("owner")).toString(),
                                       data.value(QStringLiteral("name")).toString());
        if (index >= 0)
            pushToSshMirrorRemotes(index);
        return;
    }
    if (event == QLatin1String("actions.stop")) {
        if (ActionRunner *runner = runnerForRun(data.value(QStringLiteral("run")).toInt()))
            runner->stop();
    }
}

void MainWindow::registerCoreActionQueueHandlers()
{
    if (!m_coreClient)
        return;
    connect(m_coreClient, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) {
                onCoreActionQueueEvent(event, data);
            });
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this](bool connected) {
        if (connected) {
            attachCoreActionQueue();
            return;
        }
        if (!forkmesh::coreactions::directRuns().isEmpty()) {
            QTimer::singleShot(kCoreLostGraceMs, this, [this] {
                if (m_coreClient && m_coreClient->isConnected())
                    return;
                for (int id : forkmesh::coreactions::directRuns()) {
                    forkmesh::coreactions::dropDirect(id);
                    onRunInterrupted(id);
                }
            });
        }
        if (!m_coreActionQueueAttached)
            return;
        m_coreActionQueueAttached = false;
        m_coreActionQueueReasons.clear();
        m_coreActionQueueFingerprint.clear();
        logSystem(QStringLiteral("Actions: forkmesh-core went away; this desktop runs the queue "
                                 "until it is back."));
        processActionQueue();
    });
    m_coreClient->handle(
        QStringLiteral("ui.actions.pin"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const int runId = params.value(QStringLiteral("run")).toInt();
            const int index = repoIndexFor(params.value(QStringLiteral("owner")).toString(),
                                           params.value(QStringLiteral("name")).toString());
            if (index < 0) {
                respond({}, QStringLiteral("unknown_repo"));
                return;
            }
            const RepositoryRecord &repo = m_repositories.at(index);
            QString path;
            const std::shared_ptr<void> pin = pinActionMirror(repo, &path);
            if (!pin) {
                respond({}, QStringLiteral("no_pin"));
                return;
            }
            releaseActionMirrorPin(runId);
            m_actionMirrorPins.insert(runId, {pin, path, repo.owner, repo.name});
            respond({{QStringLiteral("path"), path}}, QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.actions.autoFix"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const QString owner = params.value(QStringLiteral("owner")).toString();
            const QString name = params.value(QStringLiteral("name")).toString();
            const QString branch = params.value(QStringLiteral("branch")).toString();
            int sessionId = 0;
            for (const AgentSession &s : std::as_const(m_agentSessions)) {
                if (s.owner == owner && s.name == name && s.branchName == branch &&
                    !isExternalSession(s.id))
                    sessionId = s.id;
            }
            const AgentSession *session = sessionId > 0 ? findAgentSession(sessionId) : nullptr;
            if (branch.isEmpty() || !session || session->status == AgentStatus::Running ||
                session->status == AgentStatus::Queued ||
                session->status == AgentStatus::Waiting) {
                respond({{QStringLiteral("sent"), false}}, QString());
                return;
            }
            sendPromptToAgentSession(sessionId, params.value(QStringLiteral("prompt")).toString());
            flashMessage(QStringLiteral("Sent \"%1\"'s failure back to its agent.")
                             .arg(params.value(QStringLiteral("workflow")).toString()));
            respond({{QStringLiteral("sent"), true}, {QStringLiteral("session"), sessionId}},
                    QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.actions.unpin"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            releaseActionMirrorPin(params.value(QStringLiteral("run")).toInt());
            respond({}, QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.actions.workflowDisabled"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const int index = repoIndexFor(params.value(QStringLiteral("owner")).toString(),
                                           params.value(QStringLiteral("name")).toString());
            if (index < 0) {
                respond({}, QStringLiteral("unknown_repo"));
                return;
            }
            setRepoWorkflowDisabled(index, params.value(QStringLiteral("path")).toString(),
                                    params.value(QStringLiteral("disabled")).toBool());
            if (index == m_repoDetailIndex)
                refreshWorkflowProgress();
            pushCoreActionQueueSnapshot();
            respond({{QStringLiteral("ok"), true}}, QString());
        });
}

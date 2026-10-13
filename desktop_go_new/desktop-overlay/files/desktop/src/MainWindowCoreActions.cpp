#include "ActionRunner.h"
#include "ActionStore.h"
#include "CoreActions.h"
#include "CoreClient.h"
#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlRepos.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace reposctl = forkmesh::localcontrol::repos;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

constexpr int kCoreActionReconcileGraceMs = 20000;

}

void MainWindow::registerCoreActionsHandlers()
{
    if (!m_coreClient)
        return;
    for (ActionRunner *runner : std::as_const(m_actionRunners))
        forkmesh::coreactions::attach(runner, m_coreClient);
    registerCoreActionQueueHandlers();
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this](bool connected) {
        if (connected)
            reconcileCoreActionRuns();
    });
    m_coreClient->handle(
        QStringLiteral("ui.actions.repo"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            QString error;
            const QJsonObject facts = coreActionsRepoFacts(params, &error);
            respond(facts, error);
        });
    m_coreClient->handle(
        QStringLiteral("ui.actions.perform"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            performCoreRepoAction(params, respond);
        });
}

QJsonObject MainWindow::coreActionsRepoFacts(const QJsonObject &params, QString *error) const
{
    const QJsonObject device = params.value(QStringLiteral("device")).toObject();
    const QString user = device.value(QStringLiteral("user")).toString();
    if (device.value(QStringLiteral("key")).toString().isEmpty() ||
        (!hostctl::ownerMayManageHosts(user, nodeOwnerDisplayName()) &&
         !hostctl::ownerMayManageHosts(user, accountOwner()))) {
        *error = QStringLiteral("not_owner");
        return {};
    }
    QString owner;
    QString name;
    if (!reposctl::parseRepoSlug(params.value(QStringLiteral("repo")).toString(), &owner, &name)) {
        *error = QStringLiteral("bad_request");
        return {};
    }
    const int index = repoIndexFor(owner, name);
    if (index < 0) {
        *error = QStringLiteral("unknown_repo");
        return {};
    }
    const RepositoryRecord &repo = m_repositories.at(index);
    const RepositoryRecord &writable = writableRecordFor(repo);
    return {
        {QStringLiteral("writableLocalPath"), writable.localPath},
        {QStringLiteral("writableMirrorPath"), writable.mirrorPath},
        {QStringLiteral("repo"), repo.owner + QLatin1Char('/') + repo.name},
        {QStringLiteral("owner"), repo.owner},
        {QStringLiteral("name"), repo.name},
        {QStringLiteral("mirrorPath"), repo.mirrorPath},
        {QStringLiteral("localPath"), repo.localPath},
        {QStringLiteral("enabled"), repo.actionsEnabled},
        {QStringLiteral("autoApprove"), repo.actionsAutoApprove},
        {QStringLiteral("disabledWorkflows"), QJsonArray::fromStringList(repo.disabledWorkflows)},
        {QStringLiteral("workflowNodes"), QJsonArray::fromStringList(repo.workflowNodes)},
        {QStringLiteral("nodeLabels"), QJsonArray::fromStringList(actionNodeLabels())},
    };
}

void MainWindow::performCoreRepoAction(const QJsonObject &params,
                                       const std::function<void(const QJsonObject &, const QString &)> &respond)
{
    const QJsonObject device = params.value(QStringLiteral("device")).toObject();
    const QString user = device.value(QStringLiteral("user")).toString();
    if (device.value(QStringLiteral("key")).toString().isEmpty() ||
        (!hostctl::ownerMayManageHosts(user, nodeOwnerDisplayName()) &&
         !hostctl::ownerMayManageHosts(user, accountOwner()))) {
        respond({}, QStringLiteral("not_owner"));
        return;
    }
    const QJsonObject in = params.value(QStringLiteral("intent")).toObject();
    reposctl::Intent intent;
    intent.action = in.value(QStringLiteral("action")).toString();
    intent.repo = in.value(QStringLiteral("repo")).toString();
    intent.path = in.value(QStringLiteral("path")).toString();
    intent.branch = in.value(QStringLiteral("branch")).toString();
    intent.run = in.value(QStringLiteral("run")).toInt();
    const QString who = params.value(QStringLiteral("who")).toString();
    QString owner;
    QString name;
    reposctl::parseRepoSlug(intent.repo, &owner, &name);
    const int index = repoIndexFor(owner, name);
    if (index < 0) {
        respond({}, QStringLiteral("unknown_repo"));
        return;
    }
    const auto started = [this, respond, intent](const QString &message) {
        ringLocalControlRepos();
        respond({{QStringLiteral("started"), true},
                 {QStringLiteral("action"), intent.action},
                 {QStringLiteral("subject"), intent.repo},
                 {QStringLiteral("message"), message}},
                QString());
    };
    logSystem(QStringLiteral("Actions: %1 for %2 requested by %3.")
                  .arg(reposctl::actionLabel(intent.action).toLower(),
                       reposctl::intentSubject(intent), who));

    if (intent.action == QLatin1String("workflow.run")) {
        startManualWorkflowRun(index, intent.path, intent.branch,
                               [respond, started](int runId, const QString &error) {
                                   if (runId <= 0) {
                                       respond({}, error.isEmpty() ? QStringLiteral("not_started")
                                                                   : error);
                                       return;
                                   }
                                   started(QStringLiteral("Started run #%1.").arg(runId));
                               });
        return;
    }
    if (reposctl::actionSwitchesWorkflow(intent.action)) {
        const bool disable = intent.action == QLatin1String("workflow.disable");
        setRepoWorkflowDisabled(index, intent.path, disable);
        if (index == m_repoDetailIndex)
            refreshWorkflowProgress();
        started(disable ? QStringLiteral("Turned off %1.").arg(intent.path)
                        : QStringLiteral("Turned on %1.").arg(intent.path));
        return;
    }
    const ActionRun *run = findRun(intent.run);
    if (!run || (run->owner + QLatin1Char('/') + run->name).toLower() !=
                    (owner + QLatin1Char('/') + name).toLower()) {
        respond({}, QStringLiteral("unknown_run"));
        return;
    }
    if (!reposctl::runStatusAllows(intent.action, run->status)) {
        respond({}, QStringLiteral("wrong_state"));
        return;
    }
    if (intent.action == QLatin1String("run.approve")) {
        QString error;
        if (!approveActionRun(intent.run, &error)) {
            respond({}, error.isEmpty() ? QStringLiteral("not_started") : error);
            return;
        }
        m_actionRuns = m_actionStore->loadAllRuns();
        refreshActionsTable();
        updateNotificationButton();
        refreshOpenPullChecks();
        processActionQueue();
        started(QStringLiteral("Approved run #%1.").arg(intent.run));
        return;
    }
    if (intent.action == QLatin1String("run.reject")) {
        if (!rejectActionRun(intent.run)) {
            respond({}, QStringLiteral("wrong_state"));
            return;
        }
        started(QStringLiteral("Rejected run #%1.").arg(intent.run));
        return;
    }
    if (intent.action == QLatin1String("run.rerun")) {
        const int created = rerunActionRun(intent.run);
        if (created <= 0) {
            respond({}, QStringLiteral("not_started"));
            return;
        }
        started(QStringLiteral("Started run #%1.").arg(created));
        return;
    }
    if (intent.action == QLatin1String("run.stop")) {
        stopActionRun(intent.run);
        started(QStringLiteral("Stopping run #%1.").arg(intent.run));
        return;
    }
    if (intent.action == QLatin1String("run.skip")) {
        skipActionRun(intent.run);
        started(QStringLiteral("Skipped run #%1.").arg(intent.run));
        return;
    }
    respond({}, QStringLiteral("unsupported_action"));
}

bool MainWindow::deferActionRunToCore(const ActionRun &run)
{
    if (!m_coreClient || run.status != ActionStatus::Running)
        return false;
    m_coreActionReconcileIds.insert(run.id);
    return true;
}

void MainWindow::requeueCoreActionRuns(const QSet<int> &ids)
{
    if (m_coreClient && m_coreClient->isConnected() &&
        m_coreClient->hasNativeRoute(QStringLiteral("core.actions.queue.attach")))
        return;
    bool any = false;
    for (int id : ids) {
        bool followed = false;
        for (const ActionRunner *runner : std::as_const(m_actionRunners))
            followed = followed || forkmesh::coreactions::follows(runner, id);
        ActionRun *run = findRun(id);
        if (followed || !run || !run->requeueAfterDesktopInterruption())
            continue;
        m_actionStore->appendLog(
            *run, QString::fromUtf8("\n==> \xE2\x9A\xA0 RESTARTING: ForkMesh exited while this "
                                    "run was active. The same approved commit will run again "
                                    "from the beginning.\n"));
        m_actionStore->saveRun(*run);
        if (!m_actionQueue.contains(run->id))
            m_actionQueue.append(run->id);
        logSystem(QStringLiteral("Actions: re-queued run #%1 \"%2\" for %3/%4 @ %5 "
                                 "after ForkMesh exited; it will restart from its "
                                 "approved snapshot. Run log: %6")
                      .arg(run->id)
                      .arg(run->workflowName, run->owner, run->name, run->commit.left(8),
                           m_actionStore->logPath(*run)));
        any = true;
    }
    if (!any)
        return;
    refreshActionsTable();
    updateNotificationButton();
    processActionQueue();
}

void MainWindow::reconcileCoreActionRuns()
{
    if (!m_actionRunsRestored || !m_actionStore || !m_coreClient)
        return;
    if (!m_coreClient->isConnected()) {
        if (m_coreActionReconcileIds.isEmpty() || m_coreActionReconcileArmed)
            return;
        m_coreActionReconcileArmed = true;
        QTimer::singleShot(kCoreActionReconcileGraceMs, this, [this] {
            m_coreActionReconcileArmed = false;
            if (m_coreClient && m_coreClient->isConnected()) {
                reconcileCoreActionRuns();
                return;
            }
            requeueCoreActionRuns(std::exchange(m_coreActionReconcileIds, {}));
        });
        return;
    }
    const QSet<int> ids = std::exchange(m_coreActionReconcileIds, {});
    const QList<int> directBefore = forkmesh::coreactions::directRuns();
    QPointer<MainWindow> self(this);
    m_coreClient->request(
        QStringLiteral("core.actions.active"), {},
        [self, ids, directBefore](bool ok, const QJsonValue &result, const QString &) {
            if (!self)
                return;
            if (!ok) {
                self->requeueCoreActionRuns(ids);
                return;
            }
            const QJsonObject reply = result.toObject();
            forkmesh::coreactions::applyClaims(reply);
            QSet<int> left = ids;
            QSet<int> known;
            bool adopted = false;
            const bool direct = self->m_coreClient &&
                                self->m_coreClient->hasNativeRoute(
                                    QStringLiteral("core.actions.queue.attach"));
            for (const QJsonValue &value : reply.value(QStringLiteral("active")).toArray()) {
                const QJsonObject entry = value.toObject();
                const ActionRun record =
                    ActionRun::fromJson(entry.value(QStringLiteral("run")).toObject());
                known.insert(record.id);
                bool followed = forkmesh::coreactions::followsDirect(record.id);
                for (const ActionRunner *runner : std::as_const(self->m_actionRunners))
                    followed = followed || forkmesh::coreactions::follows(runner, record.id);
                if (followed) {
                    left.remove(record.id);
                    continue;
                }
                if (direct && record.id > 0) {
                    forkmesh::coreactions::followDirect(record.id);
                    self->m_actionStore->adoptRun(record);
                    left.remove(record.id);
                    adopted = true;
                    self->logSystem(QStringLiteral("Actions: re-attached to run #%1 \"%2\" for "
                                                   "%3/%4, still running in forkmesh-core.")
                                        .arg(record.id)
                                        .arg(record.workflowName, record.owner, record.name));
                    continue;
                }
                for (ActionRunner *runner : std::as_const(self->m_actionRunners)) {
                    if (forkmesh::coreactions::adopt(runner, entry)) {
                        left.remove(record.id);
                        adopted = true;
                        self->logSystem(QStringLiteral("Actions: re-attached to run #%1 \"%2\" for "
                                                       "%3/%4, still running in forkmesh-core.")
                                            .arg(record.id)
                                            .arg(record.workflowName, record.owner, record.name));
                        break;
                    }
                }
            }
            QList<int> acknowledged;
            for (const QJsonValue &value : reply.value(QStringLiteral("finished")).toArray()) {
                const QJsonObject entry = value.toObject();
                const ActionRun record =
                    ActionRun::fromJson(entry.value(QStringLiteral("run")).toObject());
                if (record.id <= 0)
                    continue;
                known.insert(record.id);
                bool followed = false;
                for (const ActionRunner *runner : std::as_const(self->m_actionRunners))
                    followed = followed || forkmesh::coreactions::follows(runner, record.id);
                if (followed)
                    continue;
                forkmesh::coreactions::dropDirect(record.id);
                left.remove(record.id);
                acknowledged.append(record.id);
                if (forkmesh::coreactions::finishHandled(record.id))
                    continue;
                forkmesh::coreactions::markFinishHandled(record.id);
                forkmesh::coreactions::noteFinished(entry);
                self->m_actionStore->adoptRun(record);
                self->onRunFinished(record.id, entry.value(QStringLiteral("ok")).toBool());
                if (self && entry.value(QStringLiteral("landed")).toBool())
                    self->onReleaseMetadataLanded(record.id);
                if (!self)
                    return;
            }
            for (int id : directBefore) {
                if (!known.contains(id))
                    forkmesh::coreactions::dropDirect(id);
            }
            if (!acknowledged.isEmpty() && self->m_coreClient) {
                QJsonArray runs;
                for (int id : std::as_const(acknowledged))
                    runs.append(id);
                self->m_coreClient->request(QStringLiteral("core.actions.ack"),
                                            {{QStringLiteral("runs"), runs}});
            }
            if (adopted) {
                self->m_actionRuns = self->m_actionStore->loadAllRuns();
                self->refreshActionsTable();
                self->updateNotificationButton();
            }
            self->requeueCoreActionRuns(left);
        });
}

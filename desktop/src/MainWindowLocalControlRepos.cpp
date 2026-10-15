#include "ActionFile.h"
#include "ActionStore.h"
#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlRepos.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <memory>

namespace reposctl = forkmesh::localcontrol::repos;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

constexpr int kRingDelayMs = 750;

class ReposControlState : public QObject
{
public:
    explicit ReposControlState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshReposControlState"));
        ring.setSingleShot(true);
        ring.setInterval(kRingDelayMs);
    }

    reposctl::ActionConfirmations confirmations;
    QTimer ring;
};

ReposControlState *reposControlState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(QStringLiteral("forkmeshReposControlState"),
                                                  Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<ReposControlState *>(found))
        return state;
    return new ReposControlState(window);
}

QString repoKey(const QString &owner, const QString &name)
{
    return (owner + QLatin1Char('/') + name).toLower();
}

bool hasCheckout(const QString &dir)
{
    return !dir.trimmed().isEmpty() && QFileInfo::exists(QDir(dir).filePath(QStringLiteral(".git")));
}

QString checkoutBranch(const QString &dir)
{
    QFile head(QDir(dir).filePath(QStringLiteral(".git/HEAD")));
    if (!head.open(QIODevice::ReadOnly))
        return {};
    const QString text = QString::fromUtf8(head.read(512)).trimmed();
    if (text.startsWith(QLatin1String("ref: refs/heads/")))
        return text.mid(16);
    return text.left(7);
}

bool agentActive(const AgentSession &session)
{
    return session.status == AgentStatus::Queued || session.status == AgentStatus::Running ||
           session.status == AgentStatus::Waiting;
}

reposctl::RunFacts runFacts(const ActionRun &run, const QStringList &stepNames = {})
{
    reposctl::RunFacts facts;
    facts.id = run.id;
    facts.workflowName = run.workflowName;
    facts.workflowPath = run.workflowPath;
    facts.status = run.status;
    facts.commit = run.commit;
    facts.ref = run.ref;
    facts.pullNumber = run.pullNumber;
    facts.pullAuthor = run.pullAuthor;
    facts.createdAtMs = run.createdAtMs;
    facts.startedAtMs = run.startedAtMs;
    facts.finishedAtMs = run.finishedAtMs;
    facts.stepNames = stepNames;
    facts.stepStatuses = run.stepStatuses;
    facts.currentStepIndex = run.currentStepIndex;
    return facts;
}

QStringList workflowTriggers(const ActionWorkflow &workflow)
{
    QStringList triggers;
    if (workflow.triggersOnPush())
        triggers << QStringLiteral("push");
    if (workflow.triggersOnPullRequest())
        triggers << QStringLiteral("pull_request");
    if (workflow.triggersOnRelease())
        triggers << QStringLiteral("release");
    if (workflow.allowsManualRun())
        triggers << QStringLiteral("manual");
    return triggers;
}

}

QJsonArray MainWindow::localControlRepoRows() const
{
    QHash<QString, QList<reposctl::RunFacts>> runsByRepo;
    for (const ActionRun &run : std::as_const(m_actionRuns))
        runsByRepo[repoKey(run.owner, run.name)].append(runFacts(run));
    QHash<QString, QPair<int, int>> agentsByRepo;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.cloud || session.deletedAtMs > 0 || session.associationOnly ||
            session.owner.isEmpty())
            continue;
        QPair<int, int> &counts = agentsByRepo[repoKey(session.owner, session.name)];
        ++counts.second;
        if (agentActive(session))
            ++counts.first;
    }

    QJsonArray rows;
    QSet<QString> seen;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.owner.isEmpty() || repo.name.isEmpty() || repo.previewOnly)
            continue;
        const QString key = repoKey(repo.owner, repo.name);
        if (seen.contains(key) || !reposctl::parseRepoSlug(repo.owner + QLatin1Char('/') + repo.name,
                                                           nullptr, nullptr))
            continue;
        seen.insert(key);
        const RepositoryRecord &writable = writableRecordFor(repo);
        const bool checkout = hasCheckout(writable.localPath);
        const QPair<int, int> agents = agentsByRepo.value(key);
        rows.append(QJsonObject{
            {QStringLiteral("repo"), repo.owner + QLatin1Char('/') + repo.name},
            {QStringLiteral("owner"), repo.owner},
            {QStringLiteral("name"), repo.name},
            {QStringLiteral("description"), repo.description.left(300)},
            {QStringLiteral("branch"), checkout ? checkoutBranch(writable.localPath) : QString()},
            {QStringLiteral("checkout"), checkout},
            {QStringLiteral("mirror"), !repo.mirrorPath.isEmpty()},
            {QStringLiteral("private"), repo.isPrivate},
            {QStringLiteral("published"), repo.publishToNetwork},
            {QStringLiteral("actionsEnabled"), repo.actionsEnabled},
            {QStringLiteral("lastSyncMs"), double(repo.lastSyncMs)},
            {QStringLiteral("agents"),
             QJsonObject{{QStringLiteral("running"), agents.first},
                         {QStringLiteral("total"), agents.second}}},
            {QStringLiteral("actions"), reposctl::actionsSummary(runsByRepo.value(key))},
        });
    }
    return rows;
}

void MainWindow::ringLocalControlRepos()
{
    if (!m_localControl)
        return;
    ReposControlState *state = reposControlState(this);
    if (!state->ring.property("wired").toBool()) {
        state->ring.setProperty("wired", true);
        connect(&state->ring, &QTimer::timeout, this, [this] {
            if (m_localControl)
                m_localControl->broadcast(QStringLiteral("repos.changed"));
        });
    }
    if (!state->ring.isActive())
        state->ring.start();
}

void MainWindow::answerLocalControlRepoActions(int connectionId, int requestId, int repoIndex)
{
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    const QString key = repoKey(repo.owner, repo.name);
    QList<ActionRun> runs;
    for (const ActionRun &run : std::as_const(m_actionRuns)) {
        if (repoKey(run.owner, run.name) != key)
            continue;
        runs.append(run);
        if (runs.size() >= reposctl::kMaxRuns)
            break;
    }
    struct ActionsRead {
        QList<ActionWorkflow> workflows;
        QStringList branches;
        QList<reposctl::RunFacts> runs;
    };
    QPointer<LocalControlServer> guard(m_localControl);
    runOffThread<ActionsRead>(
        [this, repo, runs] {
            ActionsRead read;
            read.workflows = availableWorkflowsForRepo(repo);
            if (!repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists())
                read.branches =
                    QString::fromUtf8(forkmesh::ui::gitCaptureStdout(repo.mirrorPath,
                                                       {QStringLiteral("for-each-ref"),
                                                        QStringLiteral("--sort=-committerdate"),
                                                        QStringLiteral("--format=%(refname:short)"),
                                                        QStringLiteral("refs/heads")}))
                        .split(QLatin1Char('\n'), Qt::SkipEmptyParts)
                        .mid(0, reposctl::kMaxManualBranches);
            QHash<QString, QStringList> stepNames;
            for (const ActionRun &run : runs) {
                const QString digest = run.workflowPath + QLatin1Char('\n') + run.workflowContent;
                if (!stepNames.contains(digest)) {
                    QStringList names;
                    const ActionWorkflow parsed = ActionFile::parse(run.workflowPath, run.workflowContent);
                    for (const ActionStep &step : parsed.steps)
                        names << (step.name.isEmpty() ? step.run.section(QLatin1Char('\n'), 0, 0)
                                                      : step.name);
                    stepNames.insert(digest, names);
                }
                read.runs.append(runFacts(run, stepNames.value(digest)));
            }
            return read;
        },
        [this, guard, connectionId, requestId, repo](ActionsRead read) {
            if (!guard)
                return;
            QJsonArray workflows;
            for (const ActionWorkflow &wf : std::as_const(read.workflows)) {
                reposctl::WorkflowFacts facts;
                facts.path = wf.path;
                facts.name = wf.name;
                facts.error = wf.error;
                facts.valid = wf.valid;
                facts.triggers = workflowTriggers(wf);
                facts.manual = wf.valid && wf.allowsManualRun();
                facts.disabled = repo.disabledWorkflows.contains(wf.path);
                facts.here = !wf.valid || workflowRunsOnThisNode(repo, wf);
                if (wf.valid)
                    facts.runsOn = workflowRunsOnLabels(repo, wf).join(QStringLiteral(", "));
                workflows.append(reposctl::workflowRow(facts));
            }
            QJsonArray runs;
            for (const reposctl::RunFacts &run : std::as_const(read.runs))
                runs.append(reposctl::runRow(run));
            guard->respond(connectionId, requestId,
                           {{QStringLiteral("repo"), repo.owner + QLatin1Char('/') + repo.name},
                            {QStringLiteral("enabled"), repo.actionsEnabled},
                            {QStringLiteral("autoApprove"), repo.actionsAutoApprove},
                            {QStringLiteral("mirror"), !repo.mirrorPath.isEmpty()},
                            {QStringLiteral("workflows"), workflows},
                            {QStringLiteral("branches"), QJsonArray::fromStringList(read.branches)},
                            {QStringLiteral("runs"), runs},
                            {QStringLiteral("summary"), reposctl::actionsSummary(read.runs)}});
        });
}

void MainWindow::handleLocalControlReposRequest(int connectionId, int requestId,
                                                const QString &method, const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    ReposControlState *state = reposControlState(this);
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString slug;
    const auto repoIndex = [this, &params, &slug]() {
        QString owner;
        QString name;
        if (!reposctl::parseRepoSlug(params.value(QStringLiteral("repo")).toString(), &owner,
                                     &name))
            return -2;
        slug = owner + QLatin1Char('/') + name;
        return repoIndexFor(owner, name);
    };
    const auto failRepo = [server, connectionId, requestId](int index) {
        server->respondError(connectionId, requestId,
                             index == -2 ? QStringLiteral("bad_request")
                                         : QStringLiteral("unknown_repo"));
    };

    if (method == QLatin1String("repos.list")) {
        server->respond(connectionId, requestId,
                        {{QStringLiteral("repos"), localControlRepoRows()},
                         {QStringLiteral("desktop"), machineNodeName()}});
        return;
    }
    if (method == QLatin1String("repos.commits") || method == QLatin1String("repos.branches")) {
        const int index = repoIndex();
        if (index < 0) {
            failRepo(index);
            return;
        }
        const RepositoryRecord &selected = m_repositories.at(index);
        const RepositoryRecord &writable = writableRecordFor(selected);
        const bool checkout = hasCheckout(writable.localPath);
        const QString dir = checkout ? writable.localPath : selected.mirrorPath;
        if (dir.isEmpty() || !QDir(dir).exists()) {
            server->respondError(connectionId, requestId, QStringLiteral("no_checkout"));
            return;
        }
        const bool commits = method == QLatin1String("repos.commits");
        const QString ref = params.value(QStringLiteral("ref")).toString().trimmed();
        if (commits && !ref.isEmpty() && !reposctl::validBranchName(ref)) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
            return;
        }
        const int limit = commits ? reposctl::clampLimit(params.value(QStringLiteral("limit")),
                                                         reposctl::kDefaultCommits,
                                                         reposctl::kMaxCommits)
                                  : reposctl::kMaxBranches;
        QPointer<LocalControlServer> guard(server);
        const QString repo = slug;
        runOffThread<QJsonObject>(
            [dir, commits, ref, limit, checkout, repo] {
                if (commits) {
                    QStringList args{QStringLiteral("log"), reposctl::commitLogFormat(),
                                     QStringLiteral("-n"), QString::number(limit)};
                    if (!ref.isEmpty())
                        args << QStringLiteral("refs/heads/") + ref;
                    args << QStringLiteral("--");
                    return QJsonObject{
                        {QStringLiteral("repo"), repo},
                        {QStringLiteral("ref"), ref},
                        {QStringLiteral("commits"),
                         reposctl::parseCommits(forkmesh::ui::gitCaptureStdout(dir, args), limit)}};
                }
                const QByteArray out = forkmesh::ui::gitCaptureStdout(
                    dir, {QStringLiteral("for-each-ref"), QStringLiteral("--sort=-committerdate"),
                          QStringLiteral("--count=%1").arg(limit), reposctl::branchRefFormat(),
                          QStringLiteral("refs/heads")});
                return QJsonObject{{QStringLiteral("repo"), repo},
                                   {QStringLiteral("checkout"), checkout},
                                   {QStringLiteral("branches"), reposctl::parseBranches(out, limit)}};
            },
            [guard, connectionId, requestId](QJsonObject reply) {
                if (guard)
                    guard->respond(connectionId, requestId, reply);
            });
        return;
    }
    if (method == QLatin1String("repos.actions")) {
        const int index = repoIndex();
        if (index < 0) {
            failRepo(index);
            return;
        }
        answerLocalControlRepoActions(connectionId, requestId, index);
        return;
    }
    if (method == QLatin1String("repos.actions.log")) {
        const int index = repoIndex();
        if (index < 0) {
            failRepo(index);
            return;
        }
        const int runId = params.value(QStringLiteral("run")).toInt();
        const ActionRun *found = findRun(runId);
        if (!found || repoKey(found->owner, found->name) != repoKey(m_repositories.at(index).owner,
                                                                    m_repositories.at(index).name)) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_run"));
            return;
        }
        const ActionRun run = *found;
        const qint64 since = params.contains(QStringLiteral("since"))
                                 ? qint64(params.value(QStringLiteral("since")).toDouble())
                                 : -1;
        std::shared_ptr<ActionStore> store = m_actionStore;
        QPointer<LocalControlServer> guard(server);
        runOffThread<QString>(
            [store, run] { return store ? store->readLog(run) : QString(); },
            [guard, connectionId, requestId, run, since, slug](QString log) {
                if (!guard)
                    return;
                const reposctl::LogSlice slice = reposctl::logSlice(log, since);
                guard->respond(connectionId, requestId,
                               {{QStringLiteral("repo"), slug},
                                {QStringLiteral("id"), run.id},
                                {QStringLiteral("status"), run.status},
                                {QStringLiteral("text"), slice.text},
                                {QStringLiteral("from"), double(slice.from)},
                                {QStringLiteral("next"), double(slice.next)},
                                {QStringLiteral("reset"), slice.reset}});
            });
        return;
    }
    if (method == QLatin1String("repos.prepare")) {
        const int index = repoIndex();
        if (index < 0) {
            failRepo(index);
            return;
        }
        reposctl::Intent intent;
        intent.action = params.value(QStringLiteral("action")).toString();
        intent.repo = slug;
        if (!reposctl::actions().contains(intent.action)) {
            server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
            return;
        }
        const RepositoryRecord &repo = m_repositories.at(index);
        QString workflowName;
        if (reposctl::actionTargetsRun(intent.action)) {
            intent.run = params.value(QStringLiteral("run")).toInt();
            const ActionRun *run = findRun(intent.run);
            if (!run || repoKey(run->owner, run->name) != repoKey(repo.owner, repo.name)) {
                server->respondError(connectionId, requestId, QStringLiteral("unknown_run"));
                return;
            }
            if (!reposctl::runStatusAllows(intent.action, run->status)) {
                server->respondError(connectionId, requestId, QStringLiteral("wrong_state"));
                return;
            }
            workflowName = run->workflowName;
        } else if (reposctl::actionSwitchesWorkflow(intent.action)) {
            intent.path = params.value(QStringLiteral("path")).toString().trimmed();
            if (!reposctl::validWorkflowPath(intent.path)) {
                server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
                return;
            }
        } else {
            intent.path = params.value(QStringLiteral("path")).toString().trimmed();
            intent.branch = params.value(QStringLiteral("branch")).toString().trimmed();
            if (!reposctl::validWorkflowPath(intent.path) ||
                !reposctl::validBranchName(intent.branch)) {
                server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
                return;
            }
            if (!repo.actionsEnabled) {
                server->respondError(connectionId, requestId, QStringLiteral("actions_disabled"));
                return;
            }
            if (repo.mirrorPath.isEmpty()) {
                server->respondError(connectionId, requestId, QStringLiteral("no_mirror"));
                return;
            }
            if (repo.disabledWorkflows.contains(intent.path)) {
                server->respondError(connectionId, requestId, QStringLiteral("workflow_disabled"));
                return;
            }
        }
        const reposctl::ActionConfirmations::Issued issued =
            state->confirmations.issue(device.key, intent, now);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), issued.token},
                         {QStringLiteral("action"), intent.action},
                         {QStringLiteral("subject"), slug},
                         {QStringLiteral("summary"), reposctl::actionSummary(intent, workflowName)},
                         {QStringLiteral("expiresAt"), double(issued.expiresAtMs)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("repos.run")) {
        reposctl::Intent intent;
        const auto verdict = state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, now, &intent);
        const QString who = localControlDeviceLabel(connectionId);
        if (verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused a repository action from %1: %2.").arg(who, why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        runLocalControlRepoAction(connectionId, requestId, who, intent);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::runLocalControlRepoAction(int connectionId, int requestId, const QString &who,
                                           const reposctl::Intent &intent)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    QString owner;
    QString name;
    reposctl::parseRepoSlug(intent.repo, &owner, &name);
    const int index = repoIndexFor(owner, name);
    if (index < 0) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
        return;
    }
    const auto started = [this, connectionId, requestId, intent](const QString &message) {
        ringLocalControlRepos();
        if (m_localControl)
            m_localControl->respond(connectionId, requestId,
                                    {{QStringLiteral("started"), true},
                                     {QStringLiteral("action"), intent.action},
                                     {QStringLiteral("subject"), intent.repo},
                                     {QStringLiteral("message"), message}});
    };
    logSystem(QStringLiteral("Actions: %1 for %2 requested by %3.")
                  .arg(reposctl::actionLabel(intent.action).toLower(),
                       reposctl::intentSubject(intent), who));

    if (intent.action == QLatin1String("workflow.run")) {
        QPointer<LocalControlServer> guard(server);
        startManualWorkflowRun(index, intent.path, intent.branch,
                               [guard, connectionId, requestId, started](int runId,
                                                                         const QString &error) {
                                   if (!guard)
                                       return;
                                   if (runId <= 0) {
                                       guard->respondError(connectionId, requestId,
                                                           error.isEmpty()
                                                               ? QStringLiteral("not_started")
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
    if (!run || repoKey(run->owner, run->name) != repoKey(owner, name)) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_run"));
        return;
    }
    if (!reposctl::runStatusAllows(intent.action, run->status)) {
        server->respondError(connectionId, requestId, QStringLiteral("wrong_state"));
        return;
    }
    if (intent.action == QLatin1String("run.approve")) {
        QString error;
        if (!approveActionRun(intent.run, &error)) {
            server->respondError(connectionId, requestId,
                                 error.isEmpty() ? QStringLiteral("not_started") : error);
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
            server->respondError(connectionId, requestId, QStringLiteral("wrong_state"));
            return;
        }
        started(QStringLiteral("Rejected run #%1.").arg(intent.run));
        return;
    }
    if (intent.action == QLatin1String("run.rerun")) {
        const int created = rerunActionRun(intent.run);
        if (created <= 0) {
            server->respondError(connectionId, requestId, QStringLiteral("not_started"));
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
    server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
}

QJsonObject MainWindow::localControlRepoLocate(const QJsonObject &params, QString *error) const
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
    if (!reposctl::parseRepoSlug(params.value(QStringLiteral("repo")).toString(), &owner,
                                 &name)) {
        *error = QStringLiteral("bad_request");
        return {};
    }
    const int index = repoIndexFor(owner, name);
    if (index < 0) {
        *error = QStringLiteral("unknown_repo");
        return {};
    }
    const RepositoryRecord &selected = m_repositories.at(index);
    const RepositoryRecord &writable = writableRecordFor(selected);
    const bool checkout = hasCheckout(writable.localPath);
    const QString dir = checkout ? writable.localPath : selected.mirrorPath;
    if (dir.isEmpty() || !QDir(dir).exists()) {
        *error = QStringLiteral("no_checkout");
        return {};
    }
    return {{QStringLiteral("dir"), dir}, {QStringLiteral("checkout"), checkout}};
}

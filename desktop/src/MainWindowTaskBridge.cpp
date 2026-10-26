#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "TaskWorkItems.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QtConcurrent>

using namespace forkmesh::ui;

namespace {

QString repositoryRootLogoPath(const QString &workTree)
{
    if (workTree.isEmpty())
        return QString();
    static const QString names[] = {
        QStringLiteral("logo.png"), QStringLiteral("logo.webp"),
        QStringLiteral("logo.jpg"), QStringLiteral("logo.jpeg")};
    const QDir dir(workTree);
    for (const QString &name : names) {
        const QString path = dir.filePath(name);
        if (QFileInfo(path).isFile())
            return path;
    }
    return QString();
}

}

void MainWindow::initOrganizationTaskStore()
{
    m_organizationTaskStore = new forkmesh::tasks::OrganizationTaskStore(this);
    forkmesh::tasks::TaskTransport transport;
    transport.network = m_networkAccess;
    transport.apiUrl = [this] { return catalogApiUrl(); };
    transport.sessionToken = [this](const QUrl &url) {
        return accountSessionTokenForUrl(url);
    };
    transport.authenticate = [this](QUrl &url, QNetworkRequest &request,
                                    const QString &proof,
                                    const QString &resource) {
        return authenticateOrgTaskRequest(url, request, proof, resource);
    };
    transport.accountAuthenticated = [this] { return m_accountAuthenticated; };
    transport.log = [this](const QString &line) { logSystem(line); };
    m_organizationTaskStore->setTransport(transport);
    m_organizationTaskStore->setCatalogHook(
        [this](const QJsonArray &tasks, bool complete) {
            reconcileOrganizationTaskWorkItems(tasks, complete);
        });
    connect(m_organizationTaskStore,
            &forkmesh::tasks::OrganizationTaskStore::catalogLoaded, this,
            [this](const QJsonObject &payload) {
                linkExistingAgentsToOrganizationTasks(
                    payload.value(QStringLiteral("tasks")).toArray());
                refreshBranchLinks();
                refreshQuickAddTaskAssignees();
            });
}

void MainWindow::requestOrganizationTasks(
    const QByteArray &method, const QString &path, const QJsonObject &body,
    OrganizationTaskReplyHandler handler, const QList<int> &expectedHttpStatuses)
{
    if (!m_organizationTaskStore) {
        handler(false, {}, QStringLiteral(
            "Sign in to an organization account to use private tasks."));
        return;
    }
    m_organizationTaskStore->request(method, path, body, std::move(handler),
                                     expectedHttpStatuses);
}

void MainWindow::requestOrganizationTaskCatalog(
    OrganizationTaskReplyHandler handler)
{
    if (!m_organizationTaskStore) {
        handler(false, {}, QStringLiteral(
            "Sign in to an organization account to use private tasks."));
        return;
    }
    m_organizationTaskStore->requestCatalog(std::move(handler));
}

void MainWindow::refreshOrganizationTasks()
{
    if (m_organizationTaskStore)
        m_organizationTaskStore->refresh();
}

void MainWindow::refreshOrganizationTaskBadge()
{
    if (m_organizationTaskStore)
        m_organizationTaskStore->refreshOnce();
}

void MainWindow::onOrganizationTaskEvent()
{
    if (m_organizationTaskStore)
        m_organizationTaskStore->handleEvent();
}

void MainWindow::openOrganizationTask(const QString &taskId)
{
    if (taskId.isEmpty())
        return;
    if (!runNativePackageCommand(QStringLiteral("task.open"),
                                 QJsonObject{{QStringLiteral("id"), taskId}}))
        flashMessage(QStringLiteral(
                         "Tasks is not installed. Install it from the Market "
                         "list to open tasks."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
}

void MainWindow::openCreatedOrganizationTask(const QJsonObject &task)
{
    runNativePackageCommand(QStringLiteral("task.created"),
                            QJsonObject{{QStringLiteral("task"), task}});
}

void MainWindow::notifyPackagesAgentSessionsChanged()
{
    QList<QPointer<forkmesh::market::NativePackageWidget>> widgets;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        widgets.append(page.widget);
    for (const QPointer<forkmesh::market::NativePackageWidget> &widget : widgets) {
        if (widget)
            widget->agentSessionsChanged();
    }
}

void MainWindow::notifyPackagesAccountPicturesChanged()
{
    QList<QPointer<forkmesh::market::NativePackageWidget>> widgets;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        widgets.append(page.widget);
    for (const QPointer<forkmesh::market::NativePackageWidget> &widget : widgets) {
        if (widget)
            widget->accountPicturesChanged();
    }
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testApplyOrganizationTasks(const QJsonObject &payload)
{
    if (m_organizationTaskStore)
        m_organizationTaskStore->applyPayload(payload);
}

void MainWindow::testSetAccountAvatar(const QString &account,
                                      const QPixmap &picture)
{
    const QString key = QStringLiteral("user:") + account.trimmed().toLower();
    if (picture.isNull())
        m_avatars.remove(key);
    else
        m_avatars.insert(key, picture);
    notifyPackagesAccountPicturesChanged();
    refreshDmList();
}
#endif

QJsonValue MainWindow::runWindowQuery(const QString &command,
                                          const QJsonObject &)
{
    if (command == QLatin1String("task.agentSessions")) {
        QJsonArray sessions;
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            if (session.orgTaskId.isEmpty() || session.id <= 0)
                continue;
            sessions.append(QJsonObject{
                {QStringLiteral("id"), session.id},
                {QStringLiteral("orgTaskId"), session.orgTaskId},
                {QStringLiteral("createdAtMs"), double(session.createdAtMs)},
                {QStringLiteral("provider"), session.provider},
                {QStringLiteral("providerName"),
                 agentProviderName(session.provider)},
                {QStringLiteral("model"), session.model},
                {QStringLiteral("status"), session.status},
                {QStringLiteral("branch"), session.branchName},
                {QStringLiteral("merged"), session.merged},
            });
        }
        return sessions;
    }
    if (command == QLatin1String("task.repositoryLogos")) {
        QJsonArray logos;
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            const QString path = repositoryRootLogoPath(repo.localPath);
            if (path.isEmpty())
                continue;
            logos.append(QJsonObject{{QStringLiteral("owner"), repo.owner},
                                     {QStringLiteral("name"), repo.name},
                                     {QStringLiteral("logo"), path}});
        }
        return logos;
    }
    return QJsonValue();
}

bool MainWindow::runTaskWindowCommand(const QString &command,
                                      const QJsonObject &args)
{
    if (command == QLatin1String("prompt.append")) {
        const QString text = args.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            return false;
        appendTextToActivePrompt(text);
        return true;
    }
    if (command == QLatin1String("agent.show")) {
        const int id = args.value(QStringLiteral("id")).toInt();
        if (id <= 0)
            return false;
        switchToAgentsTab(id);
        return true;
    }
    if (command == QLatin1String("issue.show")) {
        const int number = args.value(QStringLiteral("number")).toInt();
        if (number <= 0)
            return false;
        showSection(0);
        if (m_repoDetailTabs && m_repoDetailTabs->button(2))
            m_repoDetailTabs->button(2)->click();
        showIssue(number);
        return true;
    }
    if (command == QLatin1String("task.cleanupWork")) {
        const QString taskId = args.value(QStringLiteral("taskId")).toString();
        cleanupOrganizationTaskWorkItems(
            taskId,
            localTaskWorkItems(taskId,
                               args.value(QStringLiteral("workItems")).toArray()),
            args.value(QStringLiteral("mode")).toInt());
        return true;
    }
    if (command == QLatin1String("task.attachWorktree")) {
        attachExistingWorktreeToOrganizationTask(
            args.value(QStringLiteral("taskId")).toString(),
            args.value(QStringLiteral("repository")).toString());
        return true;
    }
    return false;
}

bool MainWindow::runWindowRequest(
    const QString &command, const QJsonObject &args,
    forkmesh::market::NativePackageReplyHandler done)
{
    if (command == QLatin1String("task.startAgent")) {
        startOrganizationTaskAgent(args.value(QStringLiteral("task")).toObject(),
                                   args.value(QStringLiteral("prompt")).toString(),
                                   std::move(done));
        return true;
    }
    if (command == QLatin1String("task.fileAsIssue")) {
        fileOrganizationTaskAsIssue(args.value(QStringLiteral("task")).toObject(),
                                    args.value(QStringLiteral("body")).toString(),
                                    std::move(done));
        return true;
    }
    if (command == QLatin1String("task.workPlan")) {
        const QString taskId = args.value(QStringLiteral("taskId")).toString();
        const QList<forkmesh::taskwork::WorkItem> items = localTaskWorkItems(
            taskId, args.value(QStringLiteral("workItems")).toArray());
        if (items.isEmpty()) {
            done(true, QJsonObject{{QStringLiteral("items"), 0}}, QString());
            return true;
        }
        const int count = int(items.size());
        inspectOrganizationTaskWorkItems(
            items,
            [count, done = std::move(done)](
                const QList<forkmesh::taskwork::Inspection> &inspections) {
                QJsonArray losses;
                QJsonArray removable;
                for (const forkmesh::taskwork::Inspection &inspection :
                     inspections) {
                    const QString where =
                        inspection.worktreePath.isEmpty()
                            ? inspection.item.branch
                            : QStringLiteral("%1 (%2)").arg(
                                  inspection.item.branch,
                                  inspection.worktreePath);
                    if (inspection.plan == forkmesh::taskwork::Plan::Confirm)
                        losses.append(where + QLatin1Char('\n') +
                                      forkmesh::taskwork::lossSummary(inspection));
                    else if (inspection.plan == forkmesh::taskwork::Plan::Remove)
                        removable.append(where);
                }
                done(true,
                     QJsonObject{{QStringLiteral("items"), count},
                                 {QStringLiteral("losses"), losses},
                                 {QStringLiteral("removable"), removable}},
                     QString());
            });
        return true;
    }
    return false;
}

void MainWindow::linkExistingAgentsToOrganizationTasks(const QJsonArray &tasks)
{
    if (!m_agentStore)
        return;

    bool changed = false;
    for (const QJsonValue &value : tasks) {
        const QJsonObject task = value.toObject();
        QString agentId = task.value(QStringLiteral("agent"))
                              .toObject()
                              .value(QStringLiteral("sessionId"))
                              .toString()
                              .trimmed();
        if (agentId.isEmpty())
            agentId = forkmesh::tasks::taskText(task, QStringLiteral("agentSessionId"));
        bool numeric = false;
        const int sessionId = agentId.toInt(&numeric);
        if (!numeric || sessionId <= 0)
            continue;
        AgentSession *session = findAgentSession(sessionId);
        if (!session || !session->orgTaskId.isEmpty())
            continue;
        const QString repository =
            session->owner + QLatin1Char('/') + session->name;
        if (forkmesh::tasks::taskText(task, QStringLiteral("repository"))
                .compare(repository, Qt::CaseInsensitive) != 0)
            continue;
        const QJsonObject agent = task.value(QStringLiteral("agent")).toObject();
        const QString taskProvider =
            forkmesh::tasks::taskText(agent, QStringLiteral("provider"));
        if (!taskProvider.isEmpty() && !session->provider.isEmpty() &&
            taskProvider != session->provider)
            continue;
        bool comparableContent = false;
        bool matchingContent = false;
        const QString title = forkmesh::tasks::taskText(task, QStringLiteral("title"));
        if (!title.isEmpty() && !session->issueTitle.isEmpty()) {
            comparableContent = true;
            matchingContent |= title == session->issueTitle;
        }
        const QString details =
            forkmesh::tasks::taskText(task, QStringLiteral("details"));
        if (!details.isEmpty() && !session->prompt.isEmpty()) {
            comparableContent = true;
            matchingContent |= details == session->prompt.trimmed();
        }
        if (comparableContent && !matchingContent)
            continue;
        session->orgTask = true;
        session->orgTaskId = forkmesh::tasks::taskText(task, QStringLiteral("id"));
        changed |= m_agentStore->saveSession(*session);
        syncOrgTaskAgentStatus(sessionId);
        refreshAgentTaskLinkUi(sessionId);
    }

    if (changed) {
        refreshAgentTable();
        notifyPackagesAgentSessionsChanged();
    }
}

void MainWindow::startOrganizationTaskAgent(
    const QJsonObject &task, const QString &taskPrompt,
    forkmesh::market::NativePackageReplyHandler done)
{
    const auto fail = [this, &done](const QString &text) {
        flashMessage(text, true, QString(), forkmesh::kLocalOperationNoticeKind);
        done(false, {}, text);
    };
    const QString id = forkmesh::tasks::taskText(task, QStringLiteral("id"));
    const QString title = forkmesh::tasks::taskText(task, QStringLiteral("title"));
    if (id.isEmpty()) {
        fail(QStringLiteral("This task has no ID. Refresh Tasks and try again."));
        return;
    }
    if (!m_agentStore) {
        fail(QStringLiteral("Agent storage is not ready. Try again after startup."));
        return;
    }

    auto choice = promptAreaAgentChoice();
    if (!agentProviderIsKnown(choice.provider) ||
        agentIsCloudflareAiProvider(choice.provider))
        choice = {defaultAgentProvider(), QString()};
    if (agentIsCloudflareAiProvider(choice.provider))
        choice = {kFallbackAgentProvider, QString()};
    const QString provider = choice.provider;
    const QString model = choice.model;

    const QString repositoryName =
        forkmesh::tasks::taskText(task, QStringLiteral("repository"));
    const QStringList repository = repositoryName.split(QLatin1Char('/'));
    int repoIndex = -1;
    if (repositoryName.isEmpty()) {
        repoIndex = resolveAgentRepositoryIndex(-1);
    } else if (repository.size() == 2) {
        const int namedIndex = repoIndexFor(repository.at(0), repository.at(1));
        if (namedIndex >= 0)
            repoIndex = resolveAgentRepositoryIndex(namedIndex);
    }
    if (repoIndex < 0) {
        fail(repositoryName.isEmpty()
                 ? QStringLiteral("Open a repository with a local checkout to run this task's agent.")
                 : QStringLiteral("Clone or open a local checkout of %1 to run this task's agent.")
                       .arg(repositoryName));
        return;
    }

    QString prompt = m_issueQuickAdd ? m_issueQuickAdd->toPlainText().trimmed()
                                     : QString();
    if (!prompt.contains(taskPrompt)) {
        if (!prompt.isEmpty())
            prompt += QStringLiteral("\n\n");
        prompt += taskPrompt;
    }
    const QStringList images = m_quickAddImages;
    for (const QString &attachment : images) {
        if (!prompt.endsWith(QLatin1Char('\n')))
            prompt += QLatin1Char('\n');
        prompt += promptAttachmentLine(attachment);
    }

    const bool createPr =
        m_quickAddCreatePr && m_quickAddCreatePr->isChecked();
    flashMessage(QStringLiteral("Starting %1 on this task...").arg(agentProviderName(provider)),
                 false, QString(), forkmesh::kLocalOperationNoticeKind);
    const int sessionId = startAdHocAgentForRepo(
        repoIndex, prompt, provider, createPr, model,
        title, false, true,
        id, QString(),
        QString(),
        task.value(QStringLiteral("priority")).toInt(50));
    if (sessionId <= 0) {
        fail(QStringLiteral("Could not start an agent on this task. Check the Log for details."));
        return;
    }
    const AgentSession *session = findAgentSession(sessionId);
    if (session && session->status == AgentStatus::Failed) {
        fail(QStringLiteral("Agent session #%1 could not start: %2")
                 .arg(sessionId)
                 .arg(session->lastError));
        return;
    }
    if (m_issueQuickAdd)
        m_issueQuickAdd->clear();
    clearQuickAddImages();
    QStringList details;
    const QString modelLabel = agentModelLabel(model);
    if (!modelLabel.isEmpty())
        details << modelLabel;
    if (m_quickAddModeSelector)
        details << m_quickAddModeSelector->currentText();
    const QString started =
        QStringLiteral("Opened %1 agent session #%2 on this task%3. Follow its progress in Agents.")
            .arg(agentProviderName(provider))
            .arg(sessionId)
            .arg(details.isEmpty()
                     ? QString()
                     : QStringLiteral(" (%1)")
                           .arg(details.join(QStringLiteral(", "))));
    flashMessage(started, false, QString(), forkmesh::kLocalOperationNoticeKind);
    notifyPackagesAgentSessionsChanged();
    done(true, QJsonObject{{QStringLiteral("message"), started}}, QString());
}

void MainWindow::fileOrganizationTaskAsIssue(
    const QJsonObject &task, const QString &body,
    forkmesh::market::NativePackageReplyHandler done)
{
    const auto cancelled = [&done] {
        done(false, QJsonObject{{QStringLiteral("cancelled"), true}}, QString());
    };
    const QString repository =
        forkmesh::tasks::taskText(task, QStringLiteral("repository"));
    const QStringList repositoryParts =
        repository.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (repositoryParts.size() != 2) {
        QMessageBox::warning(
            this, QStringLiteral("Move task to issue"),
            QStringLiteral("This task needs an owner/repository before it can "
                           "be moved into an issue."));
        cancelled();
        return;
    }

    int repoIndex = -1;
    for (int index = 0; index < m_repositories.size(); ++index) {
        const RepositoryRecord &repo = m_repositories.at(index);
        if (repo.owner.compare(repositoryParts.at(0), Qt::CaseInsensitive) == 0 &&
            repo.name.compare(repositoryParts.at(1), Qt::CaseInsensitive) == 0) {
            repoIndex = index;
            break;
        }
    }
    if (repoIndex < 0) {
        QMessageBox::warning(
            this, QStringLiteral("Move task to issue"),
            QStringLiteral("Open the task's repository (%1) on this desktop "
                           "before moving the task into an issue.")
                .arg(repository));
        cancelled();
        return;
    }

    const QString title = forkmesh::tasks::taskText(task, QStringLiteral("title"));
    if (title.isEmpty()) {
        cancelled();
        return;
    }
    const QString repoLabel = repositoryParts.join(QLatin1Char('/'));
    const QString question =
        QStringLiteral("Move \"%1\" into an issue in %2?\n\n"
                       "The task will be removed after the issue is filed.")
            .arg(title, repoLabel);
    if (QMessageBox::question(this, QStringLiteral("Move task to issue"),
                              question,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        cancelled();
        return;
    }

    if (repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    showSection(0);
    if (m_repoDetailTabs && m_repoDetailTabs->button(2))
        m_repoDetailTabs->button(2)->click();
    if (m_issuesRepoCombo) {
        const int combo = m_issuesRepoCombo->findData(repoIndex);
        if (combo >= 0)
            m_issuesRepoCombo->setCurrentIndex(combo);
    }
    if (issuesRepoIndex() != repoIndex) {
        done(false, {}, QStringLiteral(
            "Could not select the issue repository for this task."));
        return;
    }

    const int priority = task.value(QStringLiteral("priority")).toInt();
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        const bool started = submitNewIssueToInbox(
            title, body, {}, QString(), priority, {}, {}, {},
            [done, repoLabel](bool ok, const QString &error) {
                if (!ok) {
                    done(false, {},
                         QStringLiteral("Could not file the task as an issue: %1")
                             .arg(error));
                    return;
                }
                done(true,
                     QJsonObject{{QStringLiteral("issueNumber"), -1},
                                 {QStringLiteral("notice"),
                                  QStringLiteral("Issue sent to %1's repository inbox.")
                                      .arg(repoLabel)}},
                     QString());
            });
        if (!started)
            done(false, {},
                 QStringLiteral("Could not reach the repository issue inbox."));
        return;
    }

    QString error;
    Issue created;
    const int number = store.createIssue(title, body, {}, QString(), priority,
                                         {}, {}, &error, &created);
    if (number < 0) {
        done(false, {}, QStringLiteral("Could not create the issue: %1").arg(error));
        return;
    }

    appendCreatedIssue(store, created);
    propagateRepoUpdate(repoIndex);
    if (repoIndex == m_repoDetailIndex)
        triggerTeammatesForEvent(
            QStringLiteral("new-issues"),
            QStringLiteral("Issue #%1 · %2").arg(number).arg(title));
    done(true,
         QJsonObject{{QStringLiteral("issueNumber"), number},
                     {QStringLiteral("notice"),
                      QStringLiteral("Issue #%1 created in %2.")
                          .arg(number)
                          .arg(repoLabel)}},
         QString());
}

void MainWindow::createQuickAddOrganizationTask()
{
    if (!m_issueQuickAdd)
        return;
    const QString prompt = m_issueQuickAdd->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        flashMessage(QStringLiteral("Type a task first."), true, QString(),
                     forkmesh::kLocalOperationNoticeKind);
        m_issueQuickAdd->setFocus();
        return;
    }

    QString title = prompt.section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.size() > 160)
        title = title.left(159).trimmed() + QString::fromUtf8("\xE2\x80\xA6");
    QString repository;
    const int repoIndex = issuesRepoIndex();
    if (repoIndex >= 0 && repoIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(repoIndex);
        repository = repo.owner + QLatin1Char('/') + repo.name;
    }
    QString attachmentError;
    const QJsonArray attachments = forkmesh::tasks::taskImageAttachments(
        m_quickAddImages, &attachmentError);
    if (!attachmentError.isEmpty()) {
        flashMessage(attachmentError, true);
        return;
    }
    const QString assignment = quickAddTaskAssignment();
    const QString assignee = assignment.startsWith(QLatin1String("user:"))
                                 ? assignment.mid(5)
                                 : QString();
    QJsonObject body{
        {QStringLiteral("title"), title},
        {QStringLiteral("details"), promptTextWithoutAttachments(prompt)},
        {QStringLiteral("department"), QStringLiteral("general")},
        {QStringLiteral("destination"), QStringLiteral("department")},
        {QStringLiteral("assigneeKind"),
         assignee.isEmpty() ? QStringLiteral("unassigned")
                            : QStringLiteral("user")},
        {QStringLiteral("repository"), repository},
        {QStringLiteral("priority"), 50},
    };
    if (!assignee.isEmpty())
        body.insert(QStringLiteral("assignee"), assignee);
    if (!attachments.isEmpty())
        body.insert(QStringLiteral("attachments"), attachments);
    requestOrganizationTasks(
        QByteArrayLiteral("POST"), QStringLiteral("/api/tasks"), body,
        [this, prompt, assignee](bool ok, const QJsonObject &payload,
                                 const QString &error) {
            if (!ok) {
                flashMessage(QStringLiteral("Task creation failed: %1").arg(error),
                             true);
                return;
            }
            recordQuickAddHistory(prompt);
            m_issueQuickAdd->clear();
            clearQuickAddImages();
            openCreatedOrganizationTask(
                payload.value(QStringLiteral("task")).toObject());
            flashMessage(assignee.isEmpty()
                             ? QStringLiteral("Task added to General.")
                             : QStringLiteral("Task assigned to %1.")
                                   .arg(assignee));
        });
}

QStringList MainWindow::organizationTaskMemberNames() const
{
    return m_organizationTaskStore ? m_organizationTaskStore->members()
                                   : QStringList();
}

QString MainWindow::organizationTaskActorName() const
{
    return m_organizationTaskStore ? m_organizationTaskStore->actor() : QString();
}

void MainWindow::refreshQuickAddTaskAssignees()
{
    if (!m_quickAddTaskAssignee)
        return;
    const QString actor = organizationTaskActorName();
    QStringList members = organizationTaskMemberNames();
    std::sort(members.begin(), members.end(),
              [&actor](const QString &left, const QString &right) {
                  if ((left == actor) != (right == actor))
                      return left == actor;
                  return left < right;
              });
    const QString current =
        m_quickAddTaskAssignee->currentIndex() >= 0
            ? m_quickAddTaskAssignee->currentData().toString()
            : QSettings().value(kQuickAddTaskAssigneeSetting).toString();
    const QIcon nobody =
        themedOcticon(QStringLiteral("person"), QColor("#8b949e"), 18);
    bool unchanged = m_quickAddTaskAssignee->currentIndex() >= 0 &&
                     m_quickAddTaskAssignee->count() == members.size() + 1 &&
                     m_quickAddTaskAssignee->itemData(0).toString() ==
                         QLatin1String("unassigned");
    for (int i = 0; unchanged && i < members.size(); ++i) {
        const QString &member = members.at(i);
        unchanged =
            m_quickAddTaskAssignee->itemData(i + 1).toString() ==
                QStringLiteral("user:") + member &&
            m_quickAddTaskAssignee->itemText(i + 1) ==
                (member == actor ? QStringLiteral("%1 (you)").arg(member) : member);
    }
    if (unchanged) {
        for (int i = 0; i < members.size(); ++i) {
            const QPixmap face = chatSenderAvatarSource(QString(), members.at(i));
            m_quickAddTaskAssignee->setItemIcon(i + 1,
                                                face.isNull() ? nobody : QIcon(face));
        }
        return;
    }
    {
        const QSignalBlocker blocker(m_quickAddTaskAssignee);
        m_quickAddTaskAssignee->clear();
        m_quickAddTaskAssignee->addItem(nobody, QStringLiteral("Unassigned"),
                                        QStringLiteral("unassigned"));
        m_quickAddTaskAssignee->setItemData(
            0, QStringLiteral("Add the task to General with nobody assigned"),
            Qt::ToolTipRole);
        for (const QString &member : std::as_const(members)) {
            const QPixmap face = chatSenderAvatarSource(QString(), member);
            const QString label = member == actor
                                      ? QStringLiteral("%1 (you)").arg(member)
                                      : member;
            m_quickAddTaskAssignee->addItem(face.isNull() ? nobody : QIcon(face),
                                            label,
                                            QStringLiteral("user:") + member);
            m_quickAddTaskAssignee->setItemData(
                m_quickAddTaskAssignee->count() - 1,
                QStringLiteral("Assign the task to %1").arg(member),
                Qt::ToolTipRole);
        }
    }
    const int index = m_quickAddTaskAssignee->findData(current);
    m_quickAddTaskAssignee->setCurrentIndex(-1);
    m_quickAddTaskAssignee->setCurrentIndex(index >= 0 ? index : 0);
}

QString MainWindow::quickAddTaskAssignment() const
{
    if (!m_quickAddTaskAssignee || !m_quickAddAgentProvider ||
        m_quickAddAgentProvider->currentData().toString() != kTaskPromptProvider ||
        m_quickAddTaskAssignee->currentIndex() < 0)
        return QStringLiteral("unassigned");
    return m_quickAddTaskAssignee->currentData().toString();
}

// code-health: allow(git-in-loop) the repository scan is inside the QtConcurrent::run worker
void MainWindow::attachExistingWorktreeToOrganizationTask(
    const QString &taskId, const QString &taskRepository)
{
    if (taskId.isEmpty())
        return;
    int preferred = 0;
    const QStringList repositories =
        taskWorkItemRepositories(taskRepository, &preferred);
    QString start = QDir::homePath();
    if (!repositories.isEmpty()) {
        const QString nested =
            QDir(repositories.first()).filePath(QStringLiteral(".worktrees"));
        start = QDir(nested).exists() ? nested : repositories.first();
    }
    const QString picked = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Attach a worktree to this task"), start);
    if (picked.isEmpty())
        return;
    auto *watcher = new QFutureWatcher<QPair<QString, forkmesh::taskwork::WorktreeEntry>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, taskId, picked] {
                watcher->deleteLater();
                const auto [repoPath, entry] = watcher->result();
                const QString branch = entry.branch.startsWith(QLatin1String("refs/heads/"))
                                           ? entry.branch.mid(11)
                                           : QString();
                if (repoPath.isEmpty() || branch.isEmpty()) {
                    flashMessage(
                        repoPath.isEmpty()
                            ? QStringLiteral("%1 is not a linked worktree of a "
                                             "repository on this desktop.")
                                  .arg(picked)
                            : QStringLiteral("%1 has no branch checked out.").arg(picked),
                        true, QString(), forkmesh::kLocalOperationNoticeKind);
                    return;
                }
                QString repository;
                for (const RepositoryRecord &repo : std::as_const(m_repositories))
                    if (forkmesh::taskwork::samePath(repo.localPath, repoPath))
                        repository = repo.owner + QLatin1Char('/') + repo.name;
                attachOrganizationTaskWorkItem(
                    taskId,
                    forkmesh::taskwork::validRepository(repository) ? repository
                                                                    : QString(),
                    branch, entry.path);
                flashMessage(QStringLiteral("Attaching %1 to the task.").arg(branch),
                             false, QString(), forkmesh::kLocalOperationNoticeKind);
            });
    watcher->setFuture(QtConcurrent::run([repositories, picked] {
        for (const QString &repo : repositories) {
            forkmesh::taskwork::WorktreeEntry entry;
            if (forkmesh::taskwork::findLinkedWorktree(repo, picked, &entry))
                return qMakePair(repo, entry);
        }
        return qMakePair(QString(), forkmesh::taskwork::WorktreeEntry());
    }));
}

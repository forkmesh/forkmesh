#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "TaskWorkItems.h"

#include <QCheckBox>
#include <QDir>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QVBoxLayout>

namespace tw = forkmesh::taskwork;

namespace {

constexpr qint64 kAgentTaskPickerFreshMs = 30 * 1000;
constexpr int kAgentTaskPickerIdRole = Qt::UserRole;
constexpr int kAgentTaskPickerSearchRole = Qt::UserRole + 1;

QString linkTaskText(const QJsonObject &task, const QString &field)
{
    return task.value(field).toString().trimmed();
}

bool linkTaskClosed(const QJsonObject &task)
{
    return task.value(QStringLiteral("completedAt")).toDouble() > 0 ||
           linkTaskText(task, QStringLiteral("status")) == QLatin1String("done");
}

QList<QJsonObject> attachableAgentTasks(const QJsonArray &tasks,
                                        const QString &repository)
{
    QList<QJsonObject> sameRepository;
    QList<QJsonObject> others;
    for (const QJsonValue &value : tasks) {
        const QJsonObject task = value.toObject();
        if (task.isEmpty() || linkTaskClosed(task) ||
            !tw::validTaskId(linkTaskText(task, QStringLiteral("id"))))
            continue;
        if (linkTaskText(task, QStringLiteral("repository"))
                .compare(repository, Qt::CaseInsensitive) == 0)
            sameRepository.append(task);
        else
            others.append(task);
    }
    return sameRepository + others;
}

QString agentSessionTaskTitle(const AgentSession &session, int limit)
{
    QString title = session.issueTitle.trimmed();
    if (title.isEmpty())
        title = session.prompt.section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.isEmpty())
        title = QStringLiteral("Agent session #%1").arg(session.id);
    return title.left(limit);
}

}

bool MainWindow::canLinkAgentSessionToOrgTask(int sessionId)
{
    if (sessionId <= 0 || isExternalSession(sessionId) ||
        isCloudSession(sessionId) || m_orgTaskOpenPending.contains(sessionId))
        return false;
    const AgentSession *session = findAgentSession(sessionId);
    return session && session->orgTaskId.isEmpty();
}

void MainWindow::addAgentTaskLinkActions(QMenu *menu, int sessionId)
{
    if (!menu)
        return;
    const AgentSession *session = findAgentSession(sessionId);
    if (session && !session->orgTaskId.isEmpty() && session->orgTaskAttached &&
        !isExternalSession(sessionId) && !isCloudSession(sessionId)) {
        QAction *detach = menu->addAction(QStringLiteral("Detach from task"));
        detach->setObjectName(QStringLiteral("agentDetachTaskAction"));
        detach->setToolTip(QStringLiteral(
            "Stop linking this agent to its organization task. The task "
            "stays on the board."));
        connect(detach, &QAction::triggered, this, [this, sessionId] {
            detachAgentSessionFromOrgTask(sessionId);
        });
        return;
    }
    if (!canLinkAgentSessionToOrgTask(sessionId))
        return;
    QAction *create =
        menu->addAction(QStringLiteral("Create task from this agent"));
    create->setObjectName(QStringLiteral("agentCreateTaskAction"));
    create->setToolTip(QStringLiteral(
        "Open an organization task for this run and link the agent to it"));
    connect(create, &QAction::triggered, this,
            [this, sessionId] { createOrgTaskForAgentSession(sessionId); });
    QAction *attach =
        menu->addAction(QStringLiteral("Attach to an existing task..."));
    attach->setObjectName(QStringLiteral("agentAttachTaskAction"));
    attach->setToolTip(QStringLiteral(
        "Link this agent, its branch and worktree to an open organization "
        "task"));
    connect(attach, &QAction::triggered, this,
            [this, sessionId] { attachAgentSessionToOrgTask(sessionId); });
}

void MainWindow::resolveAgentSessionTaskWorktree(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || m_taskWorkItemSessionWorktree.contains(sessionId) ||
        session->directCheckout || session->branchName.isEmpty())
        return;
    const int repoIndex = repoIndexFor(session->owner, session->name);
    const QString repoLocal =
        repoIndex >= 0 ? m_repositories.at(repoIndex).localPath : QString();
    const QString branch = session->branchName;
    const QString worktree = cachedSessionWorktree(sessionId, repoLocal, branch);
    if (!worktree.isEmpty() && QDir(worktree).exists())
        m_taskWorkItemSessionWorktree.insert(sessionId, worktree);
}

void MainWindow::syncAgentViewTaskButton(int sessionId)
{
    if (!m_agentViewTaskButton)
        return;
    const AgentSession *session = findAgentSession(sessionId);
    const bool linked = session && !session->orgTaskId.isEmpty();
    const bool linkable = !linked && canLinkAgentSessionToOrgTask(sessionId);
    m_agentViewTaskButton->setVisible(linked || linkable);
    if (linked) {
        setAgentActionCaption(m_agentViewTaskButton, QStringLiteral("checklist"),
                              QStringLiteral("Task"));
        m_agentViewTaskButton->setToolTip(
            QStringLiteral("Open the organization task linked to agent #%1")
                .arg(sessionId));
    } else if (linkable) {
        setAgentActionCaption(m_agentViewTaskButton, QStringLiteral("checklist"),
                              QStringLiteral("Add task"));
        m_agentViewTaskButton->setToolTip(
            QStringLiteral("Create an organization task from agent #%1, or "
                           "attach it to an existing task")
                .arg(sessionId));
    }
}

void MainWindow::showAgentTaskLinkMenu(int sessionId)
{
    if (!m_agentViewTaskButton)
        return;
    auto *menu = new QMenu(m_agentViewTaskButton);
    menu->setObjectName(QStringLiteral("agentTaskLinkMenu"));
    menu->setToolTipsVisible(true);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    addAgentTaskLinkActions(menu, sessionId);
    if (menu->actions().isEmpty()) {
        delete menu;
        return;
    }
    menu->popup(m_agentViewTaskButton->mapToGlobal(
        QPoint(0, m_agentViewTaskButton->height())));
}

void MainWindow::createOrgTaskForAgentSession(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || !canLinkAgentSessionToOrgTask(sessionId))
        return;
    const bool finished = AgentStatus::isTerminal(session->status);

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("agentCreateTaskDialog"));
    dialog.setWindowTitle(
        QStringLiteral("Create task from agent #%1").arg(sessionId));
    auto *layout = new QFormLayout(&dialog);
    auto *title = new QLineEdit(
        agentSessionTaskTitle(*session, kOrganizationTaskTitleLimit));
    title->setMaxLength(kOrganizationTaskTitleLimit);
    layout->addRow(QStringLiteral("Title"), title);
    auto *note = new QLabel(QStringLiteral(
        "The task gets this run's prompt as its details, is assigned to the "
        "agent, and follows its status. Its branch and worktree are attached "
        "to the task."));
    note->setWordWrap(true);
    layout->addRow(note);
    QCheckBox *close = nullptr;
    if (finished) {
        close = new QCheckBox(
            QStringLiteral("Close the task with this run's summary"));
        close->setChecked(true);
        layout->addRow(close);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->addButton(QStringLiteral("Create task"),
                       QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    title->setFocus();
    title->selectAll();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString chosenTitle = title->text().trimmed();
    const bool closeWhenFinished = !close || close->isChecked();
    if (chosenTitle.isEmpty())
        return;

    resolveAgentSessionTaskWorktree(sessionId);
    session = findAgentSession(sessionId);
    if (!session || !canLinkAgentSessionToOrgTask(sessionId))
        return;
    if (!postOrgTaskForSession(*session, chosenTitle, /*manual=*/true,
                               closeWhenFinished))
        flashMessage(QStringLiteral("Could not create a task: sign in to a "
                                    "ForkMesh account with an organization "
                                    "first."),
                     true);
}

void MainWindow::attachAgentSessionToOrgTask(int sessionId)
{
    if (!canLinkAgentSessionToOrgTask(sessionId))
        return;
    const bool fresh =
        m_organizationTasksLoaded &&
        QDateTime::currentMSecsSinceEpoch() - m_organizationTasksLoadedAt <
            kAgentTaskPickerFreshMs;
    if (fresh) {
        showAgentSessionOrgTaskPicker(sessionId, m_organizationTasks);
        return;
    }
    flashMessage(QStringLiteral("Loading organization tasks..."));
    requestOrganizationTaskCatalog(
        [this, sessionId](bool ok, const QJsonObject &payload,
                          const QString &error) {
            if (!ok) {
                flashMessage(QStringLiteral("Could not load tasks: %1")
                                 .arg(error),
                             true);
                return;
            }
            showAgentSessionOrgTaskPicker(
                sessionId, payload.value(QStringLiteral("tasks")).toArray());
        });
}

void MainWindow::showAgentSessionOrgTaskPicker(int sessionId,
                                               const QJsonArray &tasks)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || !canLinkAgentSessionToOrgTask(sessionId))
        return;
    const QList<QJsonObject> candidates = attachableAgentTasks(
        tasks, session->owner + QLatin1Char('/') + session->name);
    if (candidates.isEmpty()) {
        flashMessage(
            QStringLiteral("There are no open tasks to attach this agent to."));
        return;
    }
    const bool finished = AgentStatus::isTerminal(session->status);

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("agentAttachTaskDialog"));
    dialog.setWindowTitle(
        QStringLiteral("Attach agent #%1 to a task").arg(sessionId));
    dialog.resize(560, 480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(QStringLiteral("Filter tasks"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("agentAttachTaskList"));
    list->setWordWrap(true);
    for (const QJsonObject &task : candidates) {
        const QString title = linkTaskText(task, QStringLiteral("title"));
        QStringList meta;
        for (const QString &field :
             {QStringLiteral("repository"), QStringLiteral("assignee"),
              QStringLiteral("status"), QStringLiteral("department")}) {
            const QString value = linkTaskText(task, field);
            if (!value.isEmpty())
                meta << value;
        }
        auto *item = new QListWidgetItem(
            meta.isEmpty() ? title
                           : title + QLatin1Char('\n') +
                                 meta.join(QString::fromUtf8(" \xC2\xB7 ")));
        item->setData(kAgentTaskPickerIdRole,
                      linkTaskText(task, QStringLiteral("id")));
        item->setData(kAgentTaskPickerSearchRole,
                      QStringList{title, linkTaskText(task, QStringLiteral("details")),
                                  meta.join(QLatin1Char(' ')),
                                  linkTaskText(task, QStringLiteral("id"))}
                          .join(QLatin1Char('\n')));
        list->addItem(item);
    }
    layout->addWidget(list, 1);
    QCheckBox *close = nullptr;
    if (finished) {
        close = new QCheckBox(
            QStringLiteral("Close the task with this run's summary"));
        layout->addWidget(close);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *attach = buttons->addButton(QStringLiteral("Attach"),
                                             QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    const auto syncAttach = [list, attach] {
        const QListWidgetItem *current = list->currentItem();
        attach->setEnabled(current && !current->isHidden());
    };
    connect(filter, &QLineEdit::textChanged, &dialog,
            [list, syncAttach](const QString &text) {
                const QString needle = text.trimmed();
                QListWidgetItem *firstShown = nullptr;
                for (int row = 0; row < list->count(); ++row) {
                    QListWidgetItem *item = list->item(row);
                    const bool shown =
                        needle.isEmpty() ||
                        item->data(kAgentTaskPickerSearchRole)
                            .toString()
                            .contains(needle, Qt::CaseInsensitive);
                    item->setHidden(!shown);
                    if (shown && !firstShown)
                        firstShown = item;
                }
                if (!list->currentItem() || list->currentItem()->isHidden())
                    list->setCurrentItem(firstShown);
                syncAttach();
            });
    connect(list, &QListWidget::currentItemChanged, &dialog,
            [syncAttach] { syncAttach(); });
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog](QListWidgetItem *) { dialog.accept(); });
    list->setCurrentRow(0);
    syncAttach();
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QListWidgetItem *chosen = list->currentItem();
    if (!chosen || chosen->isHidden())
        return;
    linkAgentSessionToOrgTask(sessionId,
                              chosen->data(kAgentTaskPickerIdRole).toString(),
                              close && close->isChecked());
}

void MainWindow::linkAgentSessionToOrgTask(int sessionId, const QString &taskId,
                                           bool completeIfFinished)
{
    const QString id = taskId.trimmed().toLower();
    if (!tw::validTaskId(id) || !canLinkAgentSessionToOrgTask(sessionId))
        return;
    resolveAgentSessionTaskWorktree(sessionId);
    AgentSession *linked = findAgentSession(sessionId);
    if (!linked || !linked->orgTaskId.isEmpty())
        return;
    linked->orgTask = true;
    linked->orgTaskAttached = true;
    recordOrgTaskFields(sessionId, id, QString(), /*completeIfFinished=*/false);
    linked = findAgentSession(sessionId);
    if (completeIfFinished && linked && AgentStatus::isTerminal(linked->status))
        completeOrgTaskForSession(sessionId, QString(), /*evenIfAttached=*/true);
    const QString message =
        QStringLiteral("Attached agent session #%1 to organization task %2.")
            .arg(sessionId)
            .arg(id);
    logSystem(message);
    flashMessage(message);
    refreshAgentTable();
    if (m_organizationTasksLoaded)
        refreshOrganizationTaskAgentRows();
}

void MainWindow::detachAgentSessionFromOrgTask(int sessionId)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session || session->orgTaskId.isEmpty() || !session->orgTaskAttached)
        return;
    const QString taskId = unlinkAgentSessionFromOrgTask(sessionId);
    const QString message =
        QStringLiteral("Detached agent session #%1 from organization task %2.")
            .arg(sessionId)
            .arg(taskId);
    logSystem(message);
    flashMessage(message);
    refreshAgentTaskLinkUi(sessionId);
    refreshAgentTable();
    if (m_organizationTasksLoaded)
        refreshOrganizationTaskAgentRows();
}

QString MainWindow::unlinkAgentSessionFromOrgTask(int sessionId)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session || session->orgTaskId.isEmpty())
        return QString();
    const QString taskId = session->orgTaskId;
    session->orgTaskId.clear();
    session->orgTaskAttached = false;
    session->orgTask = false;
    m_orgTaskAgentStatusSent.remove(sessionId);
    m_taskWorkItemSessionsAttached.remove(sessionId);
    if (m_agentStore)
        m_agentStore->saveSession(*session);
    return taskId;
}

#ifdef FORKMESH_WINDOW_TESTS
QMenu *MainWindow::testAgentTaskLinkMenu(int sessionId)
{
    auto *menu = new QMenu;
    addAgentTaskLinkActions(menu, sessionId);
    return menu;
}

QString MainWindow::testAgentViewTaskButtonState(int sessionId)
{
    ensureRepoDetailTabBuilt(3);
    syncAgentViewTaskButton(sessionId);
    if (!m_agentViewTaskButton || m_agentViewTaskButton->isHidden())
        return QString();
    return m_agentViewTaskButton->toolTip();
}
#endif

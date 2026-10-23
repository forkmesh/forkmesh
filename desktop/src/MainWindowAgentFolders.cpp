#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalBlocker>

QString MainWindow::agentFolderRepo() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    const auto &repo = m_repositories.at(m_repoDetailIndex);
    return AgentFolders::repoKey(repo.owner, repo.name);
}

void MainWindow::applyAgentFolderRowCells(
    int row, const AgentFolder &folder,
    const QList<const AgentSession *> &members, bool filtering)
{
    int running = 0;
    int unread = 0;
    for (const AgentSession *session : members) {
        if (!session->merged && (session->status == AgentStatus::Running ||
                                 session->status == AgentStatus::Waiting))
            ++running;
        if (agentSessionUnread(*session))
            ++unread;
    }
    QStringList summary;
    summary << (members.size() == 1
                    ? QStringLiteral("1 agent")
                    : QStringLiteral("%1 agents").arg(members.size()));
    if (running > 0)
        summary << QStringLiteral("%1 running").arg(running);
    if (unread > 0)
        summary << QStringLiteral("%1 unread").arg(unread);
    const QString summaryText =
        summary.join(QString::fromUtf8("  \xC2\xB7  "));
    const bool collapsed = folder.collapsed && !filtering;
    const QString tip =
        QStringLiteral("Folder %1: %2. Click to %3, drag to reorder, "
                       "right-click to start, stop, rename or delete.")
            .arg(folder.name, summaryText,
                 collapsed ? QStringLiteral("expand") : QStringLiteral("collapse"));
    for (const int column : {kAgentIdColumn, kAgentIssueColumn}) {
        QTableWidgetItem *cell = m_agentTable->item(row, column);
        if (!cell || !cell->data(kAgentFolderRole).isValid()) {
            cell = new QTableWidgetItem;
            m_agentTable->setItem(row, column, cell);
        }
        cell->setFlags(Qt::ItemIsEnabled);
        cell->setData(Qt::UserRole, 0);
        cell->setData(kAgentFolderRole, folder.id);
        cell->setData(kAgentFolderCollapsedRole, collapsed);
        cell->setData(kAgentFolderSummaryRole, summaryText);
        cell->setText(column == kAgentIdColumn ? folder.name : QString());
        cell->setToolTip(tip);
    }
    if (m_agentTable->columnSpan(row, kAgentIdColumn) != 2)
        m_agentTable->setSpan(row, kAgentIdColumn, 1, 2);
}

void MainWindow::saveAgentFolders()
{
    m_agentFolders.save();
    notifyLocalControlAgentsChanged();
    refreshAgentTable();
}

QString MainWindow::createAgentFolder(const QString &name,
                                      const QList<int> &sessionIds)
{
    const QString repo = agentFolderRepo();
    if (repo.isEmpty())
        return QString();
    const QString id = m_agentFolders.create(repo, name);
    if (id.isEmpty())
        return QString();
    moveAgentSessionsToFolder(sessionIds, id);
    saveAgentFolders();
    return id;
}

void MainWindow::promptNewAgentFolder(const QList<int> &sessionIds)
{
    if (agentFolderRepo().isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New agent folder"), QStringLiteral("Folder name:"),
        QLineEdit::Normal, QStringLiteral("Folder"), &ok);
    if (!ok)
        return;
    const QString id = createAgentFolder(name, sessionIds);
    const AgentFolder *folder = m_agentFolders.folder(agentFolderRepo(), id);
    if (!folder || sessionIds.isEmpty())
        return;
    flashMessage(QStringLiteral("Moved %1 to %2.")
                     .arg(sessionIds.size() == 1
                              ? QStringLiteral("the agent")
                              : QStringLiteral("%1 agents").arg(sessionIds.size()),
                          folder->name));
}

void MainWindow::promptRenameAgentFolder(const QString &folderId)
{
    const QString repo = agentFolderRepo();
    const AgentFolder *folder = m_agentFolders.folder(repo, folderId);
    if (!folder)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename agent folder"), QStringLiteral("Folder name:"),
        QLineEdit::Normal, folder->name, &ok);
    if (ok && m_agentFolders.rename(repo, folderId, name))
        saveAgentFolders();
}

void MainWindow::deleteAgentFolder(const QString &folderId)
{
    if (m_agentFolders.remove(agentFolderRepo(), folderId))
        saveAgentFolders();
}

void MainWindow::toggleAgentFolder(const QString &folderId)
{
    const QString repo = agentFolderRepo();
    const AgentFolder *folder = m_agentFolders.folder(repo, folderId);
    if (folder && m_agentFolders.setCollapsed(repo, folderId, !folder->collapsed))
        saveAgentFolders();
}

void MainWindow::setAllAgentFoldersCollapsed(bool collapsed)
{
    if (m_agentFolders.setAllCollapsed(agentFolderRepo(), collapsed))
        saveAgentFolders();
}

void MainWindow::moveAgentFolderToEdge(const QString &folderId, bool last)
{
    if (m_agentFolders.moveToEdge(agentFolderRepo(), folderId, last))
        saveAgentFolders();
}

void MainWindow::moveAgentFolderStep(const QString &folderId, bool down)
{
    const QString repo = agentFolderRepo();
    const QList<AgentFolder> folders = m_agentFolders.folders(repo);
    for (int i = 0; i < folders.size(); ++i) {
        if (folders.at(i).id != folderId)
            continue;
        const int neighbour = down ? i + 1 : i - 1;
        if (neighbour >= 0 && neighbour < folders.size() &&
            m_agentFolders.move(repo, folderId, folders.at(neighbour).id, down))
            saveAgentFolders();
        return;
    }
}

bool MainWindow::moveAgentSessionsToFolder(const QList<int> &sessionIds,
                                           const QString &folderId)
{
    bool changed = false;
    for (const int sessionId : sessionIds) {
        const AgentSession *session = findAgentSession(sessionId);
        if (!session)
            continue;
        changed |= m_agentFolders.assign(
            AgentFolders::repoKey(session->owner, session->name),
            agentReadKey(*session), folderId);
    }
    return changed;
}

void MainWindow::selectAgentFolderSessions(const QString &folderId)
{
    const QString repo = agentFolderRepo();
    if (!m_agentTable || !m_agentFolders.folder(repo, folderId))
        return;
    if (m_agentFolders.setCollapsed(repo, folderId, false))
        saveAgentFolders();
    QItemSelectionModel *selection = m_agentTable->selectionModel();
    int first = -1;
    {
        const QSignalBlocker block(m_agentTable);
        selection->clearSelection();
        for (int row = 0; row < m_agentTable->rowCount(); ++row) {
            const QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
            if (!lead || lead->data(kAgentFolderRole).isValid() ||
                lead->data(kAgentRowFolderRole).toString() != folderId)
                continue;
            if (first < 0)
                first = row;
            selection->select(m_agentTable->model()->index(row, 0),
                              QItemSelectionModel::Select |
                                  QItemSelectionModel::Rows);
        }
        if (first >= 0)
            selection->setCurrentIndex(m_agentTable->model()->index(first, 0),
                                       QItemSelectionModel::NoUpdate);
    }
    if (first >= 0)
        if (const QTableWidgetItem *lead = m_agentTable->item(first, kAgentIdColumn))
            showAgentSession(lead->data(Qt::UserRole).toInt());
    updateAgentActionState();
}

void MainWindow::dropAgentSessions(const QList<int> &sessionIds,
                                   int targetSessionId, const QString &folderId,
                                   bool after)
{
    if (sessionIds.isEmpty())
        return;
    const QString repo = agentFolderRepo();
    QString before;
    if (const AgentSession *first = findAgentSession(sessionIds.first()))
        before = m_agentFolders.folderOf(repo, agentReadKey(*first));
    const bool moved = moveAgentSessionsToFolder(sessionIds, folderId);
    if (moved) {
        m_agentFolders.save();
        notifyLocalControlAgentsChanged();
    }
    const bool reorder = targetSessionId != 0 &&
                         m_agentListSort == AgentListSort::Priority &&
                         sessionIds.size() == 1 &&
                         targetSessionId != sessionIds.first();
    if (reorder)
        reprioritizeAgentSessions(sessionIds.first(), targetSessionId, after);
    if (moved)
        refreshAgentTable();
    if (!moved) {
        if (targetSessionId != 0 && !reorder &&
            m_agentListSort != AgentListSort::Priority)
            flashMessage(QStringLiteral("Choose Priority sorting to drag agents "
                                        "into a new priority order."));
        return;
    }
    const AgentFolder *folder = m_agentFolders.folder(repo, folderId);
    const AgentFolder *previous = m_agentFolders.folder(repo, before);
    const QString count = sessionIds.size() == 1
                              ? QStringLiteral("the agent")
                              : QStringLiteral("%1 agents").arg(sessionIds.size());
    if (folder)
        flashMessage(QStringLiteral("Moved %1 to %2.").arg(count, folder->name));
    else if (previous)
        flashMessage(QStringLiteral("Took %1 out of %2.").arg(count, previous->name));
}

void MainWindow::dropAgentFolder(const QString &folderId,
                                 const QString &targetFolderId, bool after)
{
    const QString repo = agentFolderRepo();
    const bool changed =
        targetFolderId.isEmpty()
            ? m_agentFolders.moveToEdge(repo, folderId, /*last=*/true)
            : m_agentFolders.move(repo, folderId, targetFolderId, after);
    if (changed)
        saveAgentFolders();
}

QList<int> MainWindow::agentFolderSessionIds(const QString &folderId,
                                             const QList<int> &candidates) const
{
    const QString repo = agentFolderRepo();
    QList<int> ids;
    if (repo.isEmpty() || !m_agentFolders.folder(repo, folderId))
        return ids;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (!candidates.contains(session.id) ||
            AgentFolders::repoKey(session.owner, session.name) != repo)
            continue;
        const QString key = agentReadKey(session);
        if (!key.isEmpty() && m_agentFolders.folderOf(repo, key) == folderId)
            ids << session.id;
    }
    return ids;
}

void MainWindow::startAgentFolderSessions(const QString &folderId)
{
    const AgentFolder *folder =
        m_agentFolders.folder(agentFolderRepo(), folderId);
    if (!folder)
        return;
    const QString name = folder->name;
    const QList<int> ids =
        agentFolderSessionIds(folderId, startableAgentSessionIds());
    if (ids.isEmpty()) {
        flashMessage(QStringLiteral("No stopped agents in %1.").arg(name));
        return;
    }
    startAgentSessions(ids);
    flashMessage(QStringLiteral("Started %1 agent session%2 in %3.")
                     .arg(ids.size())
                     .arg(ids.size() == 1 ? QString() : QStringLiteral("s"),
                          name));
}

void MainWindow::stopAgentFolderSessions(const QString &folderId)
{
    const AgentFolder *folder =
        m_agentFolders.folder(agentFolderRepo(), folderId);
    if (!folder)
        return;
    const QString name = folder->name;
    const QList<int> ids =
        agentFolderSessionIds(folderId, stoppableAgentSessionIds());
    if (ids.isEmpty()) {
        flashMessage(QStringLiteral("No agents are running in %1.").arg(name));
        return;
    }
    stopAgentSessions(ids);
    flashMessage(QStringLiteral("Stopped %1 agent session%2 in %3.")
                     .arg(ids.size())
                     .arg(ids.size() == 1 ? QString() : QStringLiteral("s"),
                          name));
}

QMenu *MainWindow::buildAgentFolderContextMenu(const QString &folderId,
                                               QWidget *parent)
{
    const QString repo = agentFolderRepo();
    const AgentFolder *folder = m_agentFolders.folder(repo, folderId);
    if (!folder)
        return nullptr;
    const QList<AgentFolder> folders = m_agentFolders.folders(repo);
    int index = 0;
    while (index < folders.size() && folders.at(index).id != folderId)
        ++index;
    auto *menu = new QMenu(parent);
    menu->setToolTipsVisible(true);
    auto add = [this, menu](const QString &label, const QString &objectName,
                            auto slot) {
        QAction *action = menu->addAction(label);
        action->setObjectName(objectName);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    add(folder->collapsed ? QStringLiteral("Expand") : QStringLiteral("Collapse"),
        QStringLiteral("agentFolderToggleAction"),
        [this, folderId] { toggleAgentFolder(folderId); });
    add(QStringLiteral("Select agents in folder"),
        QStringLiteral("agentFolderSelectAction"),
        [this, folderId] { selectAgentFolderSessions(folderId); });
    add(QStringLiteral("Start all in folder"),
        QStringLiteral("agentFolderStartAllAction"),
        [this, folderId] { startAgentFolderSessions(folderId); })
        ->setEnabled(
            !agentFolderSessionIds(folderId, startableAgentSessionIds()).isEmpty());
    add(QStringLiteral("Stop all in folder"),
        QStringLiteral("agentFolderStopAllAction"),
        [this, folderId] { stopAgentFolderSessions(folderId); })
        ->setEnabled(
            !agentFolderSessionIds(folderId, stoppableAgentSessionIds()).isEmpty());
    menu->addSeparator();
    add(QStringLiteral("Rename folder…"), QStringLiteral("agentFolderRenameAction"),
        [this, folderId] { promptRenameAgentFolder(folderId); });
    add(QStringLiteral("New folder…"), QStringLiteral("agentFolderNewAction"),
        [this] { promptNewAgentFolder({}); });
    menu->addSeparator();
    add(QStringLiteral("Move up"), QStringLiteral("agentFolderMoveUpAction"),
        [this, folderId] { moveAgentFolderStep(folderId, /*down=*/false); })
        ->setEnabled(index > 0);
    add(QStringLiteral("Move down"), QStringLiteral("agentFolderMoveDownAction"),
        [this, folderId] { moveAgentFolderStep(folderId, /*down=*/true); })
        ->setEnabled(index + 1 < folders.size());
    add(QStringLiteral("Move to top"), QStringLiteral("agentFolderMoveTopAction"),
        [this, folderId] { moveAgentFolderToEdge(folderId, /*last=*/false); })
        ->setEnabled(index > 0);
    add(QStringLiteral("Move to bottom"),
        QStringLiteral("agentFolderMoveBottomAction"),
        [this, folderId] { moveAgentFolderToEdge(folderId, /*last=*/true); })
        ->setEnabled(index + 1 < folders.size());
    menu->addSeparator();
    add(QStringLiteral("Expand all folders"),
        QStringLiteral("agentFolderExpandAllAction"),
        [this] { setAllAgentFoldersCollapsed(false); });
    add(QStringLiteral("Collapse all folders"),
        QStringLiteral("agentFolderCollapseAllAction"),
        [this] { setAllAgentFoldersCollapsed(true); });
    menu->addSeparator();
    QAction *remove =
        add(QStringLiteral("Delete folder"), QStringLiteral("agentFolderDeleteAction"),
            [this, folderId] { deleteAgentFolder(folderId); });
    remove->setToolTip(QStringLiteral(
        "Remove the folder. Its agents stay, back in the main list."));
    return menu;
}

QMenu *MainWindow::buildAgentListBlankContextMenu(QWidget *parent)
{
    if (agentFolderRepo().isEmpty())
        return nullptr;
    auto *menu = new QMenu(parent);
    QAction *create = menu->addAction(QStringLiteral("New folder…"));
    create->setObjectName(QStringLiteral("agentFolderNewAction"));
    connect(create, &QAction::triggered, this, [this] { promptNewAgentFolder({}); });
    if (!m_agentFolders.folders(agentFolderRepo()).isEmpty()) {
        menu->addSeparator();
        QAction *expand = menu->addAction(QStringLiteral("Expand all folders"));
        expand->setObjectName(QStringLiteral("agentFolderExpandAllAction"));
        connect(expand, &QAction::triggered, this,
                [this] { setAllAgentFoldersCollapsed(false); });
        QAction *collapse = menu->addAction(QStringLiteral("Collapse all folders"));
        collapse->setObjectName(QStringLiteral("agentFolderCollapseAllAction"));
        connect(collapse, &QAction::triggered, this,
                [this] { setAllAgentFoldersCollapsed(true); });
    }
    return menu;
}

void MainWindow::addAgentFolderMenu(QMenu *menu, int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!menu || !session || agentReadKey(*session).isEmpty())
        return;
    const QString repo = AgentFolders::repoKey(session->owner, session->name);
    if (repo != agentFolderRepo())
        return;
    const QList<int> selectedIds = selectedAgentSessionIds();
    const QList<int> targets =
        selectedIds.size() > 1 && selectedIds.contains(sessionId)
            ? selectedIds
            : QList<int>{sessionId};
    const QString current = m_agentFolders.folderOf(repo, agentReadKey(*session));
    QMenu *folderMenu = menu->addMenu(
        targets.size() > 1
            ? QStringLiteral("Move %1 agents to folder").arg(targets.size())
            : QStringLiteral("Move to folder"));
    folderMenu->setObjectName(QStringLiteral("agentListFolderMenu"));
    auto *group = new QActionGroup(folderMenu);
    group->setExclusive(true);
    for (const AgentFolder &folder : m_agentFolders.folders(repo)) {
        QAction *action = folderMenu->addAction(folder.name);
        action->setObjectName(QStringLiteral("agentListFolder:") + folder.id);
        action->setCheckable(true);
        action->setChecked(folder.id == current);
        group->addAction(action);
        const QString folderId = folder.id;
        connect(action, &QAction::triggered, this, [this, targets, folderId] {
            dropAgentSessions(targets, 0, folderId, false);
        });
    }
    if (!m_agentFolders.folders(repo).isEmpty())
        folderMenu->addSeparator();
    QAction *create = folderMenu->addAction(QStringLiteral("New folder…"));
    create->setObjectName(QStringLiteral("agentListNewFolderAction"));
    connect(create, &QAction::triggered, this,
            [this, targets] { promptNewAgentFolder(targets); });
    QAction *out = folderMenu->addAction(QStringLiteral("Remove from folder"));
    out->setObjectName(QStringLiteral("agentListRemoveFromFolderAction"));
    bool anyFiled = false;
    for (const int id : targets)
        if (const AgentSession *s = findAgentSession(id))
            anyFiled |= !m_agentFolders.folderOf(repo, agentReadKey(*s)).isEmpty();
    out->setEnabled(anyFiled);
    connect(out, &QAction::triggered, this, [this, targets] {
        dropAgentSessions(targets, 0, QString(), false);
    });
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testAgentListLayout() const
{
    QStringList rows;
    if (!m_agentTable)
        return rows;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
        if (!lead)
            continue;
        if (lead->data(kAgentFolderRole).isValid())
            rows << QStringLiteral("%1[%2]").arg(
                        lead->data(kAgentFolderCollapsedRole).toBool()
                            ? QStringLiteral("+")
                            : QStringLiteral("-"),
                        lead->text());
        else
            rows << QString::number(lead->data(Qt::UserRole).toInt());
    }
    return rows;
}

static QStringList menuActionNames(const QMenu *menu)
{
    QStringList names;
    if (!menu)
        return names;
    for (const QAction *action : menu->actions()) {
        if (action->isSeparator())
            continue;
        names << action->objectName() + (action->isEnabled()
                                             ? QString()
                                             : QStringLiteral("(disabled)"));
        if (action->menu())
            for (const QString &inner : menuActionNames(action->menu()))
                names << QStringLiteral("  ") + inner;
    }
    return names;
}

static bool triggerMenuAction(QMenu *menu, const QString &objectName)
{
    if (!menu)
        return false;
    for (QAction *action : menu->actions()) {
        if (action->objectName() == objectName && action->isEnabled()) {
            action->trigger();
            return true;
        }
        if (action->menu() && triggerMenuAction(action->menu(), objectName))
            return true;
    }
    return false;
}

QStringList MainWindow::testAgentFolderMenuActions(const QString &folderId)
{
    QMenu *menu = buildAgentFolderContextMenu(folderId, this);
    const QStringList names = menuActionNames(menu);
    delete menu;
    return names;
}

bool MainWindow::testTriggerAgentFolderMenuAction(const QString &folderId,
                                                  const QString &objectName)
{
    QMenu *menu = buildAgentFolderContextMenu(folderId, this);
    const bool triggered = triggerMenuAction(menu, objectName);
    delete menu;
    return triggered;
}

QStringList MainWindow::testAgentRowFolderMenuActions(int sessionId)
{
    QMenu *menu = buildAgentListContextMenu(sessionId, this);
    QStringList names;
    if (menu)
        for (const QAction *action : menu->actions())
            if (action->menu() &&
                action->menu()->objectName() == QStringLiteral("agentListFolderMenu"))
                names << action->text() << menuActionNames(action->menu());
    delete menu;
    return names;
}

bool MainWindow::testTriggerAgentRowFolderMenuAction(int sessionId,
                                                     const QString &objectName)
{
    QMenu *menu = buildAgentListContextMenu(sessionId, this);
    const bool triggered = triggerMenuAction(menu, objectName);
    delete menu;
    return triggered;
}

bool MainWindow::testClickAgentFolderRow(const QString &folderId)
{
    if (!m_agentTable)
        return false;
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
        if (!lead || lead->data(kAgentFolderRole).toString() != folderId)
            continue;
        m_agentTable->scrollToItem(lead);
        const QRect rect = m_agentTable->visualItemRect(lead);
        const QPoint at(rect.left() + 40, rect.center().y());
        const QPoint global = m_agentTable->viewport()->mapToGlobal(at);
        QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_agentTable->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(m_agentTable->viewport(), &release);
        return true;
    }
    return false;
}

QImage MainWindow::testGrabAgentFolderRow(const QString &folderId)
{
    if (!m_agentTable)
        return QImage();
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        const QTableWidgetItem *lead = m_agentTable->item(row, kAgentIdColumn);
        if (!lead || lead->data(kAgentFolderRole).toString() != folderId)
            continue;
        m_agentTable->scrollToItem(lead);
        const QRect rect = m_agentTable->visualRect(
            m_agentTable->model()->index(row, kAgentIdColumn));
        return m_agentTable->viewport()
            ->grab(QRect(0, rect.top(), m_agentTable->viewport()->width(),
                         rect.height()))
            .toImage();
    }
    return QImage();
}
#endif

#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AppPaths.h"
#include "TaskWorkItems.h"

#include <QDir>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

using namespace forkmesh::ui;
namespace tw = forkmesh::taskwork;

namespace {

constexpr int kTaskMissingRefreshesBeforeCleanup = 2;

struct WorkItemJob {
    tw::WorkItem item;
    QStringList preferred;
    QStringList repositories;
};

// code-health: allow(git-in-loop) runs only inside QtConcurrent::run, one git pass per work item
QList<tw::Inspection> inspectJobs(const QList<WorkItemJob> &jobs, const tw::Guard &guard)
{
    QList<tw::Inspection> results;
    for (const WorkItemJob &job : jobs) {
        tw::Inspection found;
        found.item = job.item;
        found.plan = tw::Plan::Refuse;
        found.reason = job.item.worktree.isEmpty()
                           ? QStringLiteral("no repository on this desktop matches %1")
                                 .arg(job.item.repository)
                           : QStringLiteral("no repository on this desktop lists %1")
                                 .arg(job.item.worktree);
        bool matched = false;
        for (const QString &repo : job.repositories) {
            if (!job.item.worktree.isEmpty() && tw::worktreeListed(repo, job.item.worktree)) {
                found = tw::inspect(repo, job.item, guard);
                matched = true;
                break;
            }
        }
        if (!matched && !job.preferred.isEmpty())
            found = tw::inspect(job.preferred.first(), job.item, guard);
        results.append(found);
    }
    return results;
}

QString describe(const tw::WorkItem &item)
{
    return item.worktree.isEmpty()
               ? item.branch
               : QStringLiteral("%1 (%2)").arg(item.branch, item.worktree);
}

} // namespace

QString MainWindow::taskWorkItemNodeName() const
{
    const QString node = machineNodeName().trimmed().toLower();
    if (tw::validNode(node) && !node.isEmpty())
        return node;
    const QString account = accountOwner().trimmed().toLower();
    return tw::validNode(account) ? account : QString();
}

tw::Guard MainWindow::taskWorkItemGuard() const
{
    tw::Guard guard;
    guard.home = QDir::homePath();
    const forkmesh::paths::Layout &layout = forkmesh::paths::layout();
    guard.protectedDirs = {
        layout.dataDir, layout.data, layout.config, layout.state, layout.logs, layout.ssh,
        layout.cache, forkmesh::paths::dataRoot(), forkmesh::paths::localDataRoot(),
        forkmesh::paths::cacheRoot()};
    if (!layout.homeDir.isEmpty()) {
        for (const char *name : {"data", "config", "ssh", "state", "logs"})
            guard.protectedDirs.append(
                QDir(layout.homeDir).filePath(QString::fromLatin1(name)));
    }
    const QString home = QDir::homePath();
    guard.protectedDirs.append(
        {QDir(home).filePath(QStringLiteral(".config/ForkMesh")),
         QDir(home).filePath(QStringLiteral(".local/share/ForkMesh")),
         QDir(home).filePath(QStringLiteral(".forkmesh")),
         QDir(home).filePath(QStringLiteral(".ssh")),
         QDir(home).filePath(QStringLiteral("projects/forkmesh/data")),
         QDir(home).filePath(QStringLiteral("projects/forkmesh/config")),
         QDir(home).filePath(QStringLiteral("projects/forkmesh/ssh")),
         QDir(home).filePath(QStringLiteral("projects/forkmesh/state")),
         QDir(home).filePath(QStringLiteral("projects/forkmesh/logs")),
         QDir(home).filePath(QStringLiteral("projects/forkmesh-backup"))});
    guard.protectedDirs.removeAll(QString());
    guard.protectedDirs.removeDuplicates();
    return guard;
}

QStringList MainWindow::taskWorkItemRepositories(const QString &repository,
                                                 int *preferredCount) const
{
    QStringList preferred;
    QStringList others;
    const QStringList parts = repository.split(QLatin1Char('/'));
    for (const RepositoryRecord &repo : m_repositories) {
        const QString path = repo.localPath.trimmed();
        if (path.isEmpty() || !QDir(path).exists())
            continue;
        const bool match = parts.size() == 2 &&
                           repo.owner.compare(parts.at(0), Qt::CaseInsensitive) == 0 &&
                           repo.name.compare(parts.at(1), Qt::CaseInsensitive) == 0;
        QStringList &target = match ? preferred : others;
        if (!target.contains(path))
            target.append(path);
    }
    if (preferredCount)
        *preferredCount = repository.isEmpty() ? 0 : int(preferred.size());
    return repository.isEmpty() ? others : preferred + others;
}

QJsonObject MainWindow::taskWorkItemCache() const
{
    return QJsonDocument::fromJson(
               QSettings().value(kTaskWorkItemCacheSetting).toByteArray())
        .object();
}

void MainWindow::saveTaskWorkItemCache(const QJsonObject &cache)
{
    QSettings settings;
    if (cache.isEmpty())
        settings.remove(kTaskWorkItemCacheSetting);
    else
        settings.setValue(kTaskWorkItemCacheSetting,
                          QJsonDocument(cache).toJson(QJsonDocument::Compact));
}

QList<tw::WorkItem> MainWindow::localTaskWorkItems(const QString &taskId,
                                                   const QJsonArray &serverItems) const
{
    const QString node = taskWorkItemNodeName();
    QList<tw::WorkItem> items;
    const auto add = [&items](const QList<tw::WorkItem> &more) {
        for (const tw::WorkItem &item : more)
            items = tw::withItem(items, item);
    };
    add(tw::itemsForNode(tw::fromJsonArray(serverItems), node));
    add(tw::fromJsonArray(taskWorkItemCache()
                              .value(taskId)
                              .toObject()
                              .value(QStringLiteral("items"))
                              .toArray()));
    add(tw::fromJsonArray(m_taskWorkItemAttachSending.value(taskId)));
    add(tw::fromJsonArray(m_taskWorkItemAttachQueue.value(taskId)));
    return tw::itemsForNode(items, node);
}

void MainWindow::rememberOrganizationTaskWorkItems(const QString &taskId,
                                                   const QJsonArray &serverItems)
{
    const QString node = taskWorkItemNodeName();
    QList<tw::WorkItem> mine = tw::itemsForNode(tw::fromJsonArray(serverItems), node);
    for (const tw::WorkItem &item :
         tw::fromJsonArray(m_taskWorkItemAttachSending.value(taskId)) +
             tw::fromJsonArray(m_taskWorkItemAttachQueue.value(taskId)))
        mine = tw::withItem(mine, item);
    QJsonObject cache = taskWorkItemCache();
    if (mine.isEmpty()) {
        if (!cache.contains(taskId))
            return;
        cache.remove(taskId);
    } else {
        cache.insert(taskId, QJsonObject{{QStringLiteral("items"), tw::toJsonArray(mine)},
                                         {QStringLiteral("missing"), 0}});
    }
    saveTaskWorkItemCache(cache);
}

void MainWindow::reconcileOrganizationTaskWorkItems(const QJsonArray &tasks, bool complete)
{
    QJsonObject cache = taskWorkItemCache();
    const QString node = taskWorkItemNodeName();
    if (node.isEmpty())
        return;
    QSet<QString> listed;
    bool changed = false;
    for (const QJsonValue &value : tasks) {
        const QJsonObject task = value.toObject();
        const QString id = task.value(QStringLiteral("id")).toString();
        if (!tw::validTaskId(id))
            continue;
        listed.insert(id);
        QList<tw::WorkItem> mine = tw::itemsForNode(
            tw::fromJsonArray(task.value(QStringLiteral("workItems")).toArray()), node);
        for (const tw::WorkItem &item :
             tw::fromJsonArray(m_taskWorkItemAttachSending.value(id)) +
                 tw::fromJsonArray(m_taskWorkItemAttachQueue.value(id)))
            mine = tw::withItem(mine, item);
        const QJsonObject entry{{QStringLiteral("items"), tw::toJsonArray(mine)},
                                {QStringLiteral("missing"), 0}};
        if (mine.isEmpty()) {
            if (cache.contains(id)) {
                cache.remove(id);
                changed = true;
            }
        } else if (cache.value(id).toObject() != entry) {
            cache.insert(id, entry);
            changed = true;
        }
    }
    QStringList gone;
    if (complete) {
        for (const QString &id : cache.keys()) {
            if (listed.contains(id) || m_taskWorkItemCleanupInFlight.contains(id))
                continue;
            QJsonObject entry = cache.value(id).toObject();
            const int missing = entry.value(QStringLiteral("missing")).toInt() + 1;
            if (missing >= kTaskMissingRefreshesBeforeCleanup) {
                gone.append(id);
                continue;
            }
            entry.insert(QStringLiteral("missing"), missing);
            cache.insert(id, entry);
            changed = true;
        }
    }
    if (changed)
        saveTaskWorkItemCache(cache);
    for (const QString &id : std::as_const(gone)) {
        logSystem(QStringLiteral("Tasks: task %1 is gone from the board; cleaning "
                                 "its work items on this desktop.")
                      .arg(id));
        cleanupOrganizationTaskWorkItems(
            id,
            tw::fromJsonArray(
                cache.value(id).toObject().value(QStringLiteral("items")).toArray()),
            0);
    }
}

void MainWindow::attachOrgTaskWorkItemForSession(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || session->orgTaskId.isEmpty() || session->directCheckout ||
        m_taskWorkItemSessionsAttached.contains(sessionId))
        return;
    QString worktree = m_taskWorkItemSessionWorktree.value(sessionId);
    if (worktree.isEmpty())
        worktree = m_streamWorktree.value(sessionId);
    if (worktree.isEmpty() || session->branchName.isEmpty())
        return;
    m_taskWorkItemSessionsAttached.insert(sessionId);
    const QString repository = session->owner + QLatin1Char('/') + session->name;
    attachOrganizationTaskWorkItem(session->orgTaskId,
                                   tw::validRepository(repository) ? repository : QString(),
                                   session->branchName, QDir(worktree).absolutePath());
}

void MainWindow::attachOrganizationTaskWorkItem(const QString &taskId,
                                                const QString &repository,
                                                const QString &branch,
                                                const QString &worktree)
{
    const QString node = taskWorkItemNodeName();
    if (!tw::validTaskId(taskId) || !tw::validBranch(branch) || node.isEmpty() ||
        !tw::validRepository(repository) || worktree.size() > 400) {
        logSystem(QStringLiteral("Tasks: not attaching %1 to task %2: the branch, "
                                 "repository or node name is not usable.")
                      .arg(branch, taskId));
        return;
    }
    const tw::WorkItem item{repository, branch, worktree, node};
    QJsonArray queued = m_taskWorkItemAttachQueue.value(taskId);
    queued.append(tw::toJson(item));
    m_taskWorkItemAttachQueue.insert(taskId, queued);
    QJsonObject cache = taskWorkItemCache();
    const QList<tw::WorkItem> cached = tw::withItem(
        tw::fromJsonArray(cache.value(taskId).toObject().value(QStringLiteral("items")).toArray()),
        item);
    cache.insert(taskId, QJsonObject{{QStringLiteral("items"), tw::toJsonArray(cached)},
                                     {QStringLiteral("missing"), 0}});
    saveTaskWorkItemCache(cache);
    sendOrganizationTaskWorkItemAttach(taskId);
}

void MainWindow::sendOrganizationTaskWorkItemAttach(const QString &taskId)
{
    if (m_taskWorkItemAttachSending.contains(taskId) ||
        m_taskWorkItemAttachQueue.value(taskId).isEmpty())
        return;
    const QJsonArray sending = m_taskWorkItemAttachQueue.take(taskId);
    m_taskWorkItemAttachSending.insert(taskId, sending);
    requestOrganizationTasks(
        QByteArrayLiteral("PATCH"), QStringLiteral("/api/tasks/") + taskId,
        QJsonObject{{QStringLiteral("attachWorkItems"), sending}},
        [this, taskId, sending](bool ok, const QJsonObject &data, const QString &error) {
            m_taskWorkItemAttachSending.remove(taskId);
            QStringList names;
            for (const tw::WorkItem &item : tw::fromJsonArray(sending))
                names.append(describe(item));
            if (!ok && data.value(QStringLiteral("error")).toString() ==
                           QLatin1String("task_not_found")) {
                m_taskWorkItemAttachQueue.remove(taskId);
                logSystem(QStringLiteral("Tasks: task %1 is no longer on the board; "
                                         "%2 stays local.")
                              .arg(taskId, names.join(QStringLiteral(", "))));
                return;
            }
            if (!ok) {
                logSystem(QStringLiteral("Tasks: attaching %1 to task %2 failed: %3")
                              .arg(names.join(QStringLiteral(", ")), taskId, error));
                sendOrganizationTaskWorkItemAttach(taskId);
                return;
            }
            const QJsonObject task = data.value(QStringLiteral("task")).toObject();
            rememberOrganizationTaskWorkItems(
                taskId, task.value(QStringLiteral("workItems")).toArray());
            for (int index = 0; index < m_organizationTasks.size(); ++index) {
                if (m_organizationTasks.at(index).toObject().value(QStringLiteral("id")) ==
                    taskId) {
                    QJsonObject updated = m_organizationTasks.at(index).toObject();
                    updated.insert(QStringLiteral("workItems"),
                                   task.value(QStringLiteral("workItems")));
                    m_organizationTasks.replace(index, updated);
                    break;
                }
            }
            logSystem(QStringLiteral("Tasks: attached %1 to task %2.")
                          .arg(names.join(QStringLiteral(", ")), taskId));
            sendOrganizationTaskWorkItemAttach(taskId);
        },
        {404});
}

void MainWindow::inspectOrganizationTaskWorkItems(
    const QList<tw::WorkItem> &items,
    std::function<void(const QList<tw::Inspection> &)> done)
{
    QList<WorkItemJob> jobs;
    for (const tw::WorkItem &item : items) {
        int preferred = 0;
        const QStringList repositories = taskWorkItemRepositories(item.repository, &preferred);
        jobs.append(WorkItemJob{item, repositories.mid(0, preferred), repositories});
    }
    const tw::Guard guard = taskWorkItemGuard();
    auto *watcher = new QFutureWatcher<QList<tw::Inspection>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [watcher, done = std::move(done)] {
                watcher->deleteLater();
                done(watcher->result());
            });
    watcher->setFuture(QtConcurrent::run([jobs, guard] { return inspectJobs(jobs, guard); }));
}

void MainWindow::cleanupOrganizationTaskWorkItems(const QString &taskId,
                                                  const QList<tw::WorkItem> &items, int mode)
{
    const auto dropFromCache = [this, taskId] {
        QJsonObject cache = taskWorkItemCache();
        if (cache.contains(taskId)) {
            cache.remove(taskId);
            saveTaskWorkItemCache(cache);
        }
    };
    if (items.isEmpty() || !tw::validTaskId(taskId)) {
        dropFromCache();
        return;
    }
    if (m_taskWorkItemCleanupInFlight.contains(taskId))
        return;
    m_taskWorkItemCleanupInFlight.insert(taskId);
    m_taskWorkItemAttachQueue.remove(taskId);
    inspectOrganizationTaskWorkItems(
        items, [this, taskId, mode, dropFromCache](const QList<tw::Inspection> &inspections) {
            QList<tw::Inspection> removable;
            QList<tw::Inspection> unmerged;
            for (const tw::Inspection &inspection : inspections) {
                if (inspection.plan == tw::Plan::Refuse) {
                    logSystem(QStringLiteral("Tasks: left %1 of deleted task %2 alone: %3.")
                                  .arg(describe(inspection.item), taskId, inspection.reason));
                } else if (inspection.plan == tw::Plan::Remove) {
                    removable.append(inspection);
                } else if (mode == 1) {
                    removable.append(inspection);
                } else if (mode == 2) {
                    logSystem(QStringLiteral("Tasks: kept %1 of deleted task %2 as asked.")
                                  .arg(describe(inspection.item), taskId));
                } else {
                    unmerged.append(inspection);
                }
            }
            dropFromCache();
            if (!removable.isEmpty())
                removeOrganizationTaskWorkItems(taskId, removable, mode == 1);
            if (unmerged.isEmpty()) {
                if (removable.isEmpty())
                    m_taskWorkItemCleanupInFlight.remove(taskId);
                return;
            }
            QStringList losses;
            for (const tw::Inspection &inspection : std::as_const(unmerged))
                losses.append(describe(inspection.item) + QLatin1Char('\n') +
                              tw::lossSummary(inspection));
            auto *box = new QMessageBox(
                QMessageBox::Warning, QStringLiteral("Task deleted"),
                QStringLiteral("Task %1 was deleted, and its work on this desktop "
                               "is not merged.")
                    .arg(taskId),
                QMessageBox::NoButton, this);
            box->setWindowModality(Qt::NonModal);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setInformativeText(
                losses.join(QStringLiteral("\n\n")) +
                QStringLiteral("\n\nDelete saves a backup at "
                               "refs/forkmesh/trash/tasks/%1/<branch> first. Keep "
                               "leaves everything as it is.")
                    .arg(taskId));
            QPushButton *remove =
                box->addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
            box->addButton(QStringLiteral("Keep"), QMessageBox::RejectRole);
            connect(box, &QMessageBox::buttonClicked, this,
                    [this, taskId, unmerged, remove](QAbstractButton *button) {
                        if (button == remove) {
                            removeOrganizationTaskWorkItems(taskId, unmerged, true);
                            return;
                        }
                        for (const tw::Inspection &inspection : unmerged)
                            logSystem(
                                QStringLiteral("Tasks: kept %1 of deleted task %2.")
                                    .arg(describe(inspection.item), taskId));
                    });
            connect(box, &QObject::destroyed, this, [this, taskId] {
                m_taskWorkItemCleanupInFlight.remove(taskId);
            });
            box->show();
        });
}

// code-health: allow(git-in-loop) the loop is inside the QtConcurrent::run worker
void MainWindow::removeOrganizationTaskWorkItems(const QString &taskId,
                                                 const QList<tw::Inspection> &inspections,
                                                 bool discard)
{
    const tw::Guard guard = taskWorkItemGuard();
    auto *watcher = new QFutureWatcher<QList<tw::Outcome>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, taskId] {
        watcher->deleteLater();
        int removed = 0;
        int failed = 0;
        for (const tw::Outcome &outcome : watcher->result()) {
            outcome.ok ? ++removed : ++failed;
            logSystem(QStringLiteral("Tasks: task %1 cleanup: %2").arg(taskId, outcome.message));
        }
        m_taskWorkItemCleanupInFlight.remove(taskId);
        if (removed || failed)
            flashMessage(
                failed ? QStringLiteral("Deleted task: removed %1 worktree(s), %2 left "
                                        "in place (see the log).")
                             .arg(removed)
                             .arg(failed)
                       : QStringLiteral("Deleted task: removed %1 worktree(s) and "
                                        "branch(es).")
                             .arg(removed),
                failed > 0, QString(), forkmesh::kLocalOperationNoticeKind);
    });
    watcher->setFuture(QtConcurrent::run([inspections, guard, taskId, discard] {
        QList<tw::Outcome> outcomes;
        for (const tw::Inspection &before : inspections) {
            const tw::Inspection now = tw::inspect(before.repoPath, before.item, guard);
            if (now.plan == tw::Plan::Remove) {
                outcomes.append(tw::removeMerged(now));
            } else if (now.plan == tw::Plan::Confirm && discard) {
                outcomes.append(tw::removeWithBackup(now, taskId));
            } else {
                tw::Outcome skipped;
                skipped.message =
                    QStringLiteral("left %1 alone: %2")
                        .arg(describe(now.item),
                             now.plan == tw::Plan::Refuse
                                 ? now.reason
                                 : QStringLiteral("it changed after it was checked"));
                outcomes.append(skipped);
            }
        }
        return outcomes;
    }));
}

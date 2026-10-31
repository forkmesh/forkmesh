#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "ActionRunner.h"
#include "AgentScratch.h"
#include "CleanupHostFeed.h"
#include "TaskWorkItems.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

using namespace forkmesh::ui;
namespace cf = forkmesh::cleanupfeed;
namespace tw = forkmesh::taskwork;

namespace {

QString gitText(const QString &dir, const QStringList &args, bool *ok = nullptr,
                QString *error = nullptr)
{
    QByteArray out;
    const bool succeeded = runGitCapture(dir, args, &out, error);
    if (ok)
        *ok = succeeded;
    return succeeded ? QString::fromUtf8(out).trimmed() : QString();
}

bool linkedWorktree(const QString &repoPath, const QString &worktreePath,
                    tw::WorktreeEntry *entry, QString *mainBranch)
{
    bool ok = false;
    const QString listing =
        gitText(repoPath, {QStringLiteral("worktree"), QStringLiteral("list"),
                           QStringLiteral("--porcelain")},
                &ok);
    if (!ok)
        return false;
    const QList<tw::WorktreeEntry> entries = tw::parseWorktreeList(listing);
    for (qsizetype i = 0; i < entries.size(); ++i) {
        if (i == 0) {
            *mainBranch = entries.at(i).branch;
            continue;
        }
        if (tw::samePath(entries.at(i).path, worktreePath)) {
            *entry = entries.at(i);
            return true;
        }
    }
    return false;
}

QString shortBranch(const QString &ref)
{
    return ref.startsWith(QLatin1String("refs/heads/")) ? ref.mid(11) : ref;
}

} // namespace

void MainWindow::installCleanupHost()
{
    cf::Host host;
    host.repositories = [this] {
        QList<cf::Repository> list;
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (repo.localPath.trimmed().isEmpty())
                continue;
            list.append({repo.owner.isEmpty() ? repo.name
                                              : repo.owner + QLatin1Char('/') + repo.name,
                         repo.localPath});
        }
        return list;
    };
    host.agents = [this] {
        QList<cf::Agent> list;
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            cf::Agent agent;
            agent.id = session.id;
            agent.issueNumber = session.issueNumber;
            agent.title = session.issueTitle.isEmpty() ? session.name : session.issueTitle;
            agent.status = session.status;
            agent.branch = session.branchName;
            agent.worktree = m_streamWorktree.value(session.id);
            if (agent.worktree.isEmpty())
                agent.worktree = m_sessionWorkdirCache.value(session.id);
            for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
                if (repo.name.compare(session.name, Qt::CaseInsensitive) == 0 &&
                    (session.owner.isEmpty() ||
                     repo.owner.compare(session.owner, Qt::CaseInsensitive) == 0)) {
                    agent.repoPath = repo.localPath;
                    break;
                }
            }
            agent.live = session.status == AgentStatus::Running ||
                         session.status == AgentStatus::Waiting ||
                         session.backgroundProcesses > 0;
            list.append(agent);
        }
        return list;
    };
    host.taskLinks = [this] {
        QHash<QString, QString> titles;
        QList<cf::TaskLink> links;
        const QString node = taskWorkItemNodeName();
        const auto add = [&links, &titles](const QString &taskId,
                                           const QList<tw::WorkItem> &items) {
            for (const tw::WorkItem &item : items) {
                bool known = false;
                for (const cf::TaskLink &link : std::as_const(links))
                    known = known || (link.taskId == taskId && link.branch == item.branch &&
                                      link.worktree == item.worktree);
                if (!known)
                    links.append({taskId, titles.value(taskId), item.repository, item.branch,
                                  item.worktree});
            }
        };
        const QJsonArray catalog = m_organizationTaskStore
                                       ? m_organizationTaskStore->tasks()
                                       : QJsonArray();
        for (const QJsonValue &value : catalog) {
            const QJsonObject task = value.toObject();
            titles.insert(task.value(QStringLiteral("id")).toString(),
                          task.value(QStringLiteral("title")).toString());
        }
        for (const QJsonValue &value : catalog) {
            const QJsonObject task = value.toObject();
            add(task.value(QStringLiteral("id")).toString(),
                tw::itemsForNode(
                    tw::fromJsonArray(task.value(QStringLiteral("workItems")).toArray()),
                    node));
        }
        const QJsonObject cache = taskWorkItemCache();
        for (auto it = cache.begin(); it != cache.end(); ++it)
            add(it.key(), tw::fromJsonArray(it.value()
                                                .toObject()
                                                .value(QStringLiteral("items"))
                                                .toArray()));
        return links;
    };
    host.actionsSandboxDir = [this] {
        return m_actionStore ? m_actionStore->sandboxDir() : QString();
    };
    host.claimedSandboxes = [] { return ActionRunner::claimedSandboxNames(); };
    host.removeActionSandbox = [](const QString &base, const QString &path) {
        return ActionRunner::removeUnclaimedSandbox(base, path);
    };
    host.log = [this](const QString &text) { logSystem(text); };
    host.removeWorktree = [this](const cf::WorktreeRemoval &request,
                                 cf::WorktreeRemovalDone done) {
        const QList<int> released = releaseTrackedWorktree(request.worktreePath);
        runOffThread<cf::WorktreeRemovalResult>(
            [request] {
                cf::WorktreeRemovalResult result;
                tw::WorktreeEntry entry;
                QString mainBranch;
                if (!linkedWorktree(request.repoPath, request.worktreePath, &entry,
                                    &mainBranch)) {
                    result.message =
                        QStringLiteral("%1 is not a linked worktree of %2; nothing was removed.")
                            .arg(request.worktreePath, request.repoPath);
                    return result;
                }
                const QString branch = shortBranch(entry.branch);
                bool ok = false;
                const QString status = gitText(
                    request.worktreePath,
                    {QStringLiteral("--no-optional-locks"), QStringLiteral("status"),
                     QStringLiteral("--porcelain")},
                    &ok);
                if (!ok) {
                    result.message = QStringLiteral("Could not read the status of %1.")
                                         .arg(request.worktreePath);
                    return result;
                }
                const bool dirty = !status.isEmpty();
                if (dirty && !request.discardChanges) {
                    result.message =
                        QStringLiteral("%1 has uncommitted changes; kept.").arg(request.worktreePath);
                    return result;
                }
                const QString stamp = QDate::currentDate().toString(Qt::ISODate);
                const QString label =
                    branch.isEmpty() ? QFileInfo(request.worktreePath).fileName() : branch;
                if (dirty) {
                    const QString stash = gitText(
                        request.worktreePath,
                        {QStringLiteral("stash"), QStringLiteral("create"),
                         QStringLiteral("forkmesh cleanup: uncommitted work")});
                    if (!stash.isEmpty()) {
                        const QString ref =
                            QStringLiteral("refs/forkmesh/trash/cleanup/%1/%2-wip").arg(stamp, label);
                        gitText(request.repoPath, {QStringLiteral("update-ref"), ref, stash}, &ok);
                        if (ok)
                            result.wipRef = ref;
                    }
                }
                QString tip;
                if (request.retireBranch && !branch.isEmpty()) {
                    const QString refusal = tw::branchRefusal(
                        branch, shortBranch(mainBranch), QStringLiteral("main"));
                    if (!refusal.isEmpty()) {
                        result.message = QStringLiteral("Branch kept: %1.").arg(refusal);
                    } else {
                        tip = gitText(request.repoPath,
                                      {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                       QStringLiteral("refs/heads/") + branch},
                                      &ok);
                        if (ok && !tip.isEmpty()) {
                            const QString ref =
                                QStringLiteral("refs/forkmesh/trash/cleanup/%1/%2").arg(stamp, branch);
                            gitText(request.repoPath, {QStringLiteral("update-ref"), ref, tip}, &ok);
                            if (ok)
                                result.trashRef = ref;
                        }
                    }
                }
                if (dirty) {
                    const WorktreeRemoval removal = removeWorktreeAndBranch(
                        request.repoPath, request.worktreePath, branch, false);
                    if (removal.refused) {
                        result.message = QStringLiteral("git refused to remove %1.")
                                             .arg(request.worktreePath);
                        return result;
                    }
                } else {
                    QString error;
                    gitText(request.repoPath,
                            {QStringLiteral("worktree"), QStringLiteral("remove"),
                             request.worktreePath},
                            &ok, &error);
                    if (!ok) {
                        result.message = QStringLiteral("git refused to remove %1: %2")
                                             .arg(request.worktreePath, error.trimmed());
                        return result;
                    }
                }
                result.removed = true;
                forkmesh::agentscratch::removeAll(request.worktreePath);
                if (!result.trashRef.isEmpty()) {
                    gitText(request.repoPath,
                            {QStringLiteral("update-ref"), QStringLiteral("-d"),
                             QStringLiteral("refs/heads/") + branch, tip},
                            &ok);
                    result.branchRetired = ok;
                }
                return result;
            },
            [this, request, released, done](cf::WorktreeRemovalResult result) {
                if (!result.removed)
                    retrackWorktree(released, request.worktreePath);
                logSystem(result.removed
                              ? QStringLiteral("Cleanup: removed worktree %1%2%3.")
                                    .arg(request.worktreePath,
                                         result.branchRetired
                                             ? QStringLiteral("; branch saved as ") + result.trashRef
                                             : QString(),
                                         result.wipRef.isEmpty()
                                             ? QString()
                                             : QStringLiteral("; uncommitted work saved as ") +
                                                   result.wipRef)
                              : QStringLiteral("Cleanup: %1").arg(result.message));
                if (repoDetailShows(request.repoPath))
                    loadWorktreesPanel();
                if (done)
                    done(result);
            });
    };
    cf::setHost(this, std::move(host));
}

#pragma once

#include <QList>
#include <QSet>
#include <QString>

#include <functional>

namespace forkmesh::cleanupfeed {

struct Repository {
    QString name;
    QString path;
};

struct Agent {
    int id = 0;
    int issueNumber = 0;
    QString title;
    QString status;
    QString repoPath;
    QString branch;
    QString worktree;
    bool live = false;
};

struct TaskLink {
    QString taskId;
    QString title;
    QString repository;
    QString branch;
    QString worktree;
};

struct WorktreeRemoval {
    QString repoPath;
    QString worktreePath;
    QString branch;
    bool discardChanges = false;
    bool retireBranch = false;
};

struct WorktreeRemovalResult {
    bool removed = false;
    bool branchRetired = false;
    QString trashRef;
    QString wipRef;
    QString message;
};

using WorktreeRemovalDone = std::function<void(const WorktreeRemovalResult &)>;

struct Host {
    std::function<QList<Repository>()> repositories;
    std::function<QList<Agent>()> agents;
    std::function<QList<TaskLink>()> taskLinks;
    std::function<QString()> actionsSandboxDir;
    std::function<QSet<QString>()> claimedSandboxes;
    std::function<QString(const QString &, const QString &)> removeActionSandbox;
    std::function<void(const WorktreeRemoval &, WorktreeRemovalDone)> removeWorktree;
    std::function<void(const QString &)> log;
};

void setHost(const void *owner, Host host);
void clearHost(const void *owner);
bool hasHost();
const Host &host();

} // namespace forkmesh::cleanupfeed

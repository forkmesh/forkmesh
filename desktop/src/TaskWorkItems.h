#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::taskwork {

inline constexpr int kMaxWorkItems = 20;

struct WorkItem {
    QString repository;
    QString branch;
    QString worktree;
    QString node;
};

QJsonObject toJson(const WorkItem &item);
WorkItem fromJson(const QJsonObject &object);
QList<WorkItem> fromJsonArray(const QJsonArray &array);
QJsonArray toJsonArray(const QList<WorkItem> &items);
bool sameKey(const WorkItem &a, const WorkItem &b);
QList<WorkItem> withItem(const QList<WorkItem> &items, const WorkItem &item);
QList<WorkItem> withoutItem(const QList<WorkItem> &items, const WorkItem &item);
QList<WorkItem> itemsForNode(const QList<WorkItem> &items, const QString &node);

bool validBranch(const QString &branch);
bool validRepository(const QString &repository);
bool validNode(const QString &node);
bool validTaskId(const QString &taskId);

QByteArray canonicalJson(const QJsonValue &value);
QString updateDigest(const QJsonObject &body);

struct WorktreeEntry {
    QString path;
    QString head;
    QString branch;
    bool bare = false;
    bool detached = false;
};

QList<WorktreeEntry> parseWorktreeList(const QString &porcelain);
bool samePath(const QString &a, const QString &b);
bool pathWithin(const QString &path, const QString &root);

struct Guard {
    QString home;
    QStringList protectedDirs;
};

QString pathRefusal(const QString &worktree, const QList<WorktreeEntry> &entries,
                    const Guard &guard);
QString branchRefusal(const QString &branch, const QString &mainCheckoutBranch,
                      const QString &defaultBranch);

enum class Plan { Refuse, Remove, Confirm };

Plan decide(bool refused, bool dirty, bool merged);
QString backupRef(const QString &taskId, const QString &branch);

struct Inspection {
    QString repoPath;
    WorkItem item;
    Plan plan = Plan::Refuse;
    QString reason;
    QString defaultBranch;
    QString worktreePath;
    bool branchExists = false;
    bool dirty = false;
    bool merged = false;
    QStringList dirtyFiles;
    QStringList unmergedCommits;
};

bool worktreeListed(const QString &repoPath, const QString &worktree);
bool findLinkedWorktree(const QString &repoPath, const QString &path,
                        WorktreeEntry *entry);
Inspection inspect(const QString &repoPath, const WorkItem &item,
                   const Guard &guard, const QString &defaultBranchHint = {});

struct Outcome {
    bool ok = false;
    QString message;
    QString backupRef;
};

Outcome removeMerged(const Inspection &inspection);
Outcome removeWithBackup(const Inspection &inspection, const QString &taskId);
QString lossSummary(const Inspection &inspection, int maxLines = 8);

} // namespace forkmesh::taskwork

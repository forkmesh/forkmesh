#pragma once

#include "CleanupHostFeed.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

namespace forkmesh::cleanup {

enum class Category { Claude, Codex, Worktrees, BuildTemp, OtherCaches };
enum class Status { InUse, Attached, Orphaned, Protected };
enum class Kind { Files, Worktree, ActionSandbox, Report };

QString categoryName(Category category);
QString statusName(Status status);
QList<Category> categories();

struct Member {
    QString path;
    qint64 bytes = 0;
};

struct Item {
    QString id;
    Category category = Category::Claude;
    Kind kind = Kind::Files;
    QString title;
    QString path;
    QString parent;
    QList<Member> members;
    qint64 bytes = 0;
    qint64 keptBytes = 0;
    qint64 files = 0;
    qint64 modifiedMs = 0;
    Status status = Status::Orphaned;
    QStringList reasons;
    bool confirmEach = false;
    QString repoPath;
    QString branch;
    QString baseBranch;
    int ahead = -1;
    int behind = -1;
    int dirtyFiles = 0;
    bool locked = false;
    QStringList agents;
    QStringList tasks;

    QString reasonText() const;
    bool deletable() const;
    bool needsConfirm() const;
};

struct Process {
    qint64 pid = 0;
    QString comm;
    QString cwd;
    QString exe;
    QStringList cmdline;
    QStringList openPaths;
    QString label() const;
};

struct ProcessSnapshot {
    QList<Process> processes;
    bool available = false;
    QStringList usersOf(const QString &path) const;
    QList<Process> named(const QStringList &names) const;
};

ProcessSnapshot readProcesses(const QString &procRoot = QStringLiteral("/proc"));
bool isClaudeProcess(const Process &process);
bool isCodexProcess(const Process &process);

using CancelFlag = std::shared_ptr<std::atomic_bool>;

struct Usage {
    qint64 bytes = 0;
    qint64 files = 0;
    qint64 newestMs = 0;
};

Usage measure(const QString &path, const CancelFlag &cancel = {});

struct Context {
    QString home;
    QStringList tmpRoots;
    QString uid;
    QString projectHome;
    QString dataDir;
    QString agentAccountsDir;
    QString buildsDir;
    QString worktreesDir;
    QString sandboxDir;
    QString claudeBinary;
    QString ccacheDir;
    QList<cleanupfeed::Repository> repositories;
    QList<cleanupfeed::Agent> agents;
    QList<cleanupfeed::TaskLink> tasks;
    QSet<QString> claimedSandboxes;
    ProcessSnapshot processes;
    qint64 nowMs = 0;
    int liveMinutes = 30;
    int oldSessionDays = 30;
    bool runCcache = true;
};

struct ScanResult {
    QList<Item> items;
    QStringList notes;
    bool cancelled = false;
};

using Progress = std::function<void(const QString &)>;

ScanResult scan(const Context &context, const CancelFlag &cancel = {},
                const Progress &progress = {});

QString encodeClaudeProject(const QString &cwd);
QSet<QString> uuidsIn(const QString &text);
QSet<QString> liveClaudeSessions(const Context &context);
bool pathWithin(const QString &path, const QString &root);
QString sizeText(qint64 bytes);
QString ageText(qint64 modifiedMs, qint64 nowMs);

} // namespace forkmesh::cleanup

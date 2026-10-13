#pragma once

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace forkmesh::agentparity {

inline constexpr int kCheckIntervalMs = 3 * 60 * 1000;
inline constexpr int kFirstCheckDelayMs = 60 * 1000;
inline constexpr int kMaxListedIssues = 8;

struct Agent {
    int id = 0;
    QString title;
    QString branch;
    QString folder;
    bool needsBranch = false;
    bool needsWorktree = false;
    bool recycled = false;
};

struct Worktree {
    QString path;
    QString branch;
    bool missing = false;
};

struct Issue {
    QString key;
    QString text;
};

inline QList<Worktree> parseWorktrees(const QString &porcelain)
{
    QList<Worktree> out;
    Worktree current;
    bool open = false;
    for (const QString &raw : porcelain.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("worktree "))) {
            if (open)
                out << current;
            current = Worktree{};
            current.path = line.mid(9);
            open = true;
        } else if (line.startsWith(QLatin1String("branch "))) {
            QString ref = line.mid(7);
            if (ref.startsWith(QLatin1String("refs/heads/")))
                ref = ref.mid(11);
            current.branch = ref;
        } else if (line == QLatin1String("prunable") ||
                   line.startsWith(QLatin1String("prunable "))) {
            current.missing = true;
        }
    }
    if (open)
        out << current;
    return out;
}

inline QSet<QString> parseBranches(const QString &refs)
{
    QSet<QString> out;
    for (const QString &line : refs.split(QLatin1Char('\n'))) {
        const QString name = line.trimmed();
        if (!name.isEmpty())
            out.insert(name);
    }
    return out;
}

inline bool isAgentBranchName(const QString &branch)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(?:agent-)?\\d+-[a-z0-9][a-z0-9-]*$"));
    return pattern.match(branch).hasMatch();
}

inline QString agentLabel(const Agent &agent)
{
    const QString title = agent.title.simplified();
    return title.isEmpty()
               ? QStringLiteral("Agent #%1").arg(agent.id)
               : QStringLiteral("Agent #%1 (%2)").arg(agent.id).arg(title);
}

inline QList<Issue> check(const QList<Agent> &agents,
                          const QList<Worktree> &worktrees,
                          const QSet<QString> &branches, const QString &root)
{
    QList<Issue> issues;
    const QString cleanRoot = QDir::cleanPath(root);
    QHash<QString, const Worktree *> byBranch;
    QHash<QString, const Worktree *> byFolder;
    QSet<QString> checkedOut;
    QList<const Worktree *> agentArea;
    for (const Worktree &worktree : worktrees) {
        if (!worktree.branch.isEmpty()) {
            byBranch.insert(worktree.branch, &worktree);
            checkedOut.insert(worktree.branch);
        }
        const QFileInfo info(QDir::cleanPath(worktree.path));
        if (cleanRoot.isEmpty() || info.absolutePath() != cleanRoot ||
            info.fileName().startsWith(QLatin1Char('.')))
            continue;
        byFolder.insert(info.fileName(), &worktree);
        agentArea << &worktree;
    }

    QSet<QString> claimedBranches;
    QSet<QString> liveBranches;
    QSet<QString> claimedFolders;
    for (const Agent &agent : agents) {
        if (!agent.branch.isEmpty())
            claimedBranches.insert(agent.branch);
        if (agent.recycled)
            continue;
        if (!agent.branch.isEmpty())
            liveBranches.insert(agent.branch);
        if (!agent.folder.isEmpty())
            claimedFolders.insert(agent.folder);
        if (agent.branch.isEmpty())
            continue;
        const QString label = agentLabel(agent);
        if (agent.needsBranch && !branches.contains(agent.branch))
            issues << Issue{QStringLiteral("branch:%1").arg(agent.id),
                            QStringLiteral("%1 has no branch %2.")
                                .arg(label, agent.branch)};
        if (!agent.needsWorktree)
            continue;
        const Worktree *worktree = byBranch.value(agent.branch);
        if (!worktree && !agent.folder.isEmpty())
            worktree = byFolder.value(agent.folder);
        if (!worktree)
            issues << Issue{QStringLiteral("worktree:%1").arg(agent.id),
                            QStringLiteral("%1 has no worktree for branch %2.")
                                .arg(label, agent.branch)};
        else if (worktree->missing)
            issues << Issue{QStringLiteral("worktree:%1").arg(agent.id),
                            QStringLiteral("%1: Git still lists its worktree "
                                           "%2, but the folder is gone.")
                                .arg(label, worktree->path)};
    }

    for (const Worktree *worktree : std::as_const(agentArea)) {
        const QString folder = QFileInfo(worktree->path).fileName();
        if ((!worktree->branch.isEmpty() &&
             liveBranches.contains(worktree->branch)) ||
            claimedFolders.contains(folder))
            continue;
        issues << Issue{
            QStringLiteral("orphan-worktree:%1").arg(worktree->path),
            worktree->branch.isEmpty()
                ? QStringLiteral("Worktree %1 (detached) has no agent.")
                      .arg(folder)
                : QStringLiteral("Worktree %1 on branch %2 has no agent.")
                      .arg(folder, worktree->branch)};
    }

    QStringList names(branches.cbegin(), branches.cend());
    std::sort(names.begin(), names.end());
    for (const QString &branch : std::as_const(names)) {
        if (!isAgentBranchName(branch) || claimedBranches.contains(branch) ||
            checkedOut.contains(branch))
            continue;
        issues << Issue{QStringLiteral("orphan-branch:%1").arg(branch),
                        QStringLiteral("Branch %1 has no agent or worktree.")
                            .arg(branch)};
    }
    return issues;
}

struct Finding {
    QString repo;
    Issue issue;

    QString key() const { return repo + QLatin1Char('|') + issue.key; }
};

struct Alert {
    QString title;
    QString body;

    bool isEmpty() const { return title.isEmpty(); }
};

struct Watch {
    QSet<QString> previous;
    QSet<QString> alerted;

    QSet<QString> observe(const QSet<QString> &current)
    {
        QSet<QString> fresh;
        for (const QString &key : current)
            if (previous.contains(key) && !alerted.contains(key))
                fresh.insert(key);
        alerted.intersect(current);
        alerted.unite(fresh);
        previous = current;
        return fresh;
    }

    Alert observe(const QList<Finding> &findings)
    {
        QSet<QString> current;
        QSet<QString> repos;
        for (const Finding &finding : findings) {
            current.insert(finding.key());
            repos.insert(finding.repo);
        }
        const QSet<QString> fresh = observe(current);
        if (fresh.isEmpty())
            return {};
        QStringList lines;
        int count = 0;
        for (const Finding &finding : findings) {
            if (!fresh.contains(finding.key()))
                continue;
            if (count++ >= kMaxListedIssues)
                continue;
            lines << (repos.size() > 1
                          ? QStringLiteral("%1: %2").arg(finding.repo,
                                                         finding.issue.text)
                          : finding.issue.text);
        }
        if (count > kMaxListedIssues)
            lines << QStringLiteral("…and %1 more.")
                         .arg(count - kMaxListedIssues);
        Alert alert;
        alert.title =
            count == 1
                ? QStringLiteral("An agent, branch or worktree is out of step")
                : QStringLiteral("Agents, branches and worktrees are out "
                                 "of step (%1 issues)")
                      .arg(count);
        alert.body =
            lines.join(QLatin1Char('\n')) +
            QStringLiteral("\n\nEvery agent should have its own branch and "
                           "worktree, and every agent branch and worktree "
                           "its agent. Delete what is left over, or start an "
                           "agent on it, from the Agents or Branches page.");
        return alert;
    }
};

}

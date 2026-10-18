#include "MainWindow.h"
#include "MainWindowInternal.h"

#include "GitRefusal.h"
#include "PullMergeAll.h"
#include "PullStore.h"

#include <QHash>
#include <QSet>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

#include <algorithm>

using namespace forkmesh::ui;
namespace pullmergeall = forkmesh::pullmergeall;

namespace {

const QString kPullMergeAllRunsSetting = QStringLiteral("pulls/mergeAllRuns");
constexpr int kPullMergeAllCleanupRetryMs = 3000;

struct PullMergeAllPrepared
{
    QList<pullmergeall::Target> targets;
    QStringList skipped;
};

struct PullMergeAllCleanup
{
    QList<int> merged;
    QList<int> unmerged;
    QStringList deletedBranches;
    QStringList keptBranches;
    bool ok = true;
    QString error;
};

QString pullNumberList(const QList<int> &numbers)
{
    QStringList labels;
    for (int number : numbers)
        labels << QStringLiteral("#%1").arg(number);
    return labels.join(QStringLiteral(", "));
}

}

void MainWindow::loadPullMergeAllRuns()
{
    if (m_pullMergeAllRunsLoaded)
        return;
    m_pullMergeAllRunsLoaded = true;
    m_pullMergeAllRuns = pullmergeall::decodeRuns(
        QSettings().value(kPullMergeAllRunsSetting).toByteArray());
}

void MainWindow::savePullMergeAllRuns()
{
    QSettings settings;
    if (m_pullMergeAllRuns.isEmpty())
        settings.remove(kPullMergeAllRunsSetting);
    else
        settings.setValue(kPullMergeAllRunsSetting,
                          pullmergeall::encodeRuns(m_pullMergeAllRuns));
}

int MainWindow::activePullMergeAllSession(const QString &owner, const QString &name)
{
    loadPullMergeAllRuns();
    QList<int> gone;
    int active = 0;
    for (auto it = m_pullMergeAllRuns.cbegin(); it != m_pullMergeAllRuns.cend(); ++it) {
        const AgentSession *session = findAgentSession(it.key());
        if (!session) {
            gone << it.key();
            continue;
        }
        if (it->owner != owner || it->name != name)
            continue;
        if (session->status == AgentStatus::Running ||
            session->status == AgentStatus::Queued ||
            m_pullMergeAllCleaning.contains(it.key()))
            active = it.key();
    }
    if (!gone.isEmpty() && m_agentStore) {
        for (int id : std::as_const(gone))
            m_pullMergeAllRuns.remove(id);
        savePullMergeAllRuns();
    }
    return active;
}

void MainWindow::mergeAllPullsWithAgent()
{
    if (m_pullMergeAllPreparing) {
        flashMessage(QStringLiteral("Already preparing the pull requests for the "
                                    "merge agent."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString workTree = writableRecordFor(repo).localPath;
    if (workTree.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Merge all with AI"),
                             QStringLiteral("This repository is read-only on this "
                                            "node."));
        return;
    }
    if (!currentAccountOwnsPullRepository()) {
        QMessageBox::warning(this, QStringLiteral("Merge all with AI"),
                             QStringLiteral("Only the repository's owner can merge "
                                            "every pull request at once."));
        return;
    }
    if (const int running = activePullMergeAllSession(repo.owner, repo.name);
        running > 0) {
        switchToAgentsTab(running);
        flashMessage(QStringLiteral("Agent #%1 is already merging this "
                                    "repository's pull requests.")
                         .arg(running),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }

    QList<PullRequest> open;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number > 0 && !pr.archivedOnCodeBranch &&
            pr.status == QLatin1String("open"))
            open << pr;
    std::sort(open.begin(), open.end(),
              [](const PullRequest &a, const PullRequest &b) {
                  return a.number < b.number;
              });
    if (open.isEmpty()) {
        setRepoDetailNotice(QStringLiteral("No open pull requests to merge."));
        return;
    }
    if (open.size() > pullmergeall::kMaxTargets)
        open = open.mid(0, pullmergeall::kMaxTargets);

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Merge all with AI"),
        QStringLiteral(
            "Start an AI agent in %1 with no branch and no permission prompts?\n\n"
            "It reviews all %2 open pull request(s) for security issues, bugs and "
            "code that does not fit this codebase, fixes what it can, and merges "
            "the safe ones into their base branch. When it finishes, ForkMesh "
            "deletes every pull request whose branch was merged, and that branch.\n\n"
            "Pull requests it skips stay open. The agent does not push.")
            .arg(workTree)
            .arg(open.size()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

    m_pullMergeAllPreparing = true;
    if (m_pullMergeAllAiButton)
        m_pullMergeAllAiButton->setEnabled(false);
    const PullStore store = pullStoreForCurrentRepo();
    const QString owner = repo.owner;
    const QString name = repo.name;
    runOffThread<PullMergeAllPrepared>(
        [store, workTree, open] {
            PullMergeAllPrepared out;
            for (const PullRequest &pr : open) {
                QString branch = localPullAgentBranch(workTree, pr);
                QString error;
                if (branch.isEmpty() && store.canWrite()) {
                    store.ensurePullHeadRef(pr, &error);
                    branch = localPullAgentBranch(workTree, pr);
                }
                const QString base = pr.base.trimmed().isEmpty()
                                         ? QStringLiteral("main")
                                         : pr.base.trimmed();
                QString reason;
                if (branch.isEmpty())
                    reason = error.trimmed().isEmpty()
                                 ? QStringLiteral("its code is not on this node")
                                 : error.trimmed();
                else if (!pullmergeall::safeRefName(branch))
                    reason = QStringLiteral("its branch name is unsafe");
                else if (!pullmergeall::safeRefName(base) ||
                         !repoHasLocalBranch(workTree, base))
                    reason = QStringLiteral("its base `%1` is not a local branch")
                                 .arg(base);
                else if (branch == base)
                    reason = QStringLiteral("its branch is its base");
                if (!reason.isEmpty()) {
                    out.skipped << QStringLiteral("#%1 (%2)").arg(pr.number).arg(reason);
                    continue;
                }
                pullmergeall::Target target;
                target.number = pr.number;
                target.title = pr.title;
                target.authorName = pr.authorName;
                target.branch = branch;
                target.base = base;
                out.targets << target;
            }
            return out;
        },
        [this, owner, name](PullMergeAllPrepared prepared) {
            m_pullMergeAllPreparing = false;
            if (m_pullMergeAllAiButton)
                m_pullMergeAllAiButton->setEnabled(true);
            launchPullMergeAllAgent(owner, name, prepared.targets, prepared.skipped);
        });
}

void MainWindow::launchPullMergeAllAgent(
    const QString &owner, const QString &name,
    const QList<pullmergeall::Target> &targets, const QStringList &skipped)
{
    if (targets.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Merge all with AI"),
            QStringLiteral("None of the open pull requests can be merged by an "
                           "agent on this node:\n%1")
                .arg(skipped.join(QLatin1Char('\n'))));
        return;
    }
    const int repoIndex = repoIndexFor(owner, name);
    if (repoIndex < 0)
        return;
    const QString checkout = writableRecordFor(m_repositories.at(repoIndex)).localPath;

    QString provider = defaultAgentProvider();
    if (provider != QLatin1String("claude-code") && !agentIsCodexProvider(provider))
        provider = QStringLiteral("claude-code");
    const QString model =
        provider == QLatin1String("claude-code")
            ? QSettings().value(kClaudeCodeModelSetting).toString()
            : codexChatGptModelId(QSettings().value(kCodexModelSetting).toString());

    const int sessionId = startAdHocAgentForRepo(
        repoIndex, pullmergeall::agentPrompt(owner, name, checkout, targets),
        provider, false, model, pullmergeall::runTitle(targets.size()),
        false, true, QString(), QStringLiteral("Auto"),
        QString(), 50, true);
    if (sessionId <= 0)
        return;

    loadPullMergeAllRuns();
    pullmergeall::Run run;
    run.owner = owner;
    run.name = name;
    run.targets = targets;
    m_pullMergeAllRuns.insert(sessionId, run);
    savePullMergeAllRuns();

    if (AgentSession *session = findAgentSession(sessionId); session && m_agentStore) {
        QList<int> numbers;
        for (const pullmergeall::Target &target : targets)
            numbers << target.number;
        m_agentStore->appendLog(
            *session, QStringLiteral("==> Merge all: reviewing and merging %1.\n")
                          .arg(pullNumberList(numbers)));
        if (!skipped.isEmpty())
            m_agentStore->appendLog(
                *session, QStringLiteral("==> Merge all: left out %1.\n")
                              .arg(skipped.join(QStringLiteral(", "))));
    }
    setRepoDetailNotice(
        skipped.isEmpty()
            ? QStringLiteral("Agent #%1 is reviewing and merging %2 pull "
                             "request(s).")
                  .arg(sessionId)
                  .arg(targets.size())
            : QStringLiteral("Agent #%1 is reviewing and merging %2 pull "
                             "request(s). Left out: %3")
                  .arg(sessionId)
                  .arg(targets.size())
                  .arg(skipped.join(QStringLiteral(", "))));
}

void MainWindow::finishPullMergeAllRun(int sessionId)
{
    loadPullMergeAllRuns();
    const auto found = m_pullMergeAllRuns.constFind(sessionId);
    if (found == m_pullMergeAllRuns.constEnd() ||
        m_pullMergeAllCleaning.contains(sessionId))
        return;
    const pullmergeall::Run run = found.value();
    const int repoIndex = repoIndexFor(run.owner, run.name);
    if (repoIndex < 0) {
        logSystem(QStringLiteral("Merge all: %1/%2 is no longer open here, so the "
                                 "pull requests agent #%3 merged were not cleaned "
                                 "up.")
                      .arg(run.owner, run.name)
                      .arg(sessionId));
        return;
    }
    if (m_pullDeleteInProgress) {
        QTimer::singleShot(kPullMergeAllCleanupRetryMs, this,
                           [this, sessionId] { finishPullMergeAllRun(sessionId); });
        return;
    }
    const QString dir = writableRecordFor(m_repositories.at(repoIndex)).localPath;
    PullStore store = pullStoreForRepoIndex(repoIndex);
    m_pullMergeAllCleaning.insert(sessionId);
    m_pullDeleteInProgress = true;
    setPullDeleteButtonsEnabled(false);

    runOffThread<PullMergeAllCleanup>(
        [store, dir, run]() mutable {
            PullMergeAllCleanup out;
            QByteArray worktrees;
            runGitCapture(dir, {QStringLiteral("worktree"), QStringLiteral("list"),
                                QStringLiteral("--porcelain")},
                          &worktrees, nullptr);
            const QSet<QString> checkedOut =
                forkmesh::gitrefusal::branchesCheckedOutInWorktrees(
                    QString::fromUtf8(worktrees));
            QSet<QString> bases;
            for (const pullmergeall::Target &target : std::as_const(run.targets))
                bases.insert(target.base);
            QHash<QString, QString> proven;
            for (const pullmergeall::Target &target : std::as_const(run.targets)) {
                const QString head = QStringLiteral("refs/heads/") + target.branch;
                QByteArray oid;
                const bool merged =
                    runGitCapture(dir,
                                  {QStringLiteral("rev-parse"),
                                   QStringLiteral("--verify"), QStringLiteral("--quiet"),
                                   head + QStringLiteral("^{commit}")},
                                  &oid, nullptr) &&
                    runGitCapture(dir,
                                  {QStringLiteral("merge-base"),
                                   QStringLiteral("--is-ancestor"), head,
                                   QStringLiteral("refs/heads/") + target.base},
                                  nullptr, nullptr);
                if (!merged) {
                    out.unmerged << target.number;
                    continue;
                }
                out.merged << target.number;
                proven.insert(target.branch, QString::fromUtf8(oid).trimmed());
            }
            if (out.merged.isEmpty())
                return out;
            QList<int> deleted;
            QList<int> missing;
            QString error;
            if (!store.deletePulls(out.merged, false, &deleted,
                                   &missing, &error)) {
                out.ok = false;
                out.error = error;
                return out;
            }
            for (auto it = proven.cbegin(); it != proven.cend(); ++it) {
                const QString &branch = it.key();
                if (!repoHasLocalBranch(dir, branch))
                    continue;
                if (bases.contains(branch) || checkedOut.contains(branch) ||
                    it.value().isEmpty()) {
                    out.keptBranches << branch;
                    continue;
                }
                if (runGitCapture(dir,
                                  {QStringLiteral("update-ref"), QStringLiteral("-d"),
                                   QStringLiteral("refs/heads/") + branch, it.value()},
                                  nullptr, nullptr))
                    out.deletedBranches << branch;
                else
                    out.keptBranches << branch;
            }
            return out;
        },
        [this, sessionId, run](PullMergeAllCleanup out) {
            m_pullMergeAllCleaning.remove(sessionId);
            m_pullDeleteInProgress = false;
            setPullDeleteButtonsEnabled(true);
            const bool onScreen = repoIndexFor(run.owner, run.name) == m_repoDetailIndex;
            if (!out.ok) {
                const QString message =
                    QStringLiteral("Merge all: agent #%1 finished, but deleting the "
                                   "merged pull requests failed: %2")
                        .arg(sessionId)
                        .arg(out.error.isEmpty() ? QStringLiteral("unknown error")
                                                 : out.error);
                logSystem(message);
                if (onScreen) {
                    reloadPulls();
                    setRepoDetailNotice(message, true,
                                        forkmesh::kLocalOperationNoticeKind);
                }
                return;
            }
            m_pullMergeAllRuns.remove(sessionId);
            savePullMergeAllRuns();
            QStringList parts;
            parts << (out.merged.isEmpty()
                          ? QStringLiteral("Merge all: agent #%1 merged no pull "
                                           "requests.")
                                .arg(sessionId)
                          : QStringLiteral("Merge all: agent #%1 merged and deleted "
                                           "%2.")
                                .arg(sessionId)
                                .arg(pullNumberList(out.merged)));
            if (!out.deletedBranches.isEmpty())
                parts << QStringLiteral("Deleted %1 branch(es).")
                             .arg(out.deletedBranches.size());
            if (!out.unmerged.isEmpty())
                parts << QStringLiteral("Still open: %1.")
                             .arg(pullNumberList(out.unmerged));
            if (!out.keptBranches.isEmpty())
                parts << QStringLiteral("Kept branch(es) still in use: %1.")
                             .arg(out.keptBranches.join(QStringLiteral(", ")));
            const QString summary = parts.join(QLatin1Char(' '));
            logSystem(summary);
            if (AgentSession *session = findAgentSession(sessionId);
                session && m_agentStore)
                m_agentStore->appendLog(*session,
                                        QStringLiteral("==> %1\n").arg(summary));
            if (!onScreen)
                return;
            if (out.merged.contains(m_currentPullNumber))
                m_currentPullNumber = -1;
            if (!out.deletedBranches.isEmpty())
                loadBranchesAndTags();
            reloadPulls();
            setRepoDetailNotice(summary);
        });
}

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

bool createPullBranchAt(const QString &workTree, int number, const QString &sha)
{
    const QString branch = QStringLiteral("pr/%1").arg(number);
    if (sha.isEmpty() ||
        !runGitCapture(workTree, {QStringLiteral("cat-file"), QStringLiteral("-e"),
                                  sha + QStringLiteral("^{commit}")},
                       nullptr, nullptr))
        return false;
    runGitCapture(workTree,
                  {QStringLiteral("update-ref"), QStringLiteral("refs/heads/") + branch,
                   sha, QString(sha.size(), QLatin1Char('0'))},
                  nullptr, nullptr);
    return repoHasLocalBranch(workTree, branch);
}

QString adoptTrackedPullBranch(const QString &workTree, int number)
{
    for (const QString &ref : pullmergeall::trackedPullBranchRefs(number)) {
        QByteArray out;
        if (!runGitCapture(workTree,
                           {QStringLiteral("rev-parse"), QStringLiteral("-q"),
                            QStringLiteral("--verify"), ref + QStringLiteral("^{commit}")},
                           &out, nullptr))
            continue;
        if (createPullBranchAt(workTree, number, QString::fromUtf8(out).trimmed()))
            return QStringLiteral("pr/%1").arg(number);
    }
    return QString();
}

QHash<int, QString> fetchServedPullBranches(const QString &workTree,
                                            const QList<int> &numbers)
{
    QByteArray remote;
    if (numbers.isEmpty() ||
        !runGitCapture(workTree, {QStringLiteral("remote"), QStringLiteral("get-url"),
                                  QStringLiteral("origin")},
                       &remote, nullptr) ||
        remote.trimmed().isEmpty())
        return {};
    QByteArray listing;
    if (!runGitCapture(workTree,
                       QStringList{QStringLiteral("ls-remote"), QStringLiteral("origin")} +
                           pullmergeall::servedPullBranchRefs(numbers),
                       &listing, nullptr))
        return {};
    const QHash<int, QString> tips = pullmergeall::servedPullBranchTips(listing, numbers);
    if (tips.isEmpty())
        return {};
    QStringList refs;
    for (auto it = tips.cbegin(); it != tips.cend(); ++it)
        refs << QStringLiteral("refs/heads/pr/%1").arg(it.key());
    refs.sort();
    runGitCapture(workTree,
                  QStringList{QStringLiteral("fetch"), QStringLiteral("--no-tags"),
                              QStringLiteral("--quiet"),
                              QStringLiteral("--no-write-fetch-head"),
                              QStringLiteral("origin")} +
                      refs,
                  nullptr, nullptr);
    return tips;
}

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

QString MainWindow::pullMergeAllBlocker(int repoIndex)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return QStringLiteral("unknown_repo");
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    if (writableRecordFor(repo).localPath.isEmpty())
        return QStringLiteral("read_only");
    if (!accountOwnsPullRepositoryAt(repoIndex))
        return QStringLiteral("not_repo_owner");
    if (m_pullMergeAllPreparing || activePullMergeAllSession(repo.owner, repo.name) > 0)
        return QStringLiteral("busy");
    return QString();
}

QList<PullRequest> MainWindow::openPullsToMergeAll(const QList<PullRequest> &pulls)
{
    QList<PullRequest> open;
    for (const PullRequest &pr : pulls)
        if (pr.number > 0 && !pr.archivedOnCodeBranch &&
            pr.status == QLatin1String("open"))
            open << pr;
    std::sort(open.begin(), open.end(),
              [](const PullRequest &a, const PullRequest &b) {
                  return a.number < b.number;
              });
    if (open.size() > pullmergeall::kMaxTargets)
        open = open.mid(0, pullmergeall::kMaxTargets);
    return open;
}

void MainWindow::mergeAllPullsWithAgent()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString blocker = pullMergeAllBlocker(m_repoDetailIndex);
    if (blocker == QLatin1String("busy")) {
        const int running = activePullMergeAllSession(repo.owner, repo.name);
        if (running > 0)
            switchToAgentsTab(running);
        flashMessage(pullmergeall::errorMessage(blocker), true, QString(),
                     forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (!blocker.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Merge all with AI"),
                             pullmergeall::errorMessage(blocker));
        return;
    }
    const QList<PullRequest> open = openPullsToMergeAll(m_currentPulls);
    if (open.isEmpty()) {
        setRepoDetailNotice(pullmergeall::errorMessage(QStringLiteral("no_open_pulls")));
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Merge all with AI"),
        QStringLiteral(
            "Start an AI agent in %1 with no branch and no permission prompts?\n\n"
            "It reviews all %2 open pull request(s) for security issues, bugs and "
            "code that does not fit this codebase, fixes what it can, and merges "
            "the safe ones into their base branch. When it finishes, ForkMesh "
            "deletes every pull request whose branch was merged, and that branch.\n\n"
            "Pull requests it skips stay open. The agent does not push.")
            .arg(writableRecordFor(repo).localPath)
            .arg(open.size()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    startPullMergeAll(repo.owner, repo.name, open, true,
                      [this](const pullmergeall::Launch &launch) {
                          if (launch.error.isEmpty() ||
                              launch.error == QLatin1String("launch_failed"))
                              return;
                          QMessageBox::warning(this, QStringLiteral("Merge all with AI"),
                                               pullmergeall::launchMessage(launch));
                      });
}

void MainWindow::startPullMergeAll(
    const QString &owner, const QString &name, const QList<PullRequest> &open,
    bool switchToTab, std::function<void(const pullmergeall::Launch &)> done)
{
    const int repoIndex = repoIndexFor(owner, name);
    pullmergeall::Launch refused;
    refused.error = pullMergeAllBlocker(repoIndex);
    if (refused.error.isEmpty() && open.isEmpty())
        refused.error = QStringLiteral("no_open_pulls");
    if (!refused.error.isEmpty()) {
        if (done)
            done(refused);
        return;
    }
    const QString workTree = writableRecordFor(m_repositories.at(repoIndex)).localPath;
    const PullStore store = pullStoreForRepoIndex(repoIndex);
    m_pullMergeAllPreparing = true;
    if (m_pullMergeAllAiButton)
        m_pullMergeAllAiButton->setEnabled(false);
    runOffThread<PullMergeAllPrepared>(
        [store, workTree, open] {
            PullMergeAllPrepared out;
            QList<int> absent;
            for (const PullRequest &pr : open)
                if (localPullAgentBranch(workTree, pr).isEmpty())
                    absent << pr.number;
            const QHash<int, QString> served =
                fetchServedPullBranches(workTree, absent);
            for (const PullRequest &pr : open) {
                QString branch = localPullAgentBranch(workTree, pr);
                QString error;
                if (branch.isEmpty() && store.canWrite()) {
                    store.ensurePullHeadRef(pr, &error);
                    branch = localPullAgentBranch(workTree, pr);
                }
                if (branch.isEmpty() && served.contains(pr.number) &&
                    createPullBranchAt(workTree, pr.number, served.value(pr.number)))
                    branch = QStringLiteral("pr/%1").arg(pr.number);
                if (branch.isEmpty())
                    branch = adoptTrackedPullBranch(workTree, pr.number);
                const QString base = pr.base.trimmed().isEmpty()
                                         ? QStringLiteral("main")
                                         : pr.base.trimmed();
                QString reason;
                if (branch.isEmpty())
                    reason = error.trimmed().isEmpty()
                                 ? pullmergeall::missingCodeReason(
                                       pr.branchBacked, pr.head, base)
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
        [this, owner, name, switchToTab, done](PullMergeAllPrepared prepared) {
            m_pullMergeAllPreparing = false;
            if (m_pullMergeAllAiButton)
                m_pullMergeAllAiButton->setEnabled(true);
            const pullmergeall::Launch launch = launchPullMergeAllAgent(
                owner, name, prepared.targets, prepared.skipped, switchToTab);
            if (done)
                done(launch);
        });
}

pullmergeall::Launch MainWindow::launchPullMergeAllAgent(
    const QString &owner, const QString &name,
    const QList<pullmergeall::Target> &targets, const QStringList &skipped,
    bool switchToTab)
{
    pullmergeall::Launch launch;
    launch.skipped = skipped;
    if (targets.isEmpty()) {
        launch.error = QStringLiteral("nothing_mergeable");
        return launch;
    }
    const int repoIndex = repoIndexFor(owner, name);
    if (repoIndex < 0) {
        launch.error = QStringLiteral("unknown_repo");
        return launch;
    }
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
        false, switchToTab, QString(), QStringLiteral("Auto"),
        QString(), 50, true);
    if (sessionId <= 0) {
        launch.error = QStringLiteral("launch_failed");
        return launch;
    }
    launch.sessionId = sessionId;
    launch.count = targets.size();

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
    if (repoIndex == m_repoDetailIndex)
        setRepoDetailNotice(pullmergeall::launchMessage(launch));
    return launch;
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

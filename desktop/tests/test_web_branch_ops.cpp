#include "WebBranchOps.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdio>

// Branch management typed on the website (adhoc #2098) is queued for the node
// that owns the repository's checkout, and applied here. The website never
// sees this checkout, so every request is treated as network data: the tip the
// requester saw has to still be the tip, the default branch and any
// checked-out branch are untouchable, and a conflicting merge is refused
// rather than half-applied.

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool git(const QString &path, const QStringList &arguments,
         QByteArray *output = nullptr)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"),
                       QStringLiteral("Branch Ops Test"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                       QStringLiteral("branch-ops@example.invalid"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"),
                       QStringLiteral("Branch Ops Test"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                       QStringLiteral("branch-ops@example.invalid"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"),
                       QStringLiteral("/dev/null"));
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"),
                       QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(path);
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForFinished(60000))
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return process.exitStatus() == QProcess::NormalExit
           && process.exitCode() == 0;
}

QString revision(const QString &path, const QString &name)
{
    QByteArray output;
    if (!git(path, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                    QStringLiteral("--quiet"), name},
             &output))
        return QString();
    return QString::fromUtf8(output).trimmed();
}

void writeFile(const QString &path, const QString &name, const QString &text)
{
    QFile file(QDir(path).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(text.toUtf8());
    file.close();
}

void commit(const QString &path, const QString &name, const QString &text,
            const QString &message)
{
    writeFile(path, name, text);
    git(path, {QStringLiteral("add"), name});
    git(path, {QStringLiteral("commit"), QStringLiteral("-m"), message});
}

QJsonObject request(const QString &op, const QString &branch,
                    const QString &oidField = QString(),
                    const QString &oid = QString(),
                    const QString &base = QString())
{
    QJsonObject value{{QStringLiteral("op"), op},
                      {QStringLiteral("branch"), branch}};
    if (!oidField.isEmpty())
        value.insert(oidField, oid);
    if (!base.isEmpty())
        value.insert(QStringLiteral("base"), base);
    return value;
}

// main, one merged branch, one branch with its own commit, one that conflicts.
QString makeCheckout(QTemporaryDir &root)
{
    const QString path = QDir(root.path()).filePath(QStringLiteral("checkout"));
    QDir().mkpath(path);
    git(path, {QStringLiteral("init"), QStringLiteral("-b"),
               QStringLiteral("main"), QStringLiteral(".")});
    commit(path, QStringLiteral("README.md"), QStringLiteral("first\n"),
           QStringLiteral("First"));
    git(path, {QStringLiteral("branch"), QStringLiteral("agent/merged")});
    git(path, {QStringLiteral("switch"), QStringLiteral("-c"),
               QStringLiteral("feature/own")});
    commit(path, QStringLiteral("feature.txt"), QStringLiteral("feature\n"),
           QStringLiteral("Feature work"));
    git(path, {QStringLiteral("switch"), QStringLiteral("-c"),
               QStringLiteral("feature/conflict"), QStringLiteral("main")});
    commit(path, QStringLiteral("README.md"), QStringLiteral("branch side\n"),
           QStringLiteral("Branch side"));
    git(path, {QStringLiteral("switch"), QStringLiteral("main")});
    commit(path, QStringLiteral("README.md"), QStringLiteral("main side\n"),
           QStringLiteral("Main side"));
    return path;
}

void testDeleteRequiresTheExactTip()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    const QString tip = revision(path, QStringLiteral("refs/heads/agent/merged"));

    const auto moved = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("delete"), QStringLiteral("agent/merged"),
                QStringLiteral("expectedOid"), QString(40, QLatin1Char('a'))));
    check(!moved.applied, "a delete naming the wrong tip is refused");
    check(moved.message.contains(QStringLiteral("moved")),
          "the refusal says the branch moved");
    check(!revision(path, QStringLiteral("refs/heads/agent/merged")).isEmpty(),
          "the branch survives a refused delete");

    const auto applied = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("delete"), QStringLiteral("agent/merged"),
                QStringLiteral("expectedOid"), tip));
    check(applied.applied, "a delete naming the current tip is applied");
    check(revision(path, QStringLiteral("refs/heads/agent/merged")).isEmpty(),
          "the branch is gone");

    // Replaying the same queued request is a no-op, not a failure: the queue
    // is drained on read, and an operation may be redelivered.
    const auto again = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("delete"), QStringLiteral("agent/merged"),
                QStringLiteral("expectedOid"), tip));
    check(!again.applied && again.alreadyDone,
          "replaying a delete reports it was already done");
}

void testProtectedBranchesAreNeverDeleted()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    const QString main = revision(path, QStringLiteral("refs/heads/main"));
    const auto defaultBranch = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("delete"), QStringLiteral("main"),
                QStringLiteral("expectedOid"), main));
    check(!defaultBranch.applied, "the default branch is never deleted");
    check(!revision(path, QStringLiteral("refs/heads/main")).isEmpty(),
          "main survives");

    // main is checked out here, so even asking under another default name
    // (a repository whose default branch was renamed) must not delete it.
    const auto checkedOut = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("trunk"),
        request(QStringLiteral("delete"), QStringLiteral("main"),
                QStringLiteral("expectedOid"), main));
    check(!checkedOut.applied && checkedOut.message.contains(
              QStringLiteral("checked out")),
          "a checked-out branch is never deleted");

    QTemporaryDir worktreeRoot;
    const QString linked =
        QDir(worktreeRoot.path()).filePath(QStringLiteral("linked"));
    git(path, {QStringLiteral("worktree"), QStringLiteral("add"), linked,
               QStringLiteral("feature/own")});
    const auto busy = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("delete"), QStringLiteral("feature/own"),
                QStringLiteral("expectedOid"),
                revision(path, QStringLiteral("refs/heads/feature/own"))));
    check(!busy.applied && busy.message.contains(QStringLiteral("worktree")),
          "a branch live in an agent worktree is never deleted");
}

void testUpdateFastForwardsMergesAndRefusesConflicts()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    const QString main = revision(path, QStringLiteral("refs/heads/main"));

    // Nothing of its own: the branch simply becomes main.
    git(path, {QStringLiteral("branch"), QStringLiteral("agent/behind"),
               QStringLiteral("main~1")});
    const auto fastForward = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("update"), QStringLiteral("agent/behind"),
                QStringLiteral("expectedOid"),
                revision(path, QStringLiteral("refs/heads/agent/behind"))));
    check(fastForward.applied, "a branch with no work of its own is updated");
    check(revision(path, QStringLiteral("refs/heads/agent/behind")) == main,
          "the update fast-forwards it onto main");

    // Its own commit plus main's: a real merge, recorded without checking
    // anything out, with the branch's own history as the first parent.
    const QString ownBefore = revision(path, QStringLiteral("refs/heads/feature/own"));
    const auto merged = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("update"), QStringLiteral("feature/own"),
                QStringLiteral("expectedOid"), ownBefore));
    check(merged.applied, "a diverged branch is merged from main");
    const QString ownAfter = revision(path, QStringLiteral("refs/heads/feature/own"));
    check(ownAfter != ownBefore, "the branch moved");
    check(revision(path, QStringLiteral("refs/heads/feature/own^1")) == ownBefore,
          "its own history stays the first parent");
    check(revision(path, QStringLiteral("refs/heads/feature/own^2")) == main,
          "main is the second parent");
    int contained = 0;
    check(git(path, {QStringLiteral("merge-base"),
                     QStringLiteral("--is-ancestor"), main,
                     QStringLiteral("refs/heads/feature/own")}),
          "the merged branch now contains main");
    (void)contained;

    // Asking again changes nothing.
    const auto current = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("update"), QStringLiteral("feature/own"),
                QStringLiteral("expectedOid"), ownAfter));
    check(!current.applied && current.alreadyDone,
          "an already-current branch reports nothing to do");

    // A conflicting merge is refused outright: no half-merged ref, no commit.
    const QString conflictBefore =
        revision(path, QStringLiteral("refs/heads/feature/conflict"));
    const auto conflicted = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("update"), QStringLiteral("feature/conflict"),
                QStringLiteral("expectedOid"), conflictBefore));
    check(!conflicted.applied, "a conflicting update is refused");
    check(conflicted.message.contains(QStringLiteral("conflict")),
          "the refusal says why");
    check(revision(path, QStringLiteral("refs/heads/feature/conflict"))
              == conflictBefore,
          "the conflicting branch is untouched");
}

void testCreateStartsAtAnExistingCommit()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    const QString main = revision(path, QStringLiteral("refs/heads/main"));

    const auto created = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("create"), QStringLiteral("release/2026-09"),
                QStringLiteral("startOid"), main));
    check(created.applied, "a branch is created at the requested commit");
    check(revision(path, QStringLiteral("refs/heads/release/2026-09")) == main,
          "it points at that commit");

    const auto replay = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("create"), QStringLiteral("release/2026-09"),
                QStringLiteral("startOid"), main));
    check(!replay.applied && replay.alreadyDone,
          "replaying a create reports it was already done");

    const auto occupied = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("create"), QStringLiteral("feature/own"),
                QStringLiteral("startOid"), main));
    check(!occupied.applied && !occupied.alreadyDone,
          "an existing branch is never moved by a create");
    check(revision(path, QStringLiteral("refs/heads/feature/own")) != main,
          "the existing branch keeps its own tip");

    const auto unknown = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("create"), QStringLiteral("release/ghost"),
                QStringLiteral("startOid"), QString(40, QLatin1Char('f'))));
    check(!unknown.applied, "a start commit this node does not have is refused");
}

void testHostileRequestsAreRejectedBeforeGitRuns()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    const QString main = revision(path, QStringLiteral("refs/heads/main"));
    const QStringList refused{
        QStringLiteral(""),
        QStringLiteral("-delete"),
        QStringLiteral(".hidden"),
        QStringLiteral("deep/../escape"),
        QStringLiteral("with space"),
        QStringLiteral("trailing.lock"),
        QStringLiteral("reflog@{0}"),
        QStringLiteral("forkmesh/pulls"),
        QStringLiteral("forkmesh/pull/12"),
    };
    for (const QString &branch : refused) {
        check(!forkmesh::branchops::usableBranchName(branch),
              "an unusable branch name is rejected");
        const auto outcome = forkmesh::branchops::applyBranchOp(
            path, QStringLiteral("main"),
            request(QStringLiteral("delete"), branch,
                    QStringLiteral("expectedOid"), main));
        check(!outcome.applied, "no unusable name reaches git");
    }
    check(forkmesh::branchops::usableBranchName(QStringLiteral("agent/2098-x")),
          "an ordinary agent branch name is usable");

    const auto unsupported = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        request(QStringLiteral("rebase"), QStringLiteral("feature/own"),
                QStringLiteral("expectedOid"), main));
    check(!unsupported.applied, "an unsupported operation does nothing");

    const auto missingCheckout = forkmesh::branchops::applyBranchOp(
        QDir(root.path()).filePath(QStringLiteral("absent")),
        QStringLiteral("main"),
        request(QStringLiteral("delete"), QStringLiteral("feature/own"),
                QStringLiteral("expectedOid"), main));
    check(!missingCheckout.applied, "a repository without a checkout is skipped");
}

void testABatchAppliesEveryUsableRequest()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    QJsonArray ops;
    ops.append(request(QStringLiteral("delete"), QStringLiteral("agent/merged"),
                       QStringLiteral("expectedOid"),
                       revision(path, QStringLiteral("refs/heads/agent/merged"))));
    ops.append(request(QStringLiteral("create"), QStringLiteral("release/x"),
                       QStringLiteral("startOid"),
                       revision(path, QStringLiteral("refs/heads/main"))));
    ops.append(request(QStringLiteral("delete"), QStringLiteral("main"),
                       QStringLiteral("expectedOid"),
                       revision(path, QStringLiteral("refs/heads/main"))));
    const auto outcomes =
        forkmesh::branchops::applyBranchOps(path, QStringLiteral("main"), ops);
    check(outcomes.size() == 3, "every request is answered");
    check(outcomes.at(0).applied, "the delete landed");
    check(outcomes.at(1).applied, "the create landed");
    check(!outcomes.at(2).applied, "the default-branch delete was refused");
    check(revision(path, QStringLiteral("refs/heads/agent/merged")).isEmpty(),
          "the deleted branch is gone");
    check(!revision(path, QStringLiteral("refs/heads/release/x")).isEmpty(),
          "the created branch exists");
    check(!revision(path, QStringLiteral("refs/heads/main")).isEmpty(),
          "main survives the batch");
}

QJsonObject mergeRequest(const QString &path, const QString &branch,
                         const QString &base, bool deleteBranch)
{
    QJsonObject value = request(
        QStringLiteral("merge"), branch, QStringLiteral("expectedOid"),
        revision(path, QStringLiteral("refs/heads/") + branch), base);
    value.insert(QStringLiteral("baseOid"),
                 revision(path, QStringLiteral("refs/heads/") + base));
    value.insert(QStringLiteral("deleteBranch"), deleteBranch);
    value.insert(QStringLiteral("id"), 41);
    return value;
}

QString readFile(const QString &path, const QString &name)
{
    QFile file(QDir(path).filePath(name));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

void testMergeLandsABranchInTheCheckedOutDefaultBranch()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    const QString mainBefore = revision(path, QStringLiteral("refs/heads/main"));
    const QString own = revision(path, QStringLiteral("refs/heads/feature/own"));

    const auto merged = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("feature/own"), QStringLiteral("main"),
                     false));
    check(merged.applied, "a clean branch merges into main");
    check(merged.requestId == 41, "the outcome names the relay request");
    check(merged.base == QStringLiteral("main"), "the outcome names the base");
    const QString mainAfter = revision(path, QStringLiteral("refs/heads/main"));
    check(merged.mergeOid == mainAfter, "main moved to the merge commit");
    check(revision(path, QStringLiteral("refs/heads/main^1")) == mainBefore,
          "main's own history stays the first parent");
    check(revision(path, QStringLiteral("refs/heads/main^2")) == own,
          "the branch is the second parent");
    QByteArray subject;
    git(path, {QStringLiteral("log"), QStringLiteral("-1"),
               QStringLiteral("--format=%s"), QStringLiteral("main")},
        &subject);
    check(QString::fromUtf8(subject).trimmed()
              == QStringLiteral("Merge feature/own into main"),
          "the merge commit reads like the desktop's own");
    check(readFile(path, QStringLiteral("feature.txt"))
              == QStringLiteral("feature\n"),
          "the checked-out working tree follows the merge");
    QByteArray status;
    git(path, {QStringLiteral("status"), QStringLiteral("--porcelain")},
        &status);
    check(status.trimmed().isEmpty(), "the checkout is left clean");
    check(!revision(path, QStringLiteral("refs/heads/feature/own")).isEmpty(),
          "without clean-up the branch is kept");
    check(!merged.cleanupRequested, "no clean-up was asked for");

    const QJsonObject result = forkmesh::branchops::resultJson(merged);
    check(result.value(QStringLiteral("status")).toString()
              == QStringLiteral("applied"),
          "the report says it was applied");
    check(result.value(QStringLiteral("requestId")).toInt() == 41,
          "the report carries the request id");
    check(result.value(QStringLiteral("mergeOid")).toString() == mainAfter,
          "the report carries the merge commit");
}

void testMergeRefusesWhatTheDesktopWouldRefuse()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    const QString mainBefore = revision(path, QStringLiteral("refs/heads/main"));

    const auto conflicted = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("feature/conflict"),
                     QStringLiteral("main"), true));
    check(!conflicted.applied && !conflicted.alreadyDone,
          "a conflicting merge is refused");
    check(conflicted.message.contains(QStringLiteral("conflicts")),
          "the refusal names the conflict");
    check(revision(path, QStringLiteral("refs/heads/main")) == mainBefore,
          "main is untouched by a refused merge");
    check(forkmesh::branchops::resultJson(conflicted)
                  .value(QStringLiteral("status"))
                  .toString()
              == QStringLiteral("refused"),
          "the report says it was refused");

    QJsonObject moved = mergeRequest(path, QStringLiteral("feature/own"),
                                     QStringLiteral("main"), false);
    moved.insert(QStringLiteral("baseOid"), QString(40, QLatin1Char('a')));
    const auto baseMoved = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"), moved);
    check(!baseMoved.applied && baseMoved.message.contains(QStringLiteral("moved")),
          "a base that moved since the request is refused");

    QJsonObject noBase = mergeRequest(path, QStringLiteral("feature/own"),
                                      QStringLiteral("main"), false);
    noBase.remove(QStringLiteral("baseOid"));
    check(!forkmesh::branchops::applyBranchOp(path, QStringLiteral("main"),
                                              noBase)
               .applied,
          "a merge without the base tip is refused");

    git(path, {QStringLiteral("branch"), QStringLiteral("develop")});
    const auto defaultSource = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("main"), QStringLiteral("develop"),
                     true));
    check(!defaultSource.applied,
          "the default branch is never merged away and deleted");

    writeFile(path, QStringLiteral("feature.txt"), QStringLiteral("local\n"));
    const auto dirty = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("feature/own"),
                     QStringLiteral("main"), false));
    check(!dirty.applied, "local changes the merge would overwrite refuse it");
    check(revision(path, QStringLiteral("refs/heads/main")) == mainBefore,
          "main is untouched when the checkout is in the way");
    check(readFile(path, QStringLiteral("feature.txt"))
              == QStringLiteral("local\n"),
          "the owner's local file is never overwritten");
}

void testMergeIntoABranchNobodyHasCheckedOut()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    git(path, {QStringLiteral("branch"), QStringLiteral("release/1.0"),
               QStringLiteral("main~1")});
    const QString releaseBefore =
        revision(path, QStringLiteral("refs/heads/release/1.0"));
    const auto merged = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("feature/own"),
                     QStringLiteral("release/1.0"), false));
    check(merged.applied, "a branch merges into a branch nobody checked out");
    check(revision(path, QStringLiteral("refs/heads/release/1.0^1"))
              == releaseBefore,
          "the target's history is the first parent");
    check(revision(path, QStringLiteral("HEAD")) != merged.mergeOid,
          "the checkout's own branch does not move");
}

void testMergeAndCleanUpRemovesTheBranchAndItsWorktree()
{
    QTemporaryDir root;
    const QString path = makeCheckout(root);
    QTemporaryDir worktreeRoot;
    const QString linked =
        QDir(worktreeRoot.path()).filePath(QStringLiteral("linked"));
    git(path, {QStringLiteral("worktree"), QStringLiteral("add"), linked,
               QStringLiteral("feature/own")});
    writeFile(worktreeRoot.path(), QStringLiteral(".gitignore"),
              QStringLiteral("# ForkMesh agent worktrees. Never part of the project.\n*\n"));
    const QString scratch =
        QDir(worktreeRoot.path()).filePath(QStringLiteral(".scratch/linked"));
    QDir().mkpath(scratch);
    writeFile(scratch, QStringLiteral("build.log"), QStringLiteral("log\n"));

    auto merged = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("feature/own"),
                     QStringLiteral("main"), true));
    check(merged.applied, "a branch live in a worktree still merges");
    check(merged.cleanupRequested, "clean-up waits for the caller");
    check(!revision(path, QStringLiteral("refs/heads/feature/own")).isEmpty(),
          "the branch survives until the caller has recorded the merge");
    merged = forkmesh::branchops::cleanUpMergedBranch(
        path, QStringLiteral("main"), merged);
    check(merged.branchDeleted, "clean-up deletes the merged branch");
    check(revision(path, QStringLiteral("refs/heads/feature/own")).isEmpty(),
          "the branch is gone");
    check(revision(path, QStringLiteral("refs/forkmesh/trash/deleted/feature/own"))
              == merged.branchOid,
          "clean-up leaves a tombstone so replication cannot restore the branch");
    check(!QDir(linked).exists(), "its worktree is gone");
    check(!QDir(scratch).exists(), "its scratch folder is gone");
    check(merged.message.contains(QStringLiteral("worktree")),
          "the report says the worktree went too");

    const QString mergedTip =
        revision(path, QStringLiteral("refs/heads/agent/merged"));
    auto already = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("agent/merged"),
                     QStringLiteral("main"), true));
    check(already.alreadyDone && !already.applied,
          "a branch main already contains reports nothing to merge");
    already = forkmesh::branchops::cleanUpMergedBranch(
        path, QStringLiteral("main"), already);
    check(already.branchDeleted && !mergedTip.isEmpty(),
          "an already merged branch is still cleaned up");

    git(path, {QStringLiteral("switch"), QStringLiteral("-c"),
               QStringLiteral("feature/dirty"), QStringLiteral("main~1")});
    commit(path, QStringLiteral("dirty.txt"), QStringLiteral("dirty\n"),
           QStringLiteral("Dirty work"));
    git(path, {QStringLiteral("switch"), QStringLiteral("main")});
    const QString dirtyLinked =
        QDir(worktreeRoot.path()).filePath(QStringLiteral("dirty"));
    git(path, {QStringLiteral("worktree"), QStringLiteral("add"), dirtyLinked,
               QStringLiteral("feature/dirty")});
    writeFile(dirtyLinked, QStringLiteral("scratch.txt"),
              QStringLiteral("unsaved\n"));
    auto keep = forkmesh::branchops::applyBranchOp(
        path, QStringLiteral("main"),
        mergeRequest(path, QStringLiteral("feature/dirty"),
                     QStringLiteral("main"), true));
    check(keep.applied, "the merge itself lands");
    keep = forkmesh::branchops::cleanUpMergedBranch(
        path, QStringLiteral("main"), keep);
    check(!keep.branchDeleted, "a worktree with local changes is kept");
    check(QDir(dirtyLinked).exists(), "its files are still there");
    check(keep.message.contains(QStringLiteral("kept")),
          "the report says what was kept");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testDeleteRequiresTheExactTip();
    testProtectedBranchesAreNeverDeleted();
    testUpdateFastForwardsMergesAndRefusesConflicts();
    testCreateStartsAtAnExistingCommit();
    testHostileRequestsAreRejectedBeforeGitRuns();
    testABatchAppliesEveryUsableRequest();
    testMergeLandsABranchInTheCheckedOutDefaultBranch();
    testMergeRefusesWhatTheDesktopWouldRefuse();
    testMergeIntoABranchNobodyHasCheckedOut();
    testMergeAndCleanUpRemovesTheBranchAndItsWorktree();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stdout, "PASS: web branch operations\n");
    return 0;
}

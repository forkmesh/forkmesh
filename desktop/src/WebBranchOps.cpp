#include "WebBranchOps.h"

#include <QDir>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QHash>
#include <QSet>

namespace forkmesh::branchops {

namespace {

constexpr int kGitTimeoutMs = 60 * 1000;

// Fixed argument vectors, no hooks, no credential prompts, no ambient
// configuration: a queued operation is network data, and the only things it
// contributes are one validated branch name and one object id.
// code-health: allow(blocking-wait) only reached via applyBranchOps, whose one call site runs in QtConcurrent::run in MainWindowBranches applyWebBranchOps
bool runGit(const QString &dir, const QStringList &args, QString *output,
            QString *error = nullptr, int *exitCode = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"),
                       QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"),
                       QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("git"), args);
    if (!process.waitForFinished(kGitTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        if (error)
            *error = QStringLiteral("git did not finish in time");
        if (exitCode)
            *exitCode = -1;
        return false;
    }
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (error)
        *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    const int code = process.exitStatus() == QProcess::NormalExit
                         ? process.exitCode()
                         : -1;
    if (exitCode)
        *exitCode = code;
    return code == 0;
}

QString objectIdOf(const QString &dir, const QString &revision)
{
    QString output;
    if (!runGit(dir,
                {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                 QStringLiteral("--quiet"), QStringLiteral("--end-of-options"),
                 revision + QStringLiteral("^{commit}")},
                &output))
        return QString();
    static const QRegularExpression oid(
        QStringLiteral("\\A(?:[0-9a-f]{40}|[0-9a-f]{64})\\z"));
    return oid.match(output).hasMatch() ? output : QString();
}

QString validObjectId(const QJsonObject &op, const QString &field)
{
    static const QRegularExpression oid(
        QStringLiteral("\\A(?:[0-9a-f]{40}|[0-9a-f]{64})\\z"));
    const QString value = op.value(field).toString().trimmed().toLower();
    return oid.match(value).hasMatch() ? value : QString();
}

QString currentBranchOf(const QString &dir)
{
    QString output;
    if (!runGit(dir,
                {QStringLiteral("symbolic-ref"), QStringLiteral("--quiet"),
                 QStringLiteral("--short"), QStringLiteral("HEAD")},
                &output))
        return QString();
    return output;
}

QHash<QString, QString> worktreeCheckouts(const QString &dir)
{
    QHash<QString, QString> checkouts;
    QString output;
    if (!runGit(dir,
                {QStringLiteral("worktree"), QStringLiteral("list"),
                 QStringLiteral("--porcelain")},
                &output))
        return checkouts;
    QString path;
    for (const QString &line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        static const QLatin1String worktreePrefix("worktree ");
        static const QLatin1String branchPrefix("branch refs/heads/");
        if (trimmed.startsWith(worktreePrefix))
            path = trimmed.mid(worktreePrefix.size());
        else if (trimmed.startsWith(branchPrefix))
            checkouts.insert(trimmed.mid(branchPrefix.size()), path);
    }
    return checkouts;
}

// Every branch this checkout has checked out anywhere, including in the linked
// agent worktrees. Git refuses to move such a ref, and forcing one out from
// under a live worktree would leave that worktree's index describing a commit
// it no longer has.
QSet<QString> branchesInWorktrees(const QString &dir)
{
    const QHash<QString, QString> checkouts = worktreeCheckouts(dir);
    return QSet<QString>(checkouts.keyBegin(), checkouts.keyEnd());
}

bool samePath(const QString &left, const QString &right)
{
    const QString a = QDir(left).canonicalPath();
    const QString b = QDir(right).canonicalPath();
    return !a.isEmpty() && a == b;
}

Outcome outcomeOf(const QString &op, const QString &branch, bool applied,
                  bool alreadyDone, const QString &message)
{
    Outcome outcome;
    outcome.op = op;
    outcome.branch = branch;
    outcome.applied = applied;
    outcome.alreadyDone = alreadyDone;
    outcome.message = message;
    return outcome;
}

Outcome refusal(const QString &op, const QString &branch,
                const QString &message)
{
    return outcomeOf(op, branch, false, false, message);
}

Outcome satisfied(const QString &op, const QString &branch,
                  const QString &message)
{
    return outcomeOf(op, branch, false, true, message);
}

Outcome done(const QString &op, const QString &branch, const QString &message)
{
    return outcomeOf(op, branch, true, false, message);
}

Outcome applyDelete(const QString &dir, const QString &defaultBranch,
                    const QString &branch, const QString &expected)
{
    if (branch == defaultBranch)
        return refusal(QStringLiteral("delete"), branch,
                       QStringLiteral("refused: %1 is the default branch")
                           .arg(branch));
    const QString current = currentBranchOf(dir);
    if (!current.isEmpty() && current == branch)
        return refusal(QStringLiteral("delete"), branch,
                       QStringLiteral("refused: %1 is checked out here")
                           .arg(branch));
    if (branchesInWorktrees(dir).contains(branch))
        return refusal(
            QStringLiteral("delete"), branch,
            QStringLiteral("refused: %1 is checked out in a worktree")
                .arg(branch));
    const QString tip = objectIdOf(dir, QStringLiteral("refs/heads/") + branch);
    if (tip.isEmpty())
        return satisfied(QStringLiteral("delete"), branch,
                         QStringLiteral("already gone: %1").arg(branch));
    if (tip != expected)
        return refusal(
            QStringLiteral("delete"), branch,
            QStringLiteral("refused: %1 moved to %2 since the request")
                .arg(branch, tip.left(12)));
    QString error;
    if (!runGit(dir,
                {QStringLiteral("update-ref"), QStringLiteral("-d"),
                 QStringLiteral("refs/heads/") + branch, expected},
                nullptr, &error))
        return refusal(QStringLiteral("delete"), branch,
                       QStringLiteral("could not delete %1: %2")
                           .arg(branch, error.left(200)));
    return done(QStringLiteral("delete"), branch,
                QStringLiteral("deleted %1").arg(branch));
}

Outcome applyCreate(const QString &dir, const QString &branch,
                    const QString &start)
{
    const QString startOid = objectIdOf(dir, start);
    if (startOid.isEmpty())
        return refusal(QStringLiteral("create"), branch,
                       QStringLiteral("refused: %1 is not a commit here")
                           .arg(start.left(12)));
    const QString existing =
        objectIdOf(dir, QStringLiteral("refs/heads/") + branch);
    if (existing == startOid)
        return satisfied(QStringLiteral("create"), branch,
                         QStringLiteral("already exists: %1").arg(branch));
    if (!existing.isEmpty())
        return refusal(QStringLiteral("create"), branch,
                       QStringLiteral("refused: %1 already exists at %2")
                           .arg(branch, existing.left(12)));
    QString error;
    if (!runGit(dir,
                {QStringLiteral("update-ref"),
                 QStringLiteral("refs/heads/") + branch, startOid,
                 QString()},
                nullptr, &error))
        return refusal(QStringLiteral("create"), branch,
                       QStringLiteral("could not create %1: %2")
                           .arg(branch, error.left(200)));
    return done(QStringLiteral("create"), branch,
                QStringLiteral("created %1 at %2")
                    .arg(branch, startOid.left(12)));
}

// Merge the base branch into ``branch`` without checking anything out: the
// merge happens in the object database (merge-tree), and the ref moves only
// if it still points at the tip the requester saw.
Outcome applyUpdate(const QString &dir, const QString &defaultBranch,
                    const QString &branch, const QString &expected,
                    const QString &requestedBase)
{
    const QString base =
        requestedBase.isEmpty() ? defaultBranch : requestedBase;
    if (branch == base)
        return refusal(QStringLiteral("update"), branch,
                       QStringLiteral("refused: %1 is its own base")
                           .arg(branch));
    const QString baseOid = objectIdOf(dir, QStringLiteral("refs/heads/") + base);
    if (baseOid.isEmpty())
        return refusal(QStringLiteral("update"), branch,
                       QStringLiteral("refused: base %1 is missing").arg(base));
    const QString tip = objectIdOf(dir, QStringLiteral("refs/heads/") + branch);
    if (tip.isEmpty())
        return refusal(QStringLiteral("update"), branch,
                       QStringLiteral("refused: %1 no longer exists")
                           .arg(branch));
    if (tip != expected)
        return refusal(
            QStringLiteral("update"), branch,
            QStringLiteral("refused: %1 moved to %2 since the request")
                .arg(branch, tip.left(12)));
    int contained = -1;
    runGit(dir,
           {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
            baseOid, tip},
           nullptr, nullptr, &contained);
    if (contained == 0)
        return satisfied(
            QStringLiteral("update"), branch,
            QStringLiteral("already current: %1 contains %2")
                .arg(branch, base));
    const QString current = currentBranchOf(dir);
    const bool busy = (!current.isEmpty() && current == branch)
                      || branchesInWorktrees(dir).contains(branch);
    if (busy)
        return refusal(
            QStringLiteral("update"), branch,
            QStringLiteral("refused: %1 is checked out, so it has to be "
                           "merged from that working tree")
                .arg(branch));
    // Fast-forward when the branch has nothing of its own.
    int ancestor = -1;
    runGit(dir,
           {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), tip,
            baseOid},
           nullptr, nullptr, &ancestor);
    if (ancestor == 0) {
        QString error;
        if (!runGit(dir,
                    {QStringLiteral("update-ref"),
                     QStringLiteral("refs/heads/") + branch, baseOid, expected},
                    nullptr, &error))
            return refusal(QStringLiteral("update"), branch,
                           QStringLiteral("could not fast-forward %1: %2")
                               .arg(branch, error.left(200)));
        return done(QStringLiteral("update"), branch,
                    QStringLiteral("fast-forwarded %1 to %2")
                        .arg(branch, base));
    }
    QString tree;
    int mergeCode = -1;
    // First parent stays the branch's own history, exactly as "git merge
    // <base>" on that branch would record it.
    runGit(dir,
           {QStringLiteral("merge-tree"), QStringLiteral("--write-tree"),
            QStringLiteral("--no-messages"), tip, baseOid},
           &tree, nullptr, &mergeCode);
    const QString mergedTree = tree.split(QLatin1Char('\n')).value(0).trimmed();
    if (mergeCode != 0 || mergedTree.isEmpty())
        return refusal(
            QStringLiteral("update"), branch,
            QStringLiteral("refused: merging %1 into %2 conflicts")
                .arg(base, branch));
    QString merged;
    QString error;
    if (!runGit(dir,
                {QStringLiteral("commit-tree"), mergedTree,
                 QStringLiteral("-p"), tip, QStringLiteral("-p"), baseOid,
                 QStringLiteral("-m"),
                 QStringLiteral("Merge branch '%1' into %2").arg(base, branch)},
                &merged, &error)
        || merged.isEmpty())
        return refusal(QStringLiteral("update"), branch,
                       QStringLiteral("could not record the merge of %1: %2")
                           .arg(base, error.left(200)));
    if (!runGit(dir,
                {QStringLiteral("update-ref"),
                 QStringLiteral("refs/heads/") + branch, merged, expected},
                nullptr, &error))
        return refusal(QStringLiteral("update"), branch,
                       QStringLiteral("could not move %1: %2")
                           .arg(branch, error.left(200)));
    return done(QStringLiteral("update"), branch,
                QStringLiteral("merged %1 into %2").arg(base, branch));
}

Outcome applyMerge(const QString &dir, const QString &defaultBranch,
                   const QString &branch, const QString &expected,
                   const QString &requestedBase, const QString &baseExpected,
                   bool deleteBranch)
{
    const QString op = QStringLiteral("merge");
    const QString base =
        requestedBase.isEmpty() ? defaultBranch : requestedBase;
    if (branch == base)
        return refusal(op, branch,
                       QStringLiteral("refused: %1 is its own base")
                           .arg(branch));
    if (branch == defaultBranch)
        return refusal(op, branch,
                       QStringLiteral("refused: %1 is the default branch")
                           .arg(branch));
    const QString tip = objectIdOf(dir, QStringLiteral("refs/heads/") + branch);
    if (tip.isEmpty())
        return refusal(op, branch,
                       QStringLiteral("refused: %1 no longer exists")
                           .arg(branch));
    if (tip != expected)
        return refusal(
            op, branch,
            QStringLiteral("refused: %1 moved to %2 since the request")
                .arg(branch, tip.left(12)));
    const QString baseTip =
        objectIdOf(dir, QStringLiteral("refs/heads/") + base);
    if (baseTip.isEmpty())
        return refusal(op, branch,
                       QStringLiteral("refused: base %1 is missing").arg(base));
    if (baseTip != baseExpected)
        return refusal(
            op, branch,
            QStringLiteral("refused: %1 moved to %2 since the request")
                .arg(base, baseTip.left(12)));
    const QHash<QString, QString> checkouts = worktreeCheckouts(dir);
    int contained = -1;
    runGit(dir,
           {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
            tip, baseTip},
           nullptr, nullptr, &contained);
    if (contained == 0) {
        Outcome outcome = satisfied(
            op, branch,
            QStringLiteral("already merged: %1 is in %2").arg(branch, base));
        outcome.branchOid = tip;
        outcome.worktreePath = checkouts.value(branch);
        outcome.cleanupRequested = deleteBranch;
        return outcome;
    }
    QString tree;
    int mergeCode = -1;
    runGit(dir,
           {QStringLiteral("merge-tree"), QStringLiteral("--write-tree"),
            QStringLiteral("--no-messages"), baseTip, tip},
           &tree, nullptr, &mergeCode);
    const QString mergedTree = tree.split(QLatin1Char('\n')).value(0).trimmed();
    if (mergeCode != 0 || mergedTree.isEmpty())
        return refusal(
            op, branch,
            QStringLiteral("refused: merging %1 into %2 conflicts")
                .arg(branch, base));
    QString merged;
    QString error;
    if (!runGit(dir,
                {QStringLiteral("commit-tree"), mergedTree,
                 QStringLiteral("-p"), baseTip, QStringLiteral("-p"), tip,
                 QStringLiteral("-m"),
                 QStringLiteral("Merge %1 into %2").arg(branch, base)},
                &merged, &error)
        || merged.isEmpty())
        return refusal(op, branch,
                       QStringLiteral("could not record the merge of %1: %2")
                           .arg(branch, error.left(200)));
    if (checkouts.contains(base)) {
        const QString baseWorktree = checkouts.value(base);
        if (!runGit(baseWorktree,
                    {QStringLiteral("-c"),
                     QStringLiteral("core.hooksPath=/dev/null"),
                     QStringLiteral("merge"), QStringLiteral("--ff-only"),
                     QStringLiteral("--quiet"), merged},
                    nullptr, &error))
            return refusal(
                op, branch,
                QStringLiteral("refused: %1 is checked out with local "
                               "changes the merge would overwrite: %2")
                    .arg(base, error.left(200)));
        if (objectIdOf(dir, QStringLiteral("refs/heads/") + base) != merged)
            return refusal(op, branch,
                           QStringLiteral("could not move %1 to the merge")
                               .arg(base));
    } else if (!runGit(dir,
                       {QStringLiteral("update-ref"),
                        QStringLiteral("refs/heads/") + base, merged, baseTip},
                       nullptr, &error)) {
        return refusal(op, branch,
                       QStringLiteral("could not move %1: %2")
                           .arg(base, error.left(200)));
    }
    Outcome outcome = done(
        op, branch, QStringLiteral("merged %1 into %2").arg(branch, base));
    outcome.branchOid = tip;
    outcome.mergeOid = merged;
    outcome.worktreePath = checkouts.value(branch);
    outcome.cleanupRequested = deleteBranch;
    return outcome;
}

} // namespace

Outcome cleanUpMergedBranch(const QString &checkoutPath,
                            const QString &defaultBranch, Outcome outcome)
{
    if (!outcome.cleanupRequested)
        return outcome;
    outcome.cleanupRequested = false;
    const QString branch = outcome.branch;
    const auto kept = [&outcome, &branch](const QString &why) {
        outcome.message += QStringLiteral("; kept %1: %2").arg(branch, why);
        return outcome;
    };
    if (branch == defaultBranch || !usableBranchName(branch))
        return kept(QStringLiteral("protected branch"));
    const QHash<QString, QString> checkouts = worktreeCheckouts(checkoutPath);
    const QString worktree = checkouts.value(branch);
    bool removedWorktree = false;
    if (checkouts.contains(branch)) {
        if (worktree.isEmpty() || samePath(worktree, checkoutPath))
            return kept(QStringLiteral("checked out here"));
        QString error;
        if (!runGit(checkoutPath,
                    {QStringLiteral("worktree"), QStringLiteral("remove"),
                     worktree},
                    nullptr, &error))
            return kept(QStringLiteral("its worktree has local changes"));
        removedWorktree = true;
    }
    const QString tip =
        objectIdOf(checkoutPath, QStringLiteral("refs/heads/") + branch);
    if (tip.isEmpty()) {
        outcome.branchDeleted = true;
        return outcome;
    }
    if (!outcome.branchOid.isEmpty() && tip != outcome.branchOid)
        return kept(QStringLiteral("it moved to %1").arg(tip.left(12)));
    QString error;
    if (!runGit(checkoutPath,
                {QStringLiteral("update-ref"),
                 QStringLiteral("refs/forkmesh/trash/deleted/") + branch, tip},
                nullptr, &error))
        return kept(error.left(200));
    if (!runGit(checkoutPath,
                {QStringLiteral("update-ref"), QStringLiteral("-d"),
                 QStringLiteral("refs/heads/") + branch, tip},
                nullptr, &error))
        return kept(error.left(200));
    outcome.branchDeleted = true;
    outcome.message += removedWorktree
                           ? QStringLiteral("; deleted %1 and its worktree")
                                 .arg(branch)
                           : QStringLiteral("; deleted %1").arg(branch);
    return outcome;
}

QJsonObject resultJson(const Outcome &outcome)
{
    QJsonObject result{
        {QStringLiteral("op"), outcome.op},
        {QStringLiteral("branch"), outcome.branch},
        {QStringLiteral("base"), outcome.base},
        {QStringLiteral("status"),
         outcome.applied       ? QStringLiteral("applied")
         : outcome.alreadyDone ? QStringLiteral("already")
                               : QStringLiteral("refused")},
        {QStringLiteral("message"), outcome.message.left(300)},
        {QStringLiteral("branchDeleted"), outcome.branchDeleted},
    };
    if (outcome.requestId > 0)
        result.insert(QStringLiteral("requestId"),
                      static_cast<double>(outcome.requestId));
    if (!outcome.mergeOid.isEmpty())
        result.insert(QStringLiteral("mergeOid"), outcome.mergeOid);
    return result;
}

bool usableBranchName(const QString &branch)
{
    static const QRegularExpression shape(
        QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._/-]{0,199}\\z"));
    if (!shape.match(branch).hasMatch())
        return false;
    if (branch.endsWith(QLatin1Char('.')) || branch.endsWith(QLatin1Char('/'))
        || branch.endsWith(QLatin1String(".lock"))
        || branch.contains(QLatin1String(".."))
        || branch.contains(QLatin1String("//"))
        || branch.contains(QLatin1String("@{")))
        return false;
    // ForkMesh's own collaboration refs are not user branches; the ledger and
    // the per-pull metadata refs are maintained by the pull machinery.
    return branch != QLatin1String("forkmesh/pulls")
           && !branch.startsWith(QLatin1String("forkmesh/pull/"));
}

namespace {

Outcome applyBranchOpUnlabelled(const QString &checkoutPath,
                                const QString &defaultBranch,
                                const QJsonObject &op)
{
    const QString operation = op.value(QStringLiteral("op")).toString().trimmed();
    const QString branch =
        op.value(QStringLiteral("branch")).toString().trimmed();
    if (!usableBranchName(branch))
        return refusal(operation, branch,
                       QStringLiteral("refused: unusable branch name"));
    const QDir dir(checkoutPath);
    if (checkoutPath.trimmed().isEmpty() || !dir.exists())
        return refusal(operation, branch,
                       QStringLiteral("refused: no local checkout"));
    if (operation == QLatin1String("delete")) {
        const QString expected = validObjectId(op, QStringLiteral("expectedOid"));
        if (expected.isEmpty())
            return refusal(operation, branch,
                           QStringLiteral("refused: no branch tip to verify"));
        return applyDelete(checkoutPath, defaultBranch, branch, expected);
    }
    if (operation == QLatin1String("update")) {
        const QString expected = validObjectId(op, QStringLiteral("expectedOid"));
        if (expected.isEmpty())
            return refusal(operation, branch,
                           QStringLiteral("refused: no branch tip to verify"));
        QString base = op.value(QStringLiteral("base")).toString().trimmed();
        if (!base.isEmpty() && !usableBranchName(base))
            return refusal(operation, branch,
                           QStringLiteral("refused: unusable base name"));
        return applyUpdate(checkoutPath, defaultBranch, branch, expected, base);
    }
    if (operation == QLatin1String("merge")) {
        const QString expected = validObjectId(op, QStringLiteral("expectedOid"));
        const QString baseExpected = validObjectId(op, QStringLiteral("baseOid"));
        if (expected.isEmpty() || baseExpected.isEmpty())
            return refusal(operation, branch,
                           QStringLiteral("refused: no branch tip to verify"));
        const QString base = op.value(QStringLiteral("base")).toString().trimmed();
        if (!base.isEmpty() && !usableBranchName(base))
            return refusal(operation, branch,
                           QStringLiteral("refused: unusable base name"));
        return applyMerge(checkoutPath, defaultBranch, branch, expected, base,
                          baseExpected,
                          op.value(QStringLiteral("deleteBranch")).toBool());
    }
    if (operation == QLatin1String("create")) {
        const QString start = validObjectId(op, QStringLiteral("startOid"));
        if (start.isEmpty())
            return refusal(operation, branch,
                           QStringLiteral("refused: no start commit"));
        return applyCreate(checkoutPath, branch, start);
    }
    return refusal(operation, branch,
                   QStringLiteral("refused: unsupported operation"));
}

} // namespace

Outcome applyBranchOp(const QString &checkoutPath,
                      const QString &defaultBranch, const QJsonObject &op)
{
    Outcome outcome = applyBranchOpUnlabelled(checkoutPath, defaultBranch, op);
    outcome.requestId =
        static_cast<qint64>(op.value(QStringLiteral("id")).toDouble());
    if (outcome.op == QLatin1String("merge")
        || outcome.op == QLatin1String("update")) {
        const QString base =
            op.value(QStringLiteral("base")).toString().trimmed();
        outcome.base = usableBranchName(base) ? base : defaultBranch;
    }
    return outcome;
}

QList<Outcome> applyBranchOps(const QString &checkoutPath,
                              const QString &defaultBranch,
                              const QJsonArray &ops)
{
    QList<Outcome> outcomes;
    for (const QJsonValue &value : ops) {
        if (!value.isObject())
            continue;
        outcomes.append(
            applyBranchOp(checkoutPath, defaultBranch, value.toObject()));
    }
    return outcomes;
}

} // namespace forkmesh::branchops

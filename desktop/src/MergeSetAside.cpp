#include "MergeSetAside.h"

#include <QHash>
#include <QProcess>
#include <QRegularExpression>

namespace forkmesh::mergesetaside {

namespace {

QString listed(const QStringList &paths)
{
    constexpr int kShown = 5;
    QString text = paths.mid(0, kShown).join(QStringLiteral(", "));
    if (paths.size() > kShown)
        text += QStringLiteral(" (+%1 more)").arg(paths.size() - kShown);
    return text;
}

QString failure(const GitResult &result, const QString &what)
{
    const QString detail = result.error.trimmed().left(200);
    return detail.isEmpty() ? what + QStringLiteral(" failed") : what + QStringLiteral(": ") + detail;
}

}

// code-health: allow(blocking-wait) called only from the runOffThread merge preparation
Git processGit(const QString &dir)
{
    return [dir](const QStringList &args, const QByteArray &input) {
        GitResult result;
        QProcess process;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(QStringLiteral("git"), QStringList{QStringLiteral("-C"), dir} + args);
        if (!process.waitForStarted(5000)) {
            result.error = QStringLiteral("git did not start");
            return result;
        }
        if (!input.isEmpty())
            process.write(input);
        process.closeWriteChannel();
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished(1000);
            result.error = QStringLiteral("git timed out");
            return result;
        }
        result.out = process.readAllStandardOutput();
        result.error = QString::fromUtf8(process.readAllStandardError());
        result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        return result;
    };
}

bool canSetAside(const QByteArray &statusPorcelainZ, const QStringList &paths)
{
    if (paths.isEmpty())
        return false;
    QHash<QString, QByteArray> states;
    const QList<QByteArray> entries = statusPorcelainZ.split('\0');
    for (int i = 0; i < entries.size(); ++i) {
        const QByteArray &entry = entries.at(i);
        if (entry.size() < 4)
            continue;
        states.insert(QString::fromUtf8(entry.mid(3)), entry.left(2));
        if ((entry.at(0) == 'R' || entry.at(0) == 'C') && i + 1 < entries.size())
            states.insert(QString::fromUtf8(entries.at(++i)), entry.left(2));
    }
    for (const QString &path : paths) {
        if (states.value(path) != QByteArrayLiteral(" M"))
            return false;
    }
    return true;
}

bool canSetAside(const Git &git, const QStringList &paths)
{
    const GitResult status = git({QStringLiteral("status"), QStringLiteral("--porcelain"),
                                  QStringLiteral("-z"), QStringLiteral("--untracked-files=all")},
                                 {});
    return status.ok && canSetAside(status.out, paths);
}

QString backupBranchName(const QString &base, const QString &branch, const QDateTime &when)
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9._/-]+"));
    QString name = QStringLiteral("wip/%1-before-%2-%3")
                       .arg(base, branch, when.toString(QStringLiteral("yyyyMMdd-HHmmss")));
    name.replace(unsafe, QStringLiteral("-"));
    name.replace(QStringLiteral(".."), QStringLiteral("-"));
    return name;
}

Aside setAside(const Git &git, const QString &base, const QString &branch,
               const QStringList &paths, const QDateTime &when)
{
    Aside aside;
    aside.paths = paths;
    if (!canSetAside(git, paths)) {
        aside.error = QStringLiteral("only edited files that aren't staged can be set aside");
        return aside;
    }
    const GitResult snapshot = git({QStringLiteral("stash"), QStringLiteral("create")}, {});
    const QString commit = QString::fromUtf8(snapshot.out).trimmed();
    if (!snapshot.ok || commit.isEmpty()) {
        aside.error = failure(snapshot, QStringLiteral("git stash create"));
        return aside;
    }
    const QString name = backupBranchName(base, branch, when);
    const GitResult valid = git({QStringLiteral("check-ref-format"), QStringLiteral("--branch"), name}, {});
    if (!valid.ok) {
        aside.error = QStringLiteral("%1 is not a valid branch name").arg(name);
        return aside;
    }
    const GitResult saved = git({QStringLiteral("branch"), name, commit}, {});
    if (!saved.ok) {
        aside.error = failure(saved, QStringLiteral("git branch"));
        return aside;
    }
    aside.backupBranch = name;
    const GitResult patch =
        git(QStringList{QStringLiteral("diff"), QStringLiteral("--binary"), QStringLiteral("--no-ext-diff"),
                        QStringLiteral("--no-color"), QStringLiteral("HEAD"), QStringLiteral("--")}
                + paths,
            {});
    if (!patch.ok || patch.out.isEmpty()) {
        aside.error = failure(patch, QStringLiteral("git diff"));
        return aside;
    }
    aside.patch = patch.out;
    const GitResult restored =
        git(QStringList{QStringLiteral("checkout"), QStringLiteral("HEAD"), QStringLiteral("--")} + paths, {});
    if (!restored.ok) {
        aside.error = failure(restored, QStringLiteral("git checkout"));
        return aside;
    }
    aside.ok = true;
    return aside;
}

PutBack putBack(const Git &git, const Aside &aside)
{
    PutBack back;
    const GitResult applied =
        git({QStringLiteral("apply"), QStringLiteral("--3way"), QStringLiteral("--whitespace=nowarn"),
             QStringLiteral("-")},
            aside.patch);
    if (applied.ok) {
        const GitResult unstaged =
            git(QStringList{QStringLiteral("reset"), QStringLiteral("-q"), QStringLiteral("--")} + aside.paths, {});
        back.ok = unstaged.ok;
        if (!unstaged.ok)
            back.error = failure(unstaged, QStringLiteral("git reset"));
        return back;
    }
    const GitResult unmerged = git({QStringLiteral("diff"), QStringLiteral("--name-only"),
                                    QStringLiteral("--diff-filter=U"), QStringLiteral("-z")},
                                   {});
    for (const QByteArray &path : unmerged.out.split('\0')) {
        if (!path.isEmpty())
            back.clashed << QString::fromUtf8(path);
    }
    if (back.clashed.isEmpty())
        back.error = failure(applied, QStringLiteral("git apply"));
    return back;
}

QString putBackNote(const Aside &aside, const PutBack &back)
{
    if (back.ok)
        return QStringLiteral("Set aside the uncommitted changes to %1 for the merge and put them "
                              "back. A copy of all uncommitted work is on branch %2.")
            .arg(listed(aside.paths), aside.backupBranch);
    if (!back.clashed.isEmpty())
        return QStringLiteral("The uncommitted changes to %1 clash with the merge, so those files "
                              "now have conflict markers to resolve. The untouched copy is on "
                              "branch %2.")
            .arg(listed(back.clashed), aside.backupBranch);
    return QStringLiteral("Couldn't put back the uncommitted changes to %1 after the merge (%2). "
                          "They are safe on branch %3.")
        .arg(listed(aside.paths), back.error, aside.backupBranch);
}

}

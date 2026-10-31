#include "MergeSetAside.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::mergesetaside;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void write(const QString &path, const QByteArray &text)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(text);
}

static QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static bool run(const Git &git, const QStringList &args)
{
    const GitResult result = git(args, {});
    if (!result.ok)
        std::cerr << "git " << args.join(QLatin1Char(' ')).toStdString() << ": "
                  << result.error.toStdString() << "\n";
    return result.ok;
}

static bool commitAll(const Git &git, const QString &message)
{
    return run(git, {QStringLiteral("add"), QStringLiteral("-A")})
        && run(git, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), message});
}

static const QByteArray kLines = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n";

struct Repo {
    QTemporaryDir dir;
    Git git;
    QString path(const QString &name) const { return QDir(dir.path()).filePath(name); }
};

static bool prepare(Repo &repo)
{
    repo.git = processGit(repo.dir.path());
    if (!run(repo.git, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")}))
        return false;
    write(repo.path(QStringLiteral("notes.txt")), kLines);
    write(repo.path(QStringLiteral("other.txt")), "untouched\n");
    if (!commitAll(repo.git, QStringLiteral("start")))
        return false;
    if (!run(repo.git, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("agent/fix")}))
        return false;
    QByteArray changed = kLines;
    changed.replace("one\n", "one from the agent\n");
    write(repo.path(QStringLiteral("notes.txt")), changed);
    if (!commitAll(repo.git, QStringLiteral("agent edit")))
        return false;
    return run(repo.git, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("main")});
}

static bool mergeAgent(const Git &git)
{
    return run(git, {QStringLiteral("merge"), QStringLiteral("--no-ff"), QStringLiteral("-q"),
                     QStringLiteral("-m"), QStringLiteral("Merge agent/fix into main"),
                     QStringLiteral("agent/fix")});
}

static QByteArray entry(const char *line)
{
    return QByteArray(line) + '\0';
}

static void testStatusRules()
{
    const QStringList notes{QStringLiteral("notes.txt")};
    check(canSetAside(entry(" M notes.txt"), notes), "an unstaged edit can be set aside");
    check(!canSetAside(entry("M  notes.txt"), notes), "a staged edit cannot");
    check(!canSetAside(entry("MM notes.txt"), notes), "a partly staged edit cannot");
    check(!canSetAside(entry("?? notes.txt"), notes), "an untracked file cannot");
    check(!canSetAside(entry(" D notes.txt"), notes), "a deleted file cannot");
    check(!canSetAside(entry(" M other.txt"), notes), "a path missing from status cannot");
    check(!canSetAside(entry(" M notes.txt"), {}), "nothing to set aside is refused");
}

static void testBackupName()
{
    const QDateTime when(QDate(2026, 10, 23), QTime(4, 15, 30));
    check(backupBranchName(QStringLiteral("main"), QStringLiteral("2497-improve"), when)
              == QStringLiteral("wip/main-before-2497-improve-20261023-041530"),
          "backup branch names the base, the branch and the time");
    check(backupBranchName(QStringLiteral("main"), QStringLiteral("a b..c~d"), when)
              == QStringLiteral("wip/main-before-a-b-c-d-20261023-041530"),
          "unsafe characters are replaced");
}

static void testCleanPutBack()
{
    Repo repo;
    if (!prepare(repo)) {
        check(false, "clean: repository prepared");
        return;
    }
    QByteArray local = kLines;
    local.replace("eight\n", "eight edited locally\n");
    write(repo.path(QStringLiteral("notes.txt")), local);
    write(repo.path(QStringLiteral("other.txt")), "other edit\n");
    const QDateTime when(QDate(2026, 10, 23), QTime(4, 15, 30));
    const Aside aside = setAside(repo.git, QStringLiteral("main"), QStringLiteral("agent/fix"),
                                 {QStringLiteral("notes.txt")}, when);
    check(aside.ok, "clean: set aside succeeds");
    check(read(repo.path(QStringLiteral("notes.txt"))) == kLines, "clean: the file is back at HEAD");
    check(read(repo.path(QStringLiteral("other.txt"))) == "other edit\n", "clean: other edits are left alone");
    const GitResult backup = repo.git({QStringLiteral("show"), aside.backupBranch + QStringLiteral(":notes.txt")}, {});
    check(backup.ok && backup.out == local, "clean: the backup branch holds the edit");
    check(mergeAgent(repo.git), "clean: the merge runs");
    const PutBack back = putBack(repo.git, aside);
    check(back.ok, "clean: put back succeeds");
    QByteArray both = local;
    both.replace("one\n", "one from the agent\n");
    check(read(repo.path(QStringLiteral("notes.txt"))) == both, "clean: both edits are in the file");
    const GitResult status = repo.git({QStringLiteral("status"), QStringLiteral("--porcelain")}, {});
    check(status.out == " M notes.txt\n M other.txt\n", "clean: the edits are unstaged as before");
    check(putBackNote(aside, back).contains(aside.backupBranch), "clean: the note names the backup branch");
}

static void testClash()
{
    Repo repo;
    if (!prepare(repo)) {
        check(false, "clash: repository prepared");
        return;
    }
    QByteArray local = kLines;
    local.replace("one\n", "one edited locally\n");
    write(repo.path(QStringLiteral("notes.txt")), local);
    const Aside aside = setAside(repo.git, QStringLiteral("main"), QStringLiteral("agent/fix"),
                                 {QStringLiteral("notes.txt")}, QDateTime::currentDateTime());
    check(aside.ok, "clash: set aside succeeds");
    check(mergeAgent(repo.git), "clash: the merge runs");
    const PutBack back = putBack(repo.git, aside);
    check(!back.ok, "clash: put back reports the clash");
    check(back.clashed == QStringList{QStringLiteral("notes.txt")}, "clash: names the clashing file");
    const QByteArray text = read(repo.path(QStringLiteral("notes.txt")));
    check(text.contains("<<<<<<<") && text.contains("one edited locally"),
          "clash: the local edit stays in the file between conflict markers");
    check(putBackNote(aside, back).contains(QStringLiteral("conflict markers")), "clash: the note says so");
}

static void testRefusesStaged()
{
    Repo repo;
    if (!prepare(repo)) {
        check(false, "staged: repository prepared");
        return;
    }
    write(repo.path(QStringLiteral("notes.txt")), "staged\n");
    run(repo.git, {QStringLiteral("add"), QStringLiteral("notes.txt")});
    const Aside aside = setAside(repo.git, QStringLiteral("main"), QStringLiteral("agent/fix"),
                                 {QStringLiteral("notes.txt")}, QDateTime::currentDateTime());
    check(!aside.ok && aside.backupBranch.isEmpty(), "staged: nothing is set aside");
    check(read(repo.path(QStringLiteral("notes.txt"))) == "staged\n", "staged: the file is untouched");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    qputenv("GIT_AUTHOR_NAME", "Sample Author");
    qputenv("GIT_AUTHOR_EMAIL", "author@example.com");
    qputenv("GIT_COMMITTER_NAME", "Sample Author");
    qputenv("GIT_COMMITTER_EMAIL", "author@example.com");
    testStatusRules();
    testBackupName();
    testCleanPutBack();
    testClash();
    testRefusesStaged();
    if (failures)
        std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}

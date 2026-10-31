#include "TaskWorkItems.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::taskwork;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool git(const QString &path, const QStringList &arguments, QString *output = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(path);
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForFinished(60000))
        return false;
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void write(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(bytes);
}

bool refExists(const QString &repo, const QString &ref)
{
    return git(repo, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                      QStringLiteral("--quiet"), ref});
}

void sandbox(const QString &home)
{
    qputenv("HOME", home.toUtf8());
    qputenv("XDG_DATA_HOME", (home + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CONFIG_HOME", (home + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_CACHE_HOME", (home + QStringLiteral("/cache")).toUtf8());
    qunsetenv("FORKMESH_HOME");
    qunsetenv("FORKMESH_PROJECTS_DIR");
    qunsetenv("GIT_DIR");
    qunsetenv("GIT_WORK_TREE");
    qunsetenv("GIT_INDEX_FILE");
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    qputenv("GIT_AUTHOR_NAME", "Work Item Test");
    qputenv("GIT_AUTHOR_EMAIL", "work-items@example.invalid");
    qputenv("GIT_COMMITTER_NAME", "Work Item Test");
    qputenv("GIT_COMMITTER_EMAIL", "work-items@example.invalid");
}

void testDigestMatchesTheWorker()
{
    QJsonObject item{
        {QStringLiteral("repository"), QStringLiteral("forkmesh/src")},
        {QStringLiteral("branch"), QStringLiteral("feat/task-work-items")},
        {QStringLiteral("worktree"),
         QString::fromUtf8("/home/tester/src/.worktrees/012-caf\xC3\xA9 \"q\"\\\\x\t")},
        {QStringLiteral("node"), QStringLiteral("desk-1")},
    };
    QJsonObject body{
        {QStringLiteral("workItems"), QJsonArray{item}},
        {QStringLiteral("priority"), 7},
        {QStringLiteral("flag"), true},
        {QStringLiteral("none"), QJsonValue::Null},
        {QStringLiteral("sessionToken"), QStringLiteral("ignored")},
    };
    check(updateDigest(body) ==
              QLatin1String("ec09f1e510ca68958c21322518b3956925c79edba3a8c782e586f4f325ad7ad7"),
          "update digest matches org_task_update_digest in entry.py");
    check(updateDigest(QJsonObject{{QStringLiteral("sessionToken"), QStringLiteral("x")}})
              .isEmpty(),
          "a body with only a session token has no digest");
    check(canonicalJson(QJsonObject{{QStringLiteral("b"), 1}, {QStringLiteral("a"),
                                                              QStringLiteral("\x01")}}) ==
              QByteArray("{\"a\":\"\\u0001\",\"b\":1}"),
          "canonical JSON sorts keys and escapes controls like Python");
}

void testValidationAndMerge()
{
    check(validBranch(QStringLiteral("feat/task-work-items")), "ordinary branch is valid");
    check(validBranch(QStringLiteral("443-list-based-visual")), "numbered branch is valid");
    for (const char *bad : {"../main", "-rf", "x.lock", "a b", "a\tb", "a..b", "/a",
                            "a/", ".hidden", "a/.b", "a//b", "HEAD", ""})
        check(!validBranch(QString::fromLatin1(bad)), bad);
    check(!validBranch(QString(201, QLatin1Char('a'))), "branch over 200 characters");

    const WorkItem a{QStringLiteral("o/r"), QStringLiteral("a"), QStringLiteral("/w/a"),
                     QStringLiteral("desk")};
    const WorkItem other{QStringLiteral("o/r"), QStringLiteral("a"), QStringLiteral("/x/a"),
                         QStringLiteral("laptop")};
    QList<WorkItem> items = withItem({a, other}, WorkItem{QStringLiteral("O/R"),
                                                          QStringLiteral("a"),
                                                          QStringLiteral("/w/a2"),
                                                          QStringLiteral("desk")});
    check(items.size() == 2 && items.at(0).worktree == QLatin1String("/w/a2"),
          "attach replaces the entry with the same repository, branch and node");
    check(itemsForNode(items, QStringLiteral("DESK")).size() == 1, "items filter by node");
    check(withoutItem(items, a).size() == 1, "detach drops only the matching key");
    QList<WorkItem> many;
    for (int i = 0; i < 25; ++i)
        many = withItem(many, WorkItem{{}, QStringLiteral("b%1").arg(i), {}, {}});
    check(many.size() == kMaxWorkItems && many.last().branch == QLatin1String("b24"),
          "the list is capped at 20, newest kept");
    const QJsonArray round = toJsonArray(items);
    check(fromJsonArray(round).size() == 2 && round.at(0).toObject().size() == 4,
          "work items round-trip through JSON");
}

void testPorcelainAndPathRules(const QString &home)
{
    const QString porcelain = QStringLiteral(
        "worktree /repo\nHEAD 1111\nbranch refs/heads/main\n\n"
        "worktree /repo/.worktrees/001-a\nHEAD 2222\nbranch refs/heads/001-a\n\n"
        "worktree /repo/.worktrees/002-d\nHEAD 3333\ndetached\n\n"
        "worktree /data/pull-meta/x\nHEAD 4444\nbranch refs/heads/forkmesh/pulls\n");
    const QList<WorktreeEntry> entries = parseWorktreeList(porcelain);
    check(entries.size() == 4, "porcelain lists four worktrees");
    check(entries.at(1).branch == QLatin1String("refs/heads/001-a"), "branch parsed");
    check(entries.at(2).detached, "detached parsed");

    Guard guard{home, {QStringLiteral("/data")}};
    check(pathRefusal(QStringLiteral("/repo/.worktrees/001-a"), entries, guard).isEmpty(),
          "a listed linked worktree is allowed");
    check(!pathRefusal(QStringLiteral("/repo"), entries, guard).isEmpty(),
          "the main checkout is refused");
    check(!pathRefusal(QStringLiteral("/repo/.worktrees/999-x"), entries, guard).isEmpty(),
          "an unlisted path is refused");
    check(!pathRefusal(QStringLiteral("/data/pull-meta/x"), entries, guard).isEmpty(),
          "a worktree inside ForkMesh data is refused");
    check(!pathRefusal(QStringLiteral("relative"), entries, guard).isEmpty(),
          "a relative path is refused");
    const QList<WorktreeEntry> homeEntries = parseWorktreeList(
        QStringLiteral("worktree /elsewhere\nbranch refs/heads/main\n\nworktree ") + home +
        QStringLiteral("\nbranch refs/heads/x\n"));
    check(!pathRefusal(home, homeEntries, guard).isEmpty(), "the home folder is refused");

    check(!branchRefusal(QStringLiteral("main"), {}, {}).isEmpty(), "main is protected");
    check(!branchRefusal(QStringLiteral("master"), {}, {}).isEmpty(), "master is protected");
    check(!branchRefusal(QStringLiteral("dev"), QStringLiteral("dev"), {}).isEmpty(),
          "the main checkout's branch is protected");
    check(!branchRefusal(QStringLiteral("trunk"), {}, QStringLiteral("trunk")).isEmpty(),
          "the default branch is protected");
    check(branchRefusal(QStringLiteral("feature"), QStringLiteral("main"),
                        QStringLiteral("main"))
              .isEmpty(),
          "a task branch is not protected");

    check(decide(true, false, true) == Plan::Refuse, "a refusal wins");
    check(decide(false, false, true) == Plan::Remove, "clean and merged is removed");
    check(decide(false, true, true) == Plan::Confirm, "dirty needs confirmation");
    check(decide(false, false, false) == Plan::Confirm, "unmerged needs confirmation");
    check(backupRef(QStringLiteral("ab"), QStringLiteral("feat/x")) ==
              QLatin1String("refs/forkmesh/trash/tasks/ab/feat/x"),
          "backup ref name");
}

void testRealRepository(const QString &root)
{
    const QString repo = root + QStringLiteral("/checkout");
    QDir().mkpath(repo);
    check(git(repo, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("main")}),
          "git init");
    write(repo + QStringLiteral("/README"), "base\n");
    git(repo, {QStringLiteral("add"), QStringLiteral("README")});
    check(git(repo, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"),
                     QStringLiteral("base")}),
          "initial commit");
    const QString wtRoot = repo + QStringLiteral("/.worktrees");
    const QString clean = wtRoot + QStringLiteral("/001-clean");
    const QString dirty = wtRoot + QStringLiteral("/002-dirty");
    const QString wrong = wtRoot + QStringLiteral("/003-wrong");
    check(git(repo, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-q"),
                     QStringLiteral("-b"), QStringLiteral("001-clean"), clean}),
          "clean worktree added");
    check(git(repo, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-q"),
                     QStringLiteral("-b"), QStringLiteral("feat/dirty"), dirty}),
          "dirty worktree added");
    check(git(repo, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-q"),
                     QStringLiteral("-b"), QStringLiteral("003-actual"), wrong}),
          "third worktree added");
    write(dirty + QStringLiteral("/feature.txt"), "committed\n");
    git(dirty, {QStringLiteral("add"), QStringLiteral("feature.txt")});
    check(git(dirty, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"),
                      QStringLiteral("unmerged work")}),
          "unmerged commit");
    write(dirty + QStringLiteral("/README"), "edited\n");
    write(dirty + QStringLiteral("/notes/untracked.txt"), "new\n");
    git(repo, {QStringLiteral("branch"), QStringLiteral("merged-only")});
    write(wtRoot + QStringLiteral("/.gitignore"), "# ForkMesh agent worktrees. Never part of the project.\n*\n");
    const QString scratch = wtRoot + QStringLiteral("/.scratch");
    write(scratch + QStringLiteral("/001-clean/tmp/build.log"), "log\n");
    write(scratch + QStringLiteral("/002-dirty/tmp/build.log"), "log\n");
    write(scratch + QStringLiteral("/003-wrong/tmp/build.log"), "log\n");

    const Guard guard{root + QStringLiteral("/home"), {root + QStringLiteral("/fmdata")}};
    const QString taskId = QStringLiteral("0123456789abcdef0123456789abcdef");

    const Inspection mainItem =
        inspect(repo, WorkItem{{}, QStringLiteral("main"), repo, {}}, guard);
    check(mainItem.plan == Plan::Refuse, "the main branch in the main checkout is refused");
    const Inspection mainCheckout =
        inspect(repo, WorkItem{{}, QStringLiteral("001-clean"), repo, {}}, guard);
    check(mainCheckout.plan == Plan::Refuse, "the main checkout path is refused");
    const Inspection mismatched =
        inspect(repo, WorkItem{{}, QStringLiteral("003-wrong"), wrong, {}}, guard);
    check(mismatched.plan == Plan::Refuse, "a worktree on another branch is refused");
    const QString stray = root + QStringLiteral("/stray");
    write(stray + QStringLiteral("/keep.txt"), "mine\n");
    const Inspection unlisted =
        inspect(repo, WorkItem{{}, QStringLiteral("stray"), stray, {}}, guard);
    check(unlisted.plan == Plan::Refuse, "an unlisted directory is refused");
    check(!removeWithBackup(unlisted, taskId).ok, "a refused item is never removed");
    check(QFileInfo::exists(stray + QStringLiteral("/keep.txt")),
          "the unlisted directory is untouched");
    const Guard dataGuard{guard.home, {wtRoot}};
    const Inspection inData =
        inspect(repo, WorkItem{{}, QStringLiteral("001-clean"), clean, {}}, dataGuard);
    check(inData.plan == Plan::Refuse, "a worktree inside a protected dir is refused");

    const Inspection cleanItem =
        inspect(repo, WorkItem{{}, QStringLiteral("001-clean"), clean, {}}, guard,
                QStringLiteral("main"));
    check(cleanItem.plan == Plan::Remove, "a clean merged worktree is removable");
    const Outcome cleaned = removeMerged(cleanItem);
    check(cleaned.ok, "clean worktree removed");
    check(!QFileInfo::exists(clean), "clean worktree directory is gone");
    check(!QFileInfo::exists(scratch + QStringLiteral("/001-clean")),
          "the removed worktree's scratch folder is gone");
    check(QFileInfo::exists(scratch + QStringLiteral("/003-wrong/tmp/build.log")),
          "another worktree's scratch folder is kept");
    check(!refExists(repo, QStringLiteral("refs/heads/001-clean")), "merged branch deleted");

    const Inspection dirtyItem =
        inspect(repo, WorkItem{{}, QStringLiteral("feat/dirty"), dirty, {}}, guard);
    check(dirtyItem.plan == Plan::Confirm, "a dirty unmerged worktree needs confirmation");
    check(dirtyItem.dirtyFiles.size() == 2, "both dirty files are listed");
    check(dirtyItem.unmergedCommits.size() == 1 &&
              dirtyItem.unmergedCommits.first().contains(QLatin1String("unmerged work")),
          "the unmerged commit is listed");
    check(!lossSummary(dirtyItem).isEmpty(), "loss summary is written");
    check(!removeMerged(dirtyItem).ok && QFileInfo::exists(dirty),
          "removeMerged refuses dirty work");
    QString branchTip;
    git(repo, {QStringLiteral("rev-parse"), QStringLiteral("refs/heads/feat/dirty")},
        &branchTip);
    const Outcome backedUp = removeWithBackup(dirtyItem, taskId);
    check(backedUp.ok, "dirty worktree removed after backup");
    const QString trash = backupRef(taskId, QStringLiteral("feat/dirty"));
    check(backedUp.backupRef == trash && refExists(repo, trash), "backup ref written");
    check(!refExists(repo, QStringLiteral("refs/forkmesh/wip/tasks/") + taskId +
                               QStringLiteral("/feat/dirty")),
          "temporary WIP ref removed");
    check(!QFileInfo::exists(dirty), "dirty worktree directory is gone");
    check(!QFileInfo::exists(scratch + QStringLiteral("/002-dirty")),
          "the backed-up worktree's scratch folder is gone");
    check(!refExists(repo, QStringLiteral("refs/heads/feat/dirty")), "branch force-deleted");
    QString parent;
    git(repo, {QStringLiteral("rev-parse"), trash + QStringLiteral("^")}, &parent);
    check(parent == branchTip, "the WIP commit sits on the branch tip");
    QString readme, untracked;
    git(repo, {QStringLiteral("show"), trash + QStringLiteral(":README")}, &readme);
    git(repo, {QStringLiteral("show"), trash + QStringLiteral(":notes/untracked.txt")},
        &untracked);
    check(readme == QLatin1String("edited") && untracked == QLatin1String("new"),
          "the backup holds the uncommitted edits and untracked files");
    check(git(repo, {QStringLiteral("branch"), QStringLiteral("restored"), trash}),
          "a backup restores with git branch");

    const Inspection branchOnly =
        inspect(repo, WorkItem{{}, QStringLiteral("merged-only"), {}, {}}, guard);
    check(branchOnly.plan == Plan::Remove, "a merged branch-only item is removable");
    check(removeMerged(branchOnly).ok &&
              !refExists(repo, QStringLiteral("refs/heads/merged-only")),
          "the merged branch is deleted");
    const Inspection held =
        inspect(repo, WorkItem{{}, QStringLiteral("003-actual"), {}, {}}, guard);
    check(held.plan == Plan::Refuse, "a branch-only item checked out elsewhere is refused");
    check(worktreeListed(repo, wrong) && !worktreeListed(repo, repo),
          "worktreeListed ignores the main checkout");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) {
        std::fprintf(stderr, "FAIL: no temporary directory\n");
        return 1;
    }
    const QString home = root.filePath(QStringLiteral("home"));
    QDir().mkpath(home);
    sandbox(home);
    testDigestMatchesTheWorker();
    testValidationAndMerge();
    testPorcelainAndPathRules(home);
    testRealRepository(root.path());
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("task work item tests passed\n");
    return 0;
}

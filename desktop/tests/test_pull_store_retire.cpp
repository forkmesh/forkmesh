#include "AppPaths.h"
#include "ForkMeshIdentity.h"
#include "PullStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

static QString git(const QString &dir, const QStringList &args, bool *ok = nullptr)
{
    QProcess process;
    process.start("git", QStringList{"-C", dir} + args);
    process.waitForFinished(60000);
    const bool good = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (ok)
        *ok = good;
    return good ? QString::fromUtf8(process.readAllStandardOutput()).trimmed() : QString();
}

static QString tip(const QString &dir, const QString &ref)
{
    return git(dir, {"rev-parse", "--verify", "-q", ref});
}

int main(int argc, char **argv)
{
    QTemporaryDir root(QDir::tempPath() + "/pullstore-retire-XXXXXX");
    if (!root.isValid())
        return 2;
    for (const char *name : {"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT"})
        qunsetenv(name);
    qputenv("HOME", root.filePath("home").toUtf8());
    qputenv("XDG_DATA_HOME", root.filePath("xdg-data").toUtf8());
    qputenv("XDG_CONFIG_HOME", root.filePath("xdg-config").toUtf8());
    qputenv("XDG_CACHE_HOME", root.filePath("xdg-cache").toUtf8());
    QCoreApplication app(argc, argv);
    app.setApplicationName("ForkMeshPullStoreRetireTest");
    app.setOrganizationName("ForkMesh");
    if (!forkmesh::paths::dataRoot().startsWith(root.path() + "/")) {
        std::printf("FAIL: data root %s is outside the sandbox\n",
                    qPrintable(forkmesh::paths::dataRoot()));
        return 2;
    }
    const QString repo = root.filePath("repo");
    QDir().mkpath(repo);
    git(repo, {"init", "-q", "-b", "main"});
    git(repo, {"config", "user.email", "t@example.test"});
    git(repo, {"config", "user.name", "T"});
    QFile readme(repo + "/README.md");
    readme.open(QIODevice::WriteOnly);
    readme.write("one\n");
    readme.close();
    git(repo, {"add", "README.md"});
    git(repo, {"commit", "-q", "-m", "one"});
    git(repo, {"checkout", "-q", "-b", "feature"});
    QFile feature(repo + "/feature.txt");
    feature.open(QIODevice::WriteOnly);
    feature.write("feature\n");
    feature.close();
    git(repo, {"add", "feature.txt"});
    git(repo, {"commit", "-q", "-m", "feature", "-m",
               "Co-authored-by: Pat <Pat@Example.TEST>\n"
               "Co-authored-by: Sam Lee <sam@example.test>"});
    git(repo, {"checkout", "-q", "main"});

    ForkMeshIdentity identity;
    check(identity.load(), "identity loads in the sandbox");
    PullStore store(repo, QString(), &identity, "tester");
    QString error;
    const int number = store.createPull("Feature", "body", "main", "feature",
                                        QString(), QString(), true, &error);
    check(number == 1, "pull created");
    const QString openBranch = tip(repo, "refs/heads/forkmesh/pull/1");
    check(!openBranch.isEmpty() && openBranch == tip(repo, "refs/pr/1/metadata"),
          "open pull has its metadata branch");

    check(store.mergePull(1, &error, false, false), "merge succeeds");
    check(tip(repo, "refs/heads/forkmesh/pull/1").isEmpty(),
          "merge removes refs/heads/forkmesh/pull/1");
    const QString kept = tip(repo, "refs/pr/1/metadata");
    const QString tomb = tip(repo, "refs/forkmesh/trash/deleted/forkmesh/pull/1");
    check(!kept.isEmpty() && !tomb.isEmpty(), "metadata ref and tombstone exist");
    bool contained = false;
    git(repo, {"merge-base", "--is-ancestor", tomb, kept}, &contained);
    check(contained, "metadata ref contains the removed branch tip");
    check(git(repo, {"show", kept + ":.forkmesh/pulls/1/pull.md"}).contains("status: merged"),
          "metadata ref records the merge");
    const QString mergedRecord = git(repo, {"show", kept + ":.forkmesh/pulls/1/pull.md"});
    check(mergedRecord.contains("\nmergedAt: 1") || mergedRecord.contains("\nmergedAt: 2"),
          "merged record carries mergedAt");
    check(mergedRecord.contains("\nmergedBy: " + identity.publicKey() + "\n"),
          "merged record names the merging node");
    check(mergedRecord.contains("\ncoAuthors: pat@example.test,sam@example.test\n"),
          "merged record lists the co-author emails");

    PullRequest listed;
    for (const PullRequest &pull : store.loadAll())
        if (pull.number == 1)
            listed = pull;
    check(listed.number == 1 && listed.status == "merged", "loadAll still lists the merged pull");
    check(tip(repo, "refs/heads/forkmesh/pull/1").isEmpty(),
          "loadAll does not recreate the merged pull's branch");

    const int next = store.createPull("Second", "body", "main", "feature2",
                                      "diff --git a/x b/x\nnew file mode 100644\n--- /dev/null\n+++ b/x\n@@ -0,0 +1 @@\n+x\n",
                                      QString(), false, &error);
    check(next == 2, "merged pull number is not reused");

    check(store.addComment(1, "after merge", &error), "comment on merged pull");
    check(tip(repo, "refs/heads/forkmesh/pull/1").isEmpty(),
          "a later event on a merged pull keeps the branch away");
    bool advanced = false;
    git(repo, {"merge-base", "--is-ancestor", kept, tip(repo, "refs/pr/1/metadata")}, &advanced);
    check(advanced && tip(repo, "refs/pr/1/metadata") != kept,
          "a later event advances refs/pr/1/metadata");

    git(repo, {"update-ref", "refs/heads/forkmesh/pull/1", tip(repo, "refs/pr/1/metadata")});
    store.loadAll();
    check(tip(repo, "refs/heads/forkmesh/pull/1").isEmpty(),
          "loadAll retires a merged pull branch copied back from a mirror");

    const QString secondBranch = tip(repo, "refs/heads/forkmesh/pull/2");
    check(!secondBranch.isEmpty(), "open pull 2 has its branch");
    check(store.deletePull(2, false, &error), "delete pull 2");
    check(tip(repo, "refs/heads/forkmesh/pull/2").isEmpty() &&
              tip(repo, "refs/forkmesh/trash/deleted/forkmesh/pull/2") == secondBranch,
          "deleting a pull leaves a tombstone at the removed tip");

    git(repo, {"checkout", "-q", "-b", "scrub", "main"});
    QFile secret(repo + "/secret.txt");
    secret.open(QIODevice::WriteOnly);
    secret.write("secret\n");
    secret.close();
    git(repo, {"add", "secret.txt"});
    git(repo, {"commit", "-q", "-m", "secret"});
    git(repo, {"checkout", "-q", "main"});
    const int scrubbed = store.createPull("Scrub", "secret", "main", "scrub",
                                          QString(), QString(), true, &error);
    check(scrubbed > 0 && !tip(repo, QStringLiteral("refs/heads/forkmesh/pull/%1")
                                         .arg(scrubbed)).isEmpty(),
          "pull to scrub has its branch");
    store.deletePull(scrubbed, true, &error);
    check(tip(repo, QStringLiteral("refs/heads/forkmesh/pull/%1").arg(scrubbed)).isEmpty() &&
              tip(repo, QStringLiteral("refs/forkmesh/trash/deleted/forkmesh/pull/%1")
                            .arg(scrubbed)).isEmpty(),
          "a history-rewriting delete keeps no tombstone of the scrubbed record");
    return failures == 0 ? 0 : 1;
}

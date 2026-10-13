#include "AgentScratch.h"
#include "AgentWorktree.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what)
{
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
}

bool hasEntry(const QVector<QPair<QString, QString>> &found, const QString &path)
{
    for (const auto &entry : found) {
        if (entry.first == path)
            return true;
    }
    return false;
}

}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 2;
    const QString root = QFileInfo(sandbox.path()).canonicalFilePath();
    const QString home = root + QStringLiteral("/home");
    QDir().mkpath(home);
    qputenv("HOME", home.toLocal8Bit());
    for (const char *variable : {"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_DATA_DIR",
                                 "FORKMESH_CONFIG_DIR", "FORKMESH_PROJECT_DIR",
                                 "XDG_DATA_HOME", "XDG_CONFIG_HOME"})
        qunsetenv(variable);
    QCoreApplication app(argc, argv);

    const QString repo = root + QStringLiteral("/repo");
    QDir().mkpath(repo + QStringLiteral("/.git"));
    const QString worktrees = agentwt::root(repo);
    agentwt::ensureRoot(worktrees);
    const QString worktree = worktrees + QStringLiteral("/1416-cleanup");
    writeFile(worktree + QStringLiteral("/.git"), "gitdir: x\n");

    const QString folder = agentscratch::folderFor(worktree);
    check(folder == worktrees + QStringLiteral("/.scratch/1416-cleanup"),
          "an agent worktree gets a scratch folder beside it");
    check(agentscratch::folderFor(worktree + QStringLiteral("/")) == folder,
          "a trailing slash names the same folder");
    check(agentscratch::folderFor(repo).isEmpty(), "the main checkout gets no scratch folder");
    check(agentscratch::folderFor(QString()).isEmpty(), "no worktree, no scratch folder");
    const QString elsewhere = root + QStringLiteral("/other/wt");
    writeFile(elsewhere + QStringLiteral("/.git"), "gitdir: x\n");
    check(agentscratch::folderFor(elsewhere).isEmpty(),
          "a worktree outside an agent worktree root gets no scratch folder");

    const QStringList env = agentscratch::environment(folder);
    check(env.contains(QStringLiteral("FORKMESH_AGENT_SCRATCH=") + folder),
          "environment names the scratch folder");
    check(env.contains(QStringLiteral("TMPDIR=") + folder + QStringLiteral("/tmp")),
          "TMPDIR points inside the scratch folder");
    check(QFileInfo(folder + QStringLiteral("/tmp")).isDir(), "environment creates its tmp");
    check(agentscratch::environment(QString()).isEmpty(), "no folder, no environment");

    writeFile(folder + QStringLiteral("/build/CMakeCache.txt"), "x");
    writeFile(folder + QStringLiteral("/nested/.git/HEAD"), "ref: refs/heads/main\n");
    check(agentscratch::orphans(worktrees).isEmpty(), "a live worktree's scratch is no orphan");
    check(agentscratch::sweepOrphans(worktrees) == 0 && QFileInfo::exists(folder),
          "the sweep keeps a live worktree's scratch");

    const QString gone = worktrees + QStringLiteral("/.scratch/1300-merged");
    writeFile(gone + QStringLiteral("/tmp/a.log"), "x");
    check(agentscratch::orphans(worktrees) == QStringList{gone},
          "scratch without its worktree is an orphan");
    check(agentscratch::sweepOrphans(worktrees) == 1 && !QFileInfo::exists(gone),
          "the sweep removes an orphaned scratch folder");

    check(agentscratch::remove(folder) && !QFileInfo::exists(folder),
          "remove deletes the scratch folder with everything in it");
    check(agentscratch::remove(folder), "removing a missing folder is fine");
    check(QFileInfo::exists(worktree + QStringLiteral("/.git")),
          "removing scratch leaves the worktree alone");

    const QString plain = root + QStringLiteral("/plain");
    QDir().mkpath(plain + QStringLiteral("/.scratch/x"));
    check(agentscratch::orphans(plain).isEmpty(),
          "a folder ForkMesh did not mark is never swept");

    const QString builds = root + QStringLiteral("/builds");
    const QString temp = root + QStringLiteral("/tmp");
    for (const QString &path :
         {builds + QStringLiteral("/agent-1416/CMakeCache.txt"),
          builds + QStringLiteral("/agent-1416-tmp/x"),
          builds + QStringLiteral("/agent-1416.build.log"),
          builds + QStringLiteral("/agent-14160/x"),
          builds + QStringLiteral("/agent-141/x"),
          builds + QStringLiteral("/integrate.build.log"),
          temp + QStringLiteral("/fm1416/x"),
          temp + QStringLiteral("/fm1416-swift/x"),
          temp + QStringLiteral("/fm1416.log"),
          temp + QStringLiteral("/_tmp_fm1416-swift.lock"),
          temp + QStringLiteral("/fm14160/x"),
          temp + QStringLiteral("/fm1416f-base.log"),
          temp + QStringLiteral("/forkmesh-agent-jails/s1416/tmp/x"),
          temp + QStringLiteral("/forkmesh-agent-jails/s1416-triage/tmp/x"),
          temp + QStringLiteral("/forkmesh-agent-jails/s14160/tmp/x")})
        writeFile(path, "x");
    const auto found = agentscratch::sessionLeftovers(1416, builds, temp);
    check(found.size() == 9, "session leftovers count");
    check(hasEntry(found, builds + QStringLiteral("/agent-1416")) &&
              hasEntry(found, builds + QStringLiteral("/agent-1416-tmp")) &&
              hasEntry(found, builds + QStringLiteral("/agent-1416.build.log")),
          "the agent's build trees and logs are leftovers");
    check(!hasEntry(found, builds + QStringLiteral("/agent-14160")) &&
              !hasEntry(found, builds + QStringLiteral("/agent-141")),
          "another agent's build tree is not");
    check(hasEntry(found, temp + QStringLiteral("/fm1416")) &&
              hasEntry(found, temp + QStringLiteral("/fm1416-swift")) &&
              hasEntry(found, temp + QStringLiteral("/fm1416.log")) &&
              hasEntry(found, temp + QStringLiteral("/_tmp_fm1416-swift.lock")),
          "the agent's temp files are leftovers");
    check(!hasEntry(found, temp + QStringLiteral("/fm14160")) &&
              !hasEntry(found, temp + QStringLiteral("/fm1416f-base.log")),
          "look-alike temp files are not");
    check(hasEntry(found, temp + QStringLiteral("/forkmesh-agent-jails/s1416")) &&
              hasEntry(found, temp + QStringLiteral("/forkmesh-agent-jails/s1416-triage")) &&
              !hasEntry(found, temp + QStringLiteral("/forkmesh-agent-jails/s14160")),
          "only this session's jail folders are leftovers");
    check(agentscratch::sessionLeftovers(0, builds, temp).isEmpty(),
          "no session, no leftovers");

    check(agentscratch::removeSessionLeftovers(1416, builds, temp) == 9,
          "every leftover is removed");
    check(agentscratch::sessionLeftovers(1416, builds, temp).isEmpty(),
          "nothing of the agent is left");
    check(QFileInfo::exists(builds + QStringLiteral("/agent-14160/x")) &&
              QFileInfo::exists(builds + QStringLiteral("/integrate.build.log")) &&
              QFileInfo::exists(temp + QStringLiteral("/fm14160/x")) &&
              QFileInfo::exists(temp + QStringLiteral("/forkmesh-agent-jails/s14160/tmp/x")),
          "other agents' files stay");

    const QString clone = temp + QStringLiteral("/fm1417-clone");
    QDir().mkpath(clone + QStringLiteral("/.git/objects"));
    check(agentscratch::removeSessionLeftovers(1417, builds, temp) == 0 &&
              QFileInfo::exists(clone + QStringLiteral("/.git/objects")),
          "a git checkout among the leftovers is refused, not deleted");

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

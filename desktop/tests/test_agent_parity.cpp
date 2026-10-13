#include "AgentParity.h"

#include <QCoreApplication>

#include <cstdio>

using namespace forkmesh::agentparity;
namespace ap = forkmesh::agentparity;

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool ok, const char *what)
{
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

const QString kRoot = QStringLiteral("/repo/.worktrees");

Agent agent(int id, const QString &branch, bool needsWorktree = true)
{
    Agent a;
    a.id = id;
    a.title = QStringLiteral("Task %1").arg(id);
    a.branch = branch;
    a.folder = branch;
    a.needsBranch = true;
    a.needsWorktree = needsWorktree;
    return a;
}

Worktree worktree(const QString &branch, bool missing = false)
{
    return Worktree{kRoot + QLatin1Char('/') + branch, branch, missing};
}

QSet<QString> keys(const QList<Issue> &issues)
{
    QSet<QString> out;
    for (const Issue &issue : issues)
        out.insert(issue.key);
    return out;
}

void testParse()
{
    const QString porcelain = QStringLiteral(
        "worktree /repo\nHEAD abc\nbranch refs/heads/main\n\n"
        "worktree /repo/.worktrees/12-fix\nHEAD def\nbranch refs/heads/12-fix\n\n"
        "worktree /repo/.worktrees/gone\nHEAD 123\ndetached\nprunable gitdir file "
        "points to non-existent location\n");
    const QList<Worktree> list = parseWorktrees(porcelain);
    expect(list.size() == 3, "parse: three worktrees");
    expect(list.value(0).branch == QLatin1String("main"), "parse: main branch");
    expect(list.value(1).path == QLatin1String("/repo/.worktrees/12-fix") &&
              list.value(1).branch == QLatin1String("12-fix") &&
              !list.value(1).missing,
          "parse: agent worktree");
    expect(list.value(2).branch.isEmpty() && list.value(2).missing,
          "parse: detached prunable worktree");
    const QSet<QString> branches =
        parseBranches(QStringLiteral("main\n12-fix\n\n"));
    expect(branches.size() == 2 && branches.contains(QStringLiteral("12-fix")),
          "parse: branch list");
}

void testInParity()
{
    const QList<Agent> agents{agent(12, QStringLiteral("12-fix")),
                              agent(13, QStringLiteral("13-ui"))};
    const QList<Worktree> worktrees{
        Worktree{QStringLiteral("/repo"), QStringLiteral("main"), false},
        worktree(QStringLiteral("12-fix")), worktree(QStringLiteral("13-ui"))};
    const QSet<QString> branches{QStringLiteral("main"), QStringLiteral("12-fix"),
                                 QStringLiteral("13-ui"),
                                 QStringLiteral("feature/manual")};
    expect(ap::check(agents, worktrees, branches, kRoot).isEmpty(),
          "parity: matched agents, branches and worktrees raise nothing");
}

void testAgentMissingPieces()
{
    const QList<Agent> agents{agent(12, QStringLiteral("12-fix")),
                              agent(13, QStringLiteral("13-ui")),
                              agent(14, QStringLiteral("14-gone"))};
    const QList<Worktree> worktrees{worktree(QStringLiteral("12-fix")),
                                    worktree(QStringLiteral("14-gone"), true)};
    const QSet<QString> branches{QStringLiteral("12-fix"),
                                 QStringLiteral("14-gone")};
    const QSet<QString> found = keys(ap::check(agents, worktrees, branches, kRoot));
    expect(found.contains(QStringLiteral("branch:13")),
          "agent: missing branch flagged");
    expect(found.contains(QStringLiteral("worktree:13")),
          "agent: missing worktree flagged");
    expect(found.contains(QStringLiteral("worktree:14")),
          "agent: worktree whose folder is gone flagged");
    expect(!found.contains(QStringLiteral("branch:12")) &&
              !found.contains(QStringLiteral("worktree:12")),
          "agent: complete agent not flagged");
}

void testAgentWithoutWorktreeNeed()
{
    Agent finished = agent(20, QStringLiteral("20-runner"), false);
    Agent direct = agent(21, QString(), false);
    direct.needsBranch = false;
    direct.folder.clear();
    const QSet<QString> found =
        keys(ap::check({finished, direct}, {}, {QStringLiteral("20-runner")}, kRoot));
    expect(found.isEmpty(),
          "agent: finished runner and direct-checkout agents need no worktree");
}

void testOrphans()
{
    const QList<Agent> agents{agent(12, QStringLiteral("12-fix"))};
    const QList<Worktree> worktrees{
        worktree(QStringLiteral("12-fix")), worktree(QStringLiteral("30-left")),
        Worktree{kRoot + QStringLiteral("/scratch-copy"), QString(), false},
        Worktree{kRoot + QStringLiteral("/.scratch"), QString(), false},
        Worktree{QStringLiteral("/elsewhere/31-manual"),
                 QStringLiteral("31-manual"), false}};
    const QSet<QString> branches{
        QStringLiteral("main"), QStringLiteral("12-fix"), QStringLiteral("30-left"),
        QStringLiteral("31-manual"), QStringLiteral("40-stale"),
        QStringLiteral("agent-41-old"), QStringLiteral("feature/x"),
        QStringLiteral("forkmesh/pull/7")};
    const QSet<QString> found = keys(ap::check(agents, worktrees, branches, kRoot));
    expect(found.contains(QStringLiteral("orphan-worktree:/repo/.worktrees/30-left")),
          "orphan: worktree without agent flagged");
    expect(found.contains(
              QStringLiteral("orphan-worktree:/repo/.worktrees/scratch-copy")),
          "orphan: detached worktree without agent flagged");
    expect(!found.contains(QStringLiteral("orphan-worktree:/repo/.worktrees/.scratch")),
          "orphan: hidden folders under the agent root ignored");
    expect(!found.contains(QStringLiteral("orphan-worktree:/elsewhere/31-manual")),
          "orphan: worktrees outside the agent root ignored");
    expect(found.contains(QStringLiteral("orphan-branch:40-stale")) &&
              found.contains(QStringLiteral("orphan-branch:agent-41-old")),
          "orphan: agent-named branch without agent or worktree flagged");
    expect(!found.contains(QStringLiteral("orphan-branch:30-left")) &&
              !found.contains(QStringLiteral("orphan-branch:31-manual")),
          "orphan: checked-out branch reported through its worktree only");
    expect(!found.contains(QStringLiteral("orphan-branch:feature/x")) &&
              !found.contains(QStringLiteral("orphan-branch:forkmesh/pull/7")) &&
              !found.contains(QStringLiteral("orphan-branch:main")),
          "orphan: non-agent branches ignored");
    expect(found.size() == 4, "orphan: nothing else flagged");
}

void testNoAgentRoot()
{
    const QList<Worktree> worktrees{worktree(QStringLiteral("30-left"))};
    const QSet<QString> found =
        keys(ap::check({}, worktrees, {QStringLiteral("30-left")}, QString()));
    expect(found.isEmpty(),
          "root: no worktree is an orphan when the checkout has no agent root");
}

void testRecycledAgent()
{
    Agent recycled = agent(50, QStringLiteral("50-deleted"));
    recycled.recycled = true;
    const QList<Worktree> worktrees{worktree(QStringLiteral("50-deleted"))};
    const QSet<QString> found = keys(
        ap::check({recycled}, worktrees, {QStringLiteral("50-deleted")}, kRoot));
    expect(!found.contains(QStringLiteral("orphan-branch:50-deleted")),
          "recycled: branch kept while the agent is in the recycle bin");
    expect(found.contains(
              QStringLiteral("orphan-worktree:/repo/.worktrees/50-deleted")),
          "recycled: its worktree is still flagged");
    const QSet<QString> branchOnly =
        keys(ap::check({recycled}, {}, {QStringLiteral("50-deleted")}, kRoot));
    expect(branchOnly.isEmpty(),
          "recycled: no missing-branch or missing-worktree checks");
}

void testWatch()
{
    Watch watch;
    const QList<Finding> first{
        Finding{QStringLiteral("o/r"),
                Issue{QStringLiteral("branch:1"), QStringLiteral("A")}}};
    expect(watch.observe(first).isEmpty(), "watch: first sighting waits");
    const Alert alert = watch.observe(first);
    expect(!alert.isEmpty() && alert.body.startsWith(QStringLiteral("A\n")) &&
              alert.title.startsWith(QStringLiteral("An agent")),
          "watch: second sighting alerts");
    expect(watch.observe(first).isEmpty(), "watch: no repeat while unchanged");
    expect(watch.observe(QList<Finding>{}).isEmpty(), "watch: cleared");
    expect(watch.observe(first).isEmpty(), "watch: returning issue waits again");
    expect(!watch.observe(first).isEmpty(), "watch: returning issue re-alerts");

    Watch many;
    QList<Finding> lots;
    for (int i = 0; i < kMaxListedIssues + 3; ++i)
        lots << Finding{i % 2 ? QStringLiteral("a/x") : QStringLiteral("b/y"),
                        Issue{QStringLiteral("k%1").arg(i),
                              QStringLiteral("issue %1").arg(i)}};
    many.observe(lots);
    const Alert big = many.observe(lots);
    expect(big.title.endsWith(QStringLiteral("(%1 issues)").arg(lots.size())),
          "watch: title counts every new issue");
    expect(big.body.contains(QStringLiteral("…and 3 more.")),
          "watch: long lists are cut");
    expect(big.body.contains(QStringLiteral("b/y: issue 0")),
          "watch: repository named when several have issues");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testParse();
    testInParity();
    testAgentMissingPieces();
    testAgentWithoutWorktreeNeed();
    testOrphans();
    testNoAgentRoot();
    testRecycledAgent();
    testWatch();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

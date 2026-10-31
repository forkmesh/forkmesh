#include "UpstreamCheckoutSync.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdio>

// The headless-fleet regression this suite pins down: a node that seals its
// public mirror from the installer-provisioned agent checkout must keep that
// checkout tracking the upstream relay clone. Before UpstreamCheckoutSync,
// the checkout froze at its install-time snapshot (the mirror6/7/8 outage of
// 2026-07-30): new upstream commits, branches, and the pulls metadata branch
// never arrived, and the node served stale data forever while claiming to be
// a healthy mirror.

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
                       QStringLiteral("Upstream Sync Test"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                       QStringLiteral("upstream-sync@example.invalid"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"),
                       QStringLiteral("Upstream Sync Test"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                       QStringLiteral("upstream-sync@example.invalid"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), path} + arguments);
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}

QString revParse(const QString &path, const QString &ref)
{
    QByteArray out;
    if (!git(path, {QStringLiteral("rev-parse"), ref}, &out))
        return QString();
    return QString::fromUtf8(out).trimmed();
}

bool commitFile(const QString &repo, const QString &name,
                const QByteArray &bytes, const QString &message)
{
    QFile file(QDir(repo).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size())
        return false;
    file.close();
    return git(repo, {QStringLiteral("add"), name}) &&
           git(repo, {QStringLiteral("commit"), QStringLiteral("-q"),
                      QStringLiteral("-m"), message});
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::fprintf(stderr, "FAIL: no temporary directory\n");
        return 1;
    }
    const QDir root(temp.path());

    // Upstream: the relay-served source of truth, with main, a side branch,
    // the pulls metadata branch, and a branch that will later disappear.
    const QString upstream = root.filePath(QStringLiteral("upstream"));
    QDir().mkpath(upstream);
    check(git(upstream, {QStringLiteral("init"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("main")}),
          "init upstream");
    check(commitFile(upstream, QStringLiteral("README.md"),
                     QByteArrayLiteral("hello\n"), QStringLiteral("first")),
          "seed upstream");
    check(QDir(upstream).mkpath(QStringLiteral(".forkmesh/stats")),
          "create tracked stats directory");
    check(commitFile(upstream,
                     QStringLiteral(".forkmesh/stats/repository.json"),
                     QByteArrayLiteral("{\"generated\":false}\n"),
                     QStringLiteral("seed generated stats")),
          "seed tracked node stats");
    check(git(upstream, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("-b"),
                         QStringLiteral("forkmesh/pull/7")}),
          "branch independent pull metadata");
    check(QDir(upstream).mkpath(QStringLiteral(".forkmesh/pulls/7")),
          "create independent pull metadata directory");
    check(commitFile(upstream, QStringLiteral(".forkmesh/pulls/7/pull.md"),
                     QByteArrayLiteral("---\nnumber: 7\n---\n"),
                     QStringLiteral("independent pull metadata")),
          "commit independent pull metadata");
    check(git(upstream, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("main")}),
          "return from independent pull metadata");
    check(git(upstream, {QStringLiteral("branch"),
                         QStringLiteral("agent/adhoc-1-stale-claim")}),
          "seed stale branch");
    check(git(upstream, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("feature/live")}),
          "branch feature");
    check(commitFile(upstream, QStringLiteral("feature.txt"),
                     QByteArrayLiteral("f1\n"), QStringLiteral("feature 1")),
          "commit feature");
    check(git(upstream, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("--orphan"),
                         QStringLiteral("forkmesh/pulls")}),
          "orphan pulls");
    check(git(upstream, {QStringLiteral("rm"), QStringLiteral("-rfq"),
                         QStringLiteral("--ignore-unmatch"),
                         QStringLiteral(".")}),
          "clear pulls tree");
    check(commitFile(upstream, QStringLiteral("pulls.json"),
                     QByteArrayLiteral("{\"pulls\":[]}\n"),
                     QStringLiteral("pull metadata")),
          "commit pulls metadata");
    check(git(upstream, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("main")}),
          "back to main");

    // Node checkout: the installer's single-branch clone, exactly like
    // launch_root_headless_service seeds it.
    const QString checkout = root.filePath(QStringLiteral("checkout"));
    check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                            QStringLiteral("--branch"), QStringLiteral("main"),
                            QStringLiteral("--single-branch"), upstream,
                            checkout}),
          "single-branch clone");
    // Local junk pulls branch (the pre-fix nodes grew one) plus a stale agent
    // claim branch pointing at the clone-time tip, and one branch with real
    // local-only work that must survive every refresh.
    check(git(checkout, {QStringLiteral("branch"),
                         QStringLiteral("forkmesh/pulls")}),
          "local junk pulls branch");
    check(git(checkout, {QStringLiteral("branch"),
                         QStringLiteral("agent/adhoc-1-stale-claim")}),
          "local stale claim");
    check(git(checkout, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("-b"),
                         QStringLiteral("agent/adhoc-9-wip")}),
          "branch wip");
    check(commitFile(checkout, QStringLiteral("wip.txt"),
                     QByteArrayLiteral("wip\n"), QStringLiteral("agent wip")),
          "commit wip");
    check(git(checkout, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("main")}),
          "back on main");
    check(git(checkout, {QStringLiteral("branch"),
                         QStringLiteral("forkmesh/pull/7"),
                         QStringLiteral("agent/adhoc-9-wip")}),
          "seed divergent independent pull metadata");
    check(git(checkout, {QStringLiteral("branch"),
                         QStringLiteral("forkmesh/pull/8"),
                         QStringLiteral("agent/adhoc-9-wip")}),
          "seed deleted independent pull metadata");
    // A linked worktree keeps the pulls branch checked out, mirroring the
    // app's pull-meta materialization (the case that broke a naive refresh).
    const QString pullsWorktree = root.filePath(QStringLiteral("pull-meta"));
    check(git(checkout, {QStringLiteral("worktree"), QStringLiteral("add"),
                         QStringLiteral("-q"), pullsWorktree,
                         QStringLiteral("forkmesh/pulls")}),
          "pulls worktree");

    // Upstream moves on: main advances, the stale claim branch is deleted.
    check(commitFile(upstream, QStringLiteral("README.md"),
                     QByteArrayLiteral("hello v2\n"), QStringLiteral("second")),
          "advance upstream main");
    check(git(upstream, {QStringLiteral("branch"), QStringLiteral("-q"),
                         QStringLiteral("-D"),
                         QStringLiteral("agent/adhoc-1-stale-claim")}),
          "delete upstream stale claim");

    // --- The refresh under test -------------------------------------------
    const auto outcome =
        forkmesh::upstream::refreshManagedCheckoutFromUpstream(checkout,
                                                               upstream);
    check(outcome.error.isEmpty(), "refresh reports no error");
    check(outcome.fetchOk, "refresh fetched");
    check(outcome.headFastForwarded, "checked-out main fast-forwarded");
    check(!outcome.summary().isEmpty(), "changes produce a summary");

    check(revParse(checkout, QStringLiteral("main")) ==
              revParse(upstream, QStringLiteral("main")),
          "main matches upstream");
    check(revParse(checkout, QStringLiteral("feature/live")) ==
              revParse(upstream, QStringLiteral("feature/live")),
          "new upstream branch materialized locally");
    check(revParse(checkout, QStringLiteral("forkmesh/pulls")) ==
              revParse(upstream, QStringLiteral("forkmesh/pulls")),
          "pulls metadata force-tracked across divergence");
    check(revParse(checkout, QStringLiteral("forkmesh/pull/7")) ==
              revParse(upstream, QStringLiteral("forkmesh/pull/7")),
          "independent pull metadata force-tracked across divergence");
    check(revParse(checkout, QStringLiteral("forkmesh/pull/8")).isEmpty(),
          "deleted independent pull metadata pruned instead of resurfacing");
    check(revParse(checkout,
                   QStringLiteral("agent/adhoc-1-stale-claim")).isEmpty(),
          "stale merged claim branch pruned");
    check(!revParse(checkout, QStringLiteral("agent/adhoc-9-wip")).isEmpty(),
          "diverged agent work preserved");
    QByteArray wipList;
    check(git(checkout, {QStringLiteral("log"), QStringLiteral("-1"),
                         QStringLiteral("--format=%s"),
                         QStringLiteral("agent/adhoc-9-wip")},
              &wipList) &&
              QString::fromUtf8(wipList).trimmed() ==
                  QStringLiteral("agent wip"),
          "agent wip tip untouched");

    // A second refresh with nothing new is a quiet no-op.
    const auto again =
        forkmesh::upstream::refreshManagedCheckoutFromUpstream(checkout,
                                                               upstream);
    check(again.error.isEmpty(), "no-op refresh reports no error");
    check(again.summary().isEmpty(), "no-op refresh has an empty summary");

    // Node-local generated stats are tracked but unrelated to this upstream
    // change. They must not freeze the service-managed checkout forever.
    check(commitFile(upstream, QStringLiteral("README.md"),
                     QByteArrayLiteral("hello v3\n"), QStringLiteral("third")),
          "advance upstream again");
    QFile stats(QDir(checkout).filePath(
        QStringLiteral(".forkmesh/stats/repository.json")));
    check(stats.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              stats.write(QByteArrayLiteral("{\"generated\":true}\n")) > 0,
          "update node-local tracked stats");
    stats.close();
    const auto withStats =
        forkmesh::upstream::refreshManagedCheckoutFromUpstream(checkout,
                                                               upstream);
    check(withStats.error.isEmpty(), "stats refresh reports no error");
    check(withStats.headFastForwarded,
          "unrelated tracked stats do not block fast-forward");
    check(revParse(checkout, QStringLiteral("main")) ==
              revParse(upstream, QStringLiteral("main")),
          "main advances with node-local stats present");
    check(stats.open(QIODevice::ReadOnly) &&
              stats.readAll() == QByteArrayLiteral("{\"generated\":true}\n"),
          "node-local stats survive fast-forward");
    stats.close();

    // If upstream changed that same generated file, Git cannot preserve both
    // versions. A managed node may discard only this known regenerated path and
    // retry; that is the recovery needed by old fleet checkouts.
    check(commitFile(upstream,
                     QStringLiteral(".forkmesh/stats/repository.json"),
                     QByteArrayLiteral("{\"generated\":\"upstream\"}\n"),
                     QStringLiteral("refresh generated stats schema")),
          "advance upstream generated stats");
    const auto statsConflict =
        forkmesh::upstream::refreshManagedCheckoutFromUpstream(checkout,
                                                               upstream);
    check(statsConflict.error.isEmpty(),
          "generated stats conflict reports no error");
    check(statsConflict.headFastForwarded,
          "generated-only conflict is regenerated and fast-forwarded");
    check(revParse(checkout, QStringLiteral("main")) ==
              revParse(upstream, QStringLiteral("main")),
          "main advances across generated stats conflict");
    check(stats.open(QIODevice::ReadOnly) &&
              stats.readAll() ==
                  QByteArrayLiteral("{\"generated\":\"upstream\"}\n"),
          "generated stats adopts the upstream schema before regeneration");
    stats.close();

    // A conflicting dirty primary checkout must never be overwritten under an
    // agent. Git's merge safety check distinguishes it from the unrelated stats
    // case above.
    check(commitFile(upstream, QStringLiteral("README.md"),
                     QByteArrayLiteral("hello v4\n"), QStringLiteral("fourth")),
          "advance upstream into a conflicting path");
    QFile dirty(QDir(checkout).filePath(QStringLiteral("README.md")));
    check(dirty.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              dirty.write(QByteArrayLiteral("local edit\n")) > 0,
          "dirty the checkout");
    dirty.close();
    const auto guarded =
        forkmesh::upstream::refreshManagedCheckoutFromUpstream(checkout,
                                                               upstream);
    check(guarded.error.isEmpty(), "guarded refresh reports no error");
    check(!guarded.headFastForwarded, "dirty checkout left alone");
    check(revParse(checkout, QStringLiteral("main")) !=
              revParse(upstream, QStringLiteral("main")),
          "dirty main did not move");

    // An unreachable upstream fails closed without touching anything.
    const auto offline = forkmesh::upstream::refreshManagedCheckoutFromUpstream(
        checkout, root.filePath(QStringLiteral("missing")));
    check(!offline.error.isEmpty(), "unreachable upstream reports an error");
    check(!offline.summary().isEmpty(), "failure summary names the problem");

    // A path that is not a checkout is rejected outright.
    const auto notRepo = forkmesh::upstream::refreshManagedCheckoutFromUpstream(
        root.filePath(QStringLiteral("nowhere")), upstream);
    check(!notRepo.error.isEmpty(), "missing checkout rejected");

    // Two mirror nodes may independently win different pages of issue intake
    // while the source desktop is offline. Refresh must read both exact routes
    // and create one clean union instead of accepting whichever round-robin
    // peer happened to answer first.
    const QString meshSeed = root.filePath(QStringLiteral("mesh-seed"));
    QDir().mkpath(meshSeed);
    check(git(meshSeed, {QStringLiteral("init"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("main")}),
          "init issue mesh seed");
    check(commitFile(meshSeed, QStringLiteral("README.md"),
                     QByteArrayLiteral("issue mesh\n"),
                     QStringLiteral("seed issue mesh")),
          "commit issue mesh seed");
    check(QDir(meshSeed).mkpath(
              QStringLiteral(".forkmesh/issues/open/700")),
          "create seed issue directory");
    check(commitFile(
              meshSeed,
              QStringLiteral(".forkmesh/issues/open/700/issue-700.json"),
              QByteArrayLiteral("{\"number\":700,\"title\":\"seed\"}\n"),
              QStringLiteral("seed issue 700")),
          "commit seed issue");
    const QString sourceIssues = root.filePath(QStringLiteral("source-issues"));
    const QString sourceIssuesReverse =
        root.filePath(QStringLiteral("source-issues-reverse"));
    const QString mirrorOne = root.filePath(QStringLiteral("mirror-one"));
    const QString mirrorTwo = root.filePath(QStringLiteral("mirror-two"));
    for (const QString &destination :
         {sourceIssues, sourceIssuesReverse, mirrorOne, mirrorTwo}) {
        check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                meshSeed, destination}),
              "clone issue mesh member");
    }
    check(commitFile(sourceIssues, QStringLiteral("source-owned.txt"),
                     QByteArrayLiteral("canonical source code\n"),
                     QStringLiteral("advance canonical source code")),
          "advance the source on its own lineage");
    check(git(sourceIssuesReverse,
              {QStringLiteral("fetch"), QStringLiteral("-q"), sourceIssues,
               QStringLiteral("main")}) &&
              git(sourceIssuesReverse,
                  {QStringLiteral("reset"), QStringLiteral("--hard"),
                   QStringLiteral("-q"), QStringLiteral("FETCH_HEAD")}),
          "give both source fixtures the same canonical lineage");
    check(git(mirrorOne,
              {QStringLiteral("branch"),
               QStringLiteral("agent/already-merged"),
               QStringLiteral("main")}),
          "create stale mirror-only branch");
    check(commitFile(mirrorOne, QStringLiteral("legacy-mirror-code.txt"),
                     QByteArrayLiteral("must not enter source main\n"),
                     QStringLiteral("legacy mirror code change")),
          "advance mirror code on a divergent lineage");
    check(QDir(mirrorOne).mkpath(
              QStringLiteral(".forkmesh/issues/closed/700")) &&
              git(mirrorOne,
                  {QStringLiteral("mv"),
                   QStringLiteral(
                       ".forkmesh/issues/open/700/issue-700.json"),
                   QStringLiteral(
                       ".forkmesh/issues/closed/700/issue-700.json")}) &&
              git(mirrorOne,
                  {QStringLiteral("commit"), QStringLiteral("-q"),
                   QStringLiteral("-m"), QStringLiteral("close issue 700")}),
          "first mirror closes seed issue");
    check(QDir(mirrorOne).mkpath(
              QStringLiteral(".forkmesh/issues/open/701")),
          "create first mirror issue directory");
    check(commitFile(
              mirrorOne,
              QStringLiteral(".forkmesh/issues/open/701/issue-701.json"),
              QByteArrayLiteral("{\"number\":701,\"title\":\"one\"}\n"),
              QStringLiteral("mirror one drains issue 701")),
          "first mirror commits its issue");
    check(QDir(mirrorOne).mkpath(
              QStringLiteral(".forkmesh/discussions/801")),
          "create first mirror discussion directory");
    check(commitFile(
              mirrorOne,
              QStringLiteral(".forkmesh/discussions/801/discussion.md"),
              QByteArrayLiteral("# Mirror discussion\n"),
              QStringLiteral("mirror one drains discussion 801")),
          "first mirror commits its discussion");
    check(git(mirrorOne,
              {QStringLiteral("checkout"), QStringLiteral("-q"),
               QStringLiteral("-b"), QStringLiteral("agent/unique-work")}),
          "create unique mirror branch");
    check(commitFile(mirrorOne, QStringLiteral("unique.txt"),
                     QByteArrayLiteral("unique mirror work\n"),
                     QStringLiteral("unique mirror branch commit")),
          "commit unique mirror branch work");
    for (const QString &branch : {QStringLiteral("agent/deleted-on-purpose"),
                                  QStringLiteral("agent/deleted-then-advanced")}) {
        check(git(mirrorOne,
                  {QStringLiteral("checkout"), QStringLiteral("-q"),
                   QStringLiteral("-b"), branch, QStringLiteral("main")}) &&
                  commitFile(mirrorOne, QStringLiteral("deleted.txt"),
                             branch.toUtf8() + '\n',
                             QStringLiteral("work the source deletes")) &&
                  git(sourceIssues,
                      {QStringLiteral("fetch"), QStringLiteral("-q"), mirrorOne,
                       branch}) &&
                  git(sourceIssues,
                      {QStringLiteral("update-ref"),
                       forkmesh::upstream::deletedBranchTombstoneRef(branch),
                       QStringLiteral("FETCH_HEAD")}),
              "the source records a branch it deleted on purpose");
    }
    check(commitFile(mirrorOne, QStringLiteral("deleted.txt"),
                     QByteArrayLiteral("pushed after the delete\n"),
                     QStringLiteral("work pushed after the source deleted it")),
          "a mirror advances a deleted branch");
    check(git(mirrorOne,
              {QStringLiteral("checkout"), QStringLiteral("-q"),
               QStringLiteral("main")}),
          "return mirror one to main");
    check(QDir(mirrorTwo).mkpath(
              QStringLiteral(".forkmesh/issues/open/702")),
          "create second mirror issue directory");
    check(commitFile(
              mirrorTwo,
              QStringLiteral(".forkmesh/issues/open/702/issue-702.json"),
              QByteArrayLiteral("{\"number\":702,\"title\":\"two\"}\n"),
              QStringLiteral("mirror two drains issue 702")),
          "second mirror commits its issue");
    check(QDir(mirrorTwo).mkpath(QStringLiteral(".forkmesh/pulls/901")),
          "create second mirror pull directory");
    check(commitFile(
              mirrorTwo, QStringLiteral(".forkmesh/pulls/901/pull.md"),
              QByteArrayLiteral("# Mirror pull\n"),
              QStringLiteral("mirror two drains pull 901")),
          "second mirror commits its pull ledger");

    int reachableRoutePublications = 0;
    QStringList publishedTips;
    const auto allMirrors =
        forkmesh::upstream::convergeSourceCheckoutFromRoutes(
            sourceIssues,
            {{mirrorOne, QProcessEnvironment()},
             {mirrorTwo, QProcessEnvironment()}},
            [&] {
                ++reachableRoutePublications;
                publishedTips.append(
                    revParse(sourceIssues, QStringLiteral("main")));
            });
    check(allMirrors.error.isEmpty(), "all exact issue mirrors fetched");
    check(allMirrors.fetchOk, "multi-mirror refresh reports a fetch");
    check(reachableRoutePublications == 2,
          "each reachable mirror generation is publishable immediately");
    check(publishedTips.size() == 2 &&
              publishedTips.first() != publishedTips.last(),
          "the first mirror publishes before the second route completes");

    // Direct SSH and public HTTPS are access paths to one mirror, not two
    // independently required mirrors. If the preferred path is temporarily
    // unreadable, the fallback must make the group complete without emitting
    // the partial-convergence error that used to repeat in the network log.
    int fallbackPublications = 0;
    const auto fallbackMirror =
        forkmesh::upstream::convergeSourceCheckoutFromRoutes(
            sourceIssues,
            {{root.filePath(QStringLiteral("missing-preferred-route")),
              QProcessEnvironment(), QStringLiteral("mirror-one")},
             {mirrorOne, QProcessEnvironment(),
              QStringLiteral("mirror-one")},
             {mirrorTwo, QProcessEnvironment(),
              QStringLiteral("mirror-two")}},
            [&] { ++fallbackPublications; });
    check(fallbackMirror.error.isEmpty(),
          "a readable fallback completes its mirror route group");
    check(fallbackMirror.fetchOk && fallbackPublications == 2,
          "each mirror publishes once even when one route falls back");
    const auto unavailableMirror =
        forkmesh::upstream::convergeSourceCheckoutFromRoutes(
            sourceIssues,
            {{root.filePath(QStringLiteral("missing-route-one")),
              QProcessEnvironment(), QStringLiteral("offline-mirror")},
             {root.filePath(QStringLiteral("missing-route-two")),
              QProcessEnvironment(), QStringLiteral("offline-mirror")},
             {mirrorOne, QProcessEnvironment(),
              QStringLiteral("healthy-mirror")}});
    check(unavailableMirror.error ==
              QStringLiteral("1 mirror could not be read"),
          "an unreadable fallback group still fails closed as one mirror");
    check(QFileInfo::exists(QDir(sourceIssues).filePath(
              QStringLiteral(".forkmesh/issues/open/701/issue-701.json"))),
          "first mirror issue lands in source checkout");
    check(QFileInfo::exists(QDir(sourceIssues).filePath(
              QStringLiteral(".forkmesh/issues/open/702/issue-702.json"))),
          "second mirror issue lands in source checkout");
    check(QFileInfo::exists(QDir(sourceIssues).filePath(
              QStringLiteral(".forkmesh/issues/closed/700/issue-700.json"))) &&
              !QFileInfo::exists(QDir(sourceIssues).filePath(
                  QStringLiteral(".forkmesh/issues/open/700/issue-700.json"))),
          "mirror issue closure lands in source checkout");
    check(QFileInfo::exists(QDir(sourceIssues).filePath(
              QStringLiteral(".forkmesh/discussions/801/discussion.md"))),
          "mirror discussion lands in source checkout");
    check(QFileInfo::exists(QDir(sourceIssues).filePath(
              QStringLiteral(".forkmesh/pulls/901/pull.md"))),
          "mirror pull ledger lands in source checkout");
    check(QFileInfo::exists(
              QDir(sourceIssues).filePath(QStringLiteral("source-owned.txt"))) &&
              !QFileInfo::exists(QDir(sourceIssues).filePath(
                  QStringLiteral("legacy-mirror-code.txt"))),
          "selective reconciliation keeps source code canonical");
    check(revParse(sourceIssues,
                   QStringLiteral("agent/already-merged")).isEmpty(),
          "source does not resurrect a stale mirror-only branch");
    check(!revParse(sourceIssues,
                    QStringLiteral("agent/unique-work")).isEmpty(),
          "source preserves a mirror-only branch with unique work");
    check(revParse(sourceIssues,
                   QStringLiteral("refs/heads/agent/deleted-on-purpose")).isEmpty(),
          "source does not resurrect a branch it deleted on purpose");
    check(!revParse(sourceIssues,
                    QStringLiteral("refs/heads/agent/deleted-then-advanced")).isEmpty(),
          "source keeps work a mirror added after the delete");

    const auto reversedMirrors =
        forkmesh::upstream::convergeSourceCheckoutFromMirrors(
            sourceIssuesReverse, QStringList{mirrorTwo, mirrorOne});
    check(reversedMirrors.error.isEmpty(),
          "reversed exact issue mirrors fetched");
    check(revParse(sourceIssuesReverse, QStringLiteral("main^{tree}")) ==
              revParse(sourceIssues, QStringLiteral("main^{tree}")),
          "collaboration reconciliation produces identical content across route order");

    // A rewritten branch can contain the same files under different commit
    // IDs. Bridge both histories without changing its tree so subsequent
    // mirror propagation is a normal fast-forward, not a permanent rejection.
    const QString equivalentSource =
        root.filePath(QStringLiteral("equivalent-source"));
    const QString equivalentPeer =
        root.filePath(QStringLiteral("equivalent-peer"));
    for (const QString &destination : {equivalentSource, equivalentPeer}) {
        check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                meshSeed, destination}),
              "clone equivalent-history fixture");
    }
    check(commitFile(equivalentSource, QStringLiteral("equivalent.txt"),
                     QByteArrayLiteral("same tree\n"),
                     QStringLiteral("source spelling")) &&
              commitFile(equivalentPeer, QStringLiteral("equivalent.txt"),
                         QByteArrayLiteral("same tree\n"),
                         QStringLiteral("peer spelling")),
          "create equal trees on divergent histories");
    const QString equivalentPeerTip =
        revParse(equivalentPeer, QStringLiteral("main"));
    const auto equivalentOutcome =
        forkmesh::upstream::convergeSourceCheckoutFromMirrors(
            equivalentSource, QStringList{equivalentPeer});
    check(equivalentOutcome.error.isEmpty() &&
              git(equivalentSource,
                  {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
                   equivalentPeerTip, QStringLiteral("main")}),
          "equal-tree divergence is bridged for fast-forward propagation");

    // A mirror that has healed its own intake divergence leaves the source and
    // the fleet sharing TWO merge bases: the mirror merged two lanes the
    // source already holds separately. That shape used to abort reconciliation
    // outright, deadlocking both directions — the source could not
    // fast-forward to the mirror, the fast-forward-only fan-out could not move
    // the mirror to the source, and the fleet froze at the mirror's commit.
    const QString multiSource =
        root.filePath(QStringLiteral("multi-base-source"));
    const QString multiPeer = root.filePath(QStringLiteral("multi-base-peer"));
    const QString multiPeerCode =
        root.filePath(QStringLiteral("multi-base-peer-code"));
    check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                            meshSeed, multiSource}),
          "clone multi-base source fixture");
    check(git(multiSource, {QStringLiteral("checkout"), QStringLiteral("-q"),
                            QStringLiteral("-b"), QStringLiteral("laneA")}) &&
              QDir(multiSource)
                  .mkpath(QStringLiteral(".forkmesh/issues/open/800")) &&
              commitFile(
                  multiSource,
                  QStringLiteral(".forkmesh/issues/open/800/issue-800.json"),
                  QByteArrayLiteral("{\"number\":800}\n"),
                  QStringLiteral("issue #800: mirror intake")),
          "seed intake lane A");
    check(git(multiSource,
              {QStringLiteral("checkout"), QStringLiteral("-q"),
               QStringLiteral("-b"), QStringLiteral("laneB"),
               QStringLiteral("main")}) &&
              QDir(multiSource)
                  .mkpath(QStringLiteral(".forkmesh/issues/open/801")) &&
              commitFile(
                  multiSource,
                  QStringLiteral(".forkmesh/issues/open/801/issue-801.json"),
                  QByteArrayLiteral("{\"number\":801}\n"),
                  QStringLiteral("issue #801: mirror intake")),
          "seed intake lane B");
    check(git(multiSource, {QStringLiteral("checkout"), QStringLiteral("-q"),
                            QStringLiteral("main")}),
          "return multi-base source to main");
    // The source absorbs each lane on its own, so both lane tips — and only
    // those — are merge bases with the peer.
    check(git(multiSource, {QStringLiteral("merge"), QStringLiteral("-q"),
                            QStringLiteral("--no-ff"), QStringLiteral("--no-edit"),
                            QStringLiteral("-m"), QStringLiteral("absorb lane A"),
                            QStringLiteral("laneA")}) &&
              git(multiSource,
                  {QStringLiteral("merge"), QStringLiteral("-q"),
                   QStringLiteral("--no-ff"), QStringLiteral("--no-edit"),
                   QStringLiteral("-m"), QStringLiteral("absorb lane B"),
                   QStringLiteral("laneB")}),
          "source absorbs both lanes separately");
    for (const QString &peer : {multiPeer, multiPeerCode}) {
        check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                multiSource, peer}),
              "clone multi-base peer fixture");
        // The peer heals its own divergence exactly as the mirror reconciler
        // does: one merge of both lanes.
        check(git(peer, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("-B"), QStringLiteral("main"),
                         QStringLiteral("origin/laneA")}) &&
                  git(peer, {QStringLiteral("merge"), QStringLiteral("-q"),
                             QStringLiteral("--no-ff"),
                             QStringLiteral("--no-edit"), QStringLiteral("-m"),
                             QStringLiteral("Merge divergent mirror issue intake"),
                             QStringLiteral("origin/laneB")}),
              "peer merges both intake lanes");
    }
    check(QDir(multiPeer).mkpath(QStringLiteral(".forkmesh/issues/open/802")) &&
              commitFile(
                  multiPeer,
                  QStringLiteral(".forkmesh/issues/open/802/issue-802.json"),
                  QByteArrayLiteral("{\"number\":802}\n"),
                  QStringLiteral("issue #802: mirror intake")),
          "peer takes further intake on the healed lineage");
    QByteArray multiBases;
    check(git(multiPeer,
              {QStringLiteral("merge-base"), QStringLiteral("--all"),
               QStringLiteral("main"), QStringLiteral("origin/main")},
              &multiBases) &&
              QString::fromUtf8(multiBases)
                      .trimmed()
                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts)
                      .size() == 2,
          "fixture really has two merge bases");
    const QString multiPeerTip = revParse(multiPeer, QStringLiteral("main"));
    const auto multiOutcome =
        forkmesh::upstream::convergeSourceCheckoutFromMirrors(
            multiSource, QStringList{multiPeer});
    check(multiOutcome.error.isEmpty(), "multi-base peer fetched");
    check(git(multiSource,
              {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
               multiPeerTip, QStringLiteral("main")}),
          "multi-base collaboration divergence is absorbed, not deadlocked");
    check(QFileInfo::exists(
              QDir(multiSource)
                  .filePath(QStringLiteral(
                      ".forkmesh/issues/open/802/issue-802.json"))),
          "multi-base absorption carries the stranded intake into the source");

    // The same shape carrying source code is still left alone: a side that
    // several bases cannot describe must never be merged automatically.
    check(commitFile(multiPeerCode, QStringLiteral("README.md"),
                     QByteArrayLiteral("peer rewrote the code\n"),
                     QStringLiteral("peer code change")),
          "peer changes code on the healed lineage");
    const QString multiPeerCodeTip =
        revParse(multiPeerCode, QStringLiteral("main"));
    const auto multiCodeOutcome =
        forkmesh::upstream::convergeSourceCheckoutFromMirrors(
            multiSource, QStringList{multiPeerCode});
    check(multiCodeOutcome.error.isEmpty(), "multi-base code peer fetched");
    check(!git(multiSource,
               {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
                multiPeerCodeTip, QStringLiteral("main")}),
          "multi-base source-code divergence is left for the operator");

    const QString sameSource = root.filePath(QStringLiteral("same-issue-source"));
    const QString samePeer = root.filePath(QStringLiteral("same-issue-peer"));
    for (const QString &destination : {sameSource, samePeer})
        check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                meshSeed, destination}),
              "clone same-issue fixture");
    const QString sameIssuePath =
        QStringLiteral(".forkmesh/issues/open/729/issue-729.json");
    check(commitFile(sameSource, QStringLiteral("source-code.txt"),
                     QByteArrayLiteral("source work\n"),
                     QStringLiteral("source code advances")) &&
              QDir(sameSource).mkpath(QStringLiteral(".forkmesh/issues/open/729")) &&
              commitFile(
                  sameSource, sameIssuePath,
                  QByteArrayLiteral(
                      "{\"schema\":\"forkmesh-issue-v1\",\"number\":729,"
                      "\"title\":\"Real title\",\"status\":\"open\","
                      "\"assignees\":[],\"createdAt\":200,\"updatedAt\":300,"
                      "\"events\":[{\"type\":\"open\",\"id\":\"open-729\","
                      "\"sig\":\"\",\"title\":\"\",\"ts\":200},"
                      "{\"type\":\"title\",\"id\":\"title-1\",\"sig\":\"s1\","
                      "\"title\":\"Real title\",\"ts\":300}]}\n"),
                  QStringLiteral("issue #729: title")),
          "source materializes the title events");
    check(QDir(samePeer).mkpath(QStringLiteral(".forkmesh/issues/open/729")) &&
              commitFile(
                  samePeer, sameIssuePath,
                  QByteArrayLiteral(
                      "{\"schema\":\"forkmesh-issue-v1\",\"number\":729,"
                      "\"title\":\"\",\"status\":\"open\",\"assignees\":[],"
                      "\"createdAt\":100,\"updatedAt\":100,"
                      "\"events\":[{\"type\":\"open\",\"id\":\"open-729\","
                      "\"sig\":\"relay-placeholder\",\"title\":\"\",\"ts\":100},"
                      "{\"type\":\"claim\",\"id\":\"claim-1\",\"sig\":\"s2\","
                      "\"assignee\":\"alice\",\"claimState\":\"assigned\","
                      "\"ts\":100}]}\n"),
                  QStringLiteral("issue #729: direct mirror intake")),
          "peer materializes only the claim");
    const QString samePeerTip = revParse(samePeer, QStringLiteral("main"));
    const auto sameOutcome =
        forkmesh::upstream::convergeSourceCheckoutFromMirrors(
            sameSource, QStringList{samePeer});
    check(sameOutcome.error.isEmpty(), "same-issue peer fetched");
    check(git(sameSource,
              {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
               samePeerTip, QStringLiteral("main")}),
          "one issue written on both sides is absorbed, not deadlocked");
    QFile sameIssue(QDir(sameSource).filePath(sameIssuePath));
    const QJsonObject sameRecord =
        sameIssue.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(sameIssue.readAll()).object()
            : QJsonObject();
    QStringList sameEventIds;
    for (const QJsonValue &value : sameRecord.value(QStringLiteral("events")).toArray())
        sameEventIds << value.toObject().value(QStringLiteral("id")).toString();
    check(sameRecord.value(QStringLiteral("title")).toString() ==
              QLatin1String("Real title"),
          "the merged issue keeps its signed title");
    check(sameEventIds == QStringList{QStringLiteral("open-729"),
                                      QStringLiteral("title-1"),
                                      QStringLiteral("claim-1")},
          "the merged issue carries both sides' events once");
    check(sameRecord.value(QStringLiteral("assignees")).toArray() ==
              QJsonArray{QStringLiteral("alice")},
          "the peer's claim is applied to the merged issue");
    check(QFileInfo::exists(
              QDir(sameSource).filePath(QStringLiteral("source-code.txt"))),
          "the source's code survives the issue merge");

    // A desktop fork has one split origin: pull from its source, push to the
    // locally served fork mirror. This is the layout created from the Repos tab.
    const QString forkMirror = root.filePath(QStringLiteral("fork.git"));
    const QString forkCheckout = root.filePath(QStringLiteral("fork-checkout"));
    check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                            QStringLiteral("--mirror"), upstream, forkMirror}),
          "clone fork mirror");
    check(git(forkMirror, {QStringLiteral("remote"), QStringLiteral("remove"),
                           QStringLiteral("origin")}),
          "detach fork mirror from source");
    check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                            forkMirror, forkCheckout}),
          "clone fork checkout");
    QString forkRemoteError;
    check(forkmesh::upstream::configureForkCheckoutRemote(
              forkCheckout, upstream, forkMirror, &forkRemoteError),
          "configure fork split origin");
    QByteArray fetchUrl;
    QByteArray pushUrl;
    check(git(forkCheckout,
              {QStringLiteral("remote"), QStringLiteral("get-url"),
               QStringLiteral("origin")},
              &fetchUrl) &&
              QString::fromUtf8(fetchUrl).trimmed() == upstream,
          "fork origin fetches from source");
    check(git(forkCheckout,
              {QStringLiteral("remote"), QStringLiteral("get-url"),
               QStringLiteral("--push"), QStringLiteral("origin")},
              &pushUrl) &&
              QString::fromUtf8(pushUrl).trimmed() == forkMirror,
          "fork origin pushes to fork mirror");

    check(commitFile(upstream, QStringLiteral("upstream-new.txt"),
                     QByteArrayLiteral("new upstream work\n"),
                     QStringLiteral("source advances after fork")),
          "advance source after fork");
    check(git(forkCheckout, {QStringLiteral("pull"), QStringLiteral("--ff-only")}),
          "plain pull receives source update");
    check(revParse(forkCheckout, QStringLiteral("main")) ==
              revParse(upstream, QStringLiteral("main")),
          "fork main fast-forwards to source");
    check(commitFile(forkCheckout, QStringLiteral("fork-only.txt"),
                     QByteArrayLiteral("fork work\n"),
                     QStringLiteral("work in fork")),
          "commit fork-only work");
    check(git(forkCheckout, {QStringLiteral("push")}),
          "plain push writes to fork mirror");
    check(revParse(forkMirror, QStringLiteral("main")) ==
              revParse(forkCheckout, QStringLiteral("main")),
          "fork mirror receives fork work");
    check(revParse(upstream, QStringLiteral("main")) !=
              revParse(forkCheckout, QStringLiteral("main")),
          "fork push does not modify source");

    {
        const QString source = root.filePath(QStringLiteral("badref-source"));
        const QString mirror = root.filePath(QStringLiteral("badref-mirror.git"));
        QDir().mkpath(source);
        check(git(source, {QStringLiteral("init"), QStringLiteral("-q"),
                           QStringLiteral("-b"), QStringLiteral("main")}),
              "init bad-ref source");
        check(commitFile(source, QStringLiteral("a.txt"), QByteArrayLiteral("a\n"),
                         QStringLiteral("one")),
              "seed bad-ref source");
        check(git(source, {QStringLiteral("branch"),
                           QStringLiteral("608-add-crm-mobile")}) &&
                  git(source, {QStringLiteral("branch"), QStringLiteral("corrupt")}),
              "seed branches that will break in the mirror");
        check(git(root.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                QStringLiteral("--bare"), source, mirror}),
              "clone bad-ref mirror");
        const QString missing =
            QStringLiteral("0123456789abcdef0123456789abcdef01234567");
        const QString corruptOid =
            QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");
        const auto writeFile = [](const QString &path, const QByteArray &bytes) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   file.write(bytes) == bytes.size();
        };
        check(git(mirror, {QStringLiteral("pack-refs"), QStringLiteral("--all"),
                           QStringLiteral("--prune")}),
              "pack the mirror's refs");
        check(writeFile(QDir(mirror).filePath(
                            QStringLiteral("refs/heads/608-add-crm-mobile")),
                        missing.toLatin1() + '\n') &&
                  writeFile(QDir(mirror).filePath(
                                QStringLiteral("refs/heads/corrupt")),
                            corruptOid.toLatin1() + '\n') &&
                  writeFile(QDir(mirror).filePath(
                                QStringLiteral("objects/") + corruptOid.left(2) +
                                QLatin1Char('/') + corruptOid.mid(2)),
                            QByteArray()),
              "point two mirror branches at a missing and a corrupt object");
        const auto gitErr = [](const QString &path, const QStringList &arguments,
                               QString *errors) {
            QProcess process;
            QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("GIT_AUTHOR_NAME"),
                               QStringLiteral("t"));
            environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                               QStringLiteral("t@example.invalid"));
            environment.insert(QStringLiteral("GIT_COMMITTER_NAME"),
                               QStringLiteral("t"));
            environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                               QStringLiteral("t@example.invalid"));
            process.setProcessEnvironment(environment);
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), path} + arguments);
            const bool finished = process.waitForFinished(30000);
            *errors += QString::fromUtf8(process.readAllStandardError());
            return finished && process.exitCode() == 0;
        };
        check(commitFile(source, QStringLiteral("b.txt"), QByteArrayLiteral("b\n"),
                         QStringLiteral("two")),
              "advance bad-ref source");
        const QStringList fetchArgs{
            QStringLiteral("fetch"), QStringLiteral("--prune"), source,
            QStringLiteral("+refs/heads/main:refs/heads/main")};
        QString fetchErrors;
        bool fetched = gitErr(mirror, fetchArgs, &fetchErrors);
        std::fprintf(stderr, "fetch into broken mirror: %s\n",
                     qPrintable(fetchErrors.simplified()));
        check(!fetched &&
                  !forkmesh::upstream::refsNamedAsBadObjects(fetchErrors).isEmpty(),
              "a mirror branch at an unreadable object fails every fetch and "
              "names the ref");
        QString pushErrors;
        check(!gitErr(source, {QStringLiteral("push"), mirror,
                               QStringLiteral("HEAD:refs/pr/310")},
                      &pushErrors) &&
                  pushErrors.contains(QStringLiteral("bad object refs/")),
              "a push into that mirror is refused over the same ref");
        std::fprintf(stderr, "push into broken mirror: %s\n",
                     qPrintable(pushErrors.simplified()));
        QStringList dropped;
        for (int attempt = 0; attempt < 4 && !fetched; ++attempt) {
            dropped += forkmesh::upstream::dropUnreadableRefs(
                mirror, fetchErrors + pushErrors);
            pushErrors.clear();
            fetchErrors.clear();
            fetched = gitErr(mirror, fetchArgs, &fetchErrors);
        }
        check(fetched,
              "the fetch succeeds once the unreadable mirror refs are dropped");
        check(dropped.contains(QStringLiteral("refs/heads/608-add-crm-mobile")) &&
                  dropped.contains(QStringLiteral("refs/heads/corrupt")),
              "both the missing-object and the corrupt-object branch are dropped");
        check(revParse(mirror, QStringLiteral("main")) ==
                  revParse(source, QStringLiteral("main")),
              "the rest of the mirror syncs past the dropped refs");
        check(forkmesh::upstream::dropUnreadableRefs(
                  mirror, QStringLiteral("fatal: bad object refs/heads/main"))
                      .isEmpty() &&
                  !revParse(mirror, QStringLiteral("main")).isEmpty(),
              "a readable ref named in error text is never dropped");
        QString afterErrors;
        check(gitErr(source, {QStringLiteral("push"), mirror,
                              QStringLiteral("HEAD:refs/pr/310")},
                     &afterErrors),
              "a push into the healed mirror is accepted");
    }

    {
        const QString source = root.filePath(QStringLiteral("empty-object-source"));
        const QString mirror = root.filePath(QStringLiteral("empty-object-mirror.git"));
        QDir().mkpath(source);
        check(git(source, {QStringLiteral("init"), QStringLiteral("-q"),
                           QStringLiteral("-b"), QStringLiteral("main")}),
              "init empty-object source");
        check(commitFile(source, QStringLiteral("a.txt"), QByteArrayLiteral("a\n"),
                         QStringLiteral("one")),
              "seed empty-object source");
        check(git(root.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                QStringLiteral("--bare"), source, mirror}),
              "clone empty-object mirror");
        const QString tip = revParse(mirror, QStringLiteral("main"));
        const QString tipObject = tip.left(2) + QLatin1Char('/') + tip.mid(2);
        const QString tipPath =
            QDir(mirror).filePath(QStringLiteral("objects/") + tipObject);
        QFile emptied(tipPath);
        check(QFileInfo(tipPath).size() > 0 && QFile::remove(tipPath) &&
                  emptied.open(QIODevice::WriteOnly),
              "replace the mirror's tip commit with an empty loose object");
        emptied.close();
        check(commitFile(source, QStringLiteral("b.txt"), QByteArrayLiteral("b\n"),
                         QStringLiteral("two")),
              "advance empty-object source");
        const auto gitErr = [](const QString &path, const QStringList &arguments,
                               QString *errors) {
            QProcess process;
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), path} + arguments);
            const bool finished = process.waitForFinished(30000);
            *errors += QString::fromUtf8(process.readAllStandardError());
            return finished && process.exitCode() == 0;
        };
        const QStringList fetchArgs{
            QStringLiteral("fetch"), QStringLiteral("--prune"), source,
            QStringLiteral("+refs/heads/main:refs/heads/main")};
        QString fetchErrors;
        bool fetched = gitErr(mirror, fetchArgs, &fetchErrors);
        std::fprintf(stderr, "fetch into mirror with an empty object: %s\n",
                     qPrintable(fetchErrors.simplified()));
        check(!fetched &&
                  forkmesh::upstream::emptyLooseObjectsNamed(fetchErrors)
                      .contains(tipObject),
              "an empty loose object fails the fetch and git names its file");
        QStringList removed;
        for (int attempt = 0; attempt < 4 && !fetched; ++attempt) {
            removed += forkmesh::upstream::removeEmptyLooseObjects(mirror,
                                                                   fetchErrors);
            forkmesh::upstream::dropUnreadableRefs(mirror, fetchErrors);
            fetchErrors.clear();
            fetched = gitErr(mirror, fetchArgs, &fetchErrors);
        }
        check(fetched, "the fetch succeeds once the empty object is removed");
        check(removed == QStringList{tip}, "exactly the empty object is removed");
        check(revParse(mirror, QStringLiteral("main")) ==
                  revParse(source, QStringLiteral("main")),
              "the repaired mirror syncs to the source");
        const QString blob = revParse(mirror, QStringLiteral("main:a.txt"));
        const QString blobPath = QDir(mirror).filePath(
            QStringLiteral("objects/") + blob.left(2) + QLatin1Char('/') +
            blob.mid(2));
        check(QFileInfo(blobPath).size() > 0 &&
                  forkmesh::upstream::removeEmptyLooseObjects(
                      mirror, QStringLiteral("error: object file ./objects/") +
                                  blob.left(2) + QLatin1Char('/') + blob.mid(2) +
                                  QStringLiteral(" is empty"))
                      .isEmpty() &&
                  QFileInfo(blobPath).size() > 0,
              "a readable object named in error text is never removed");
    }

    {
        const QString served = root.filePath(QStringLiteral("cloud-served.git"));
        const QString runner = root.filePath(QStringLiteral("cloud-runner"));
        const QString desk = root.filePath(QStringLiteral("cloud-desktop"));
        check(git(temp.path(), {QStringLiteral("init"), QStringLiteral("-q"),
                                QStringLiteral("--bare"), QStringLiteral("-b"),
                                QStringLiteral("main"), served}),
              "init served cloud repository");
        QDir().mkpath(runner);
        check(git(runner, {QStringLiteral("init"), QStringLiteral("-q"),
                           QStringLiteral("-b"), QStringLiteral("main")}) &&
                  commitFile(runner, QStringLiteral("app.txt"),
                             QByteArrayLiteral("base\n"),
                             QStringLiteral("base")) &&
                  git(runner, {QStringLiteral("push"), QStringLiteral("-q"),
                               served, QStringLiteral("main")}),
              "seed served main");
        const auto runnerPush = [&](const QString &branch, const QByteArray &bytes) {
            return git(runner, {QStringLiteral("checkout"), QStringLiteral("-q"),
                                QStringLiteral("-B"), branch}) &&
                   commitFile(runner, branch.section(QLatin1Char('/'), -1) +
                                          QStringLiteral(".txt"),
                              bytes, branch) &&
                   git(runner, {QStringLiteral("push"), QStringLiteral("-q"),
                                served,
                                QStringLiteral("HEAD:refs/heads/") + branch});
        };
        check(runnerPush(QStringLiteral("agent/run-1"), "one\n") &&
                  git(runner, {QStringLiteral("checkout"), QStringLiteral("-q"),
                               QStringLiteral("main")}) &&
                  runnerPush(QStringLiteral("agent/run-2"), "two\n"),
              "runner publishes two cloud agent branches");
        check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                QStringLiteral("--single-branch"), served,
                                desk}),
              "desktop clone of main only");
        const QString unreachable = root.filePath(QStringLiteral("no-such-core"));
        const QStringList sources{unreachable, served};

        auto sync = forkmesh::upstream::syncServedAgentBranches(
            desk, sources,
            {QStringLiteral("agent/run-1"), QStringLiteral("agent/run-2"),
             QStringLiteral("agent/absent"), QStringLiteral("main")});
        check(sync.reachedSource, "the served source is reached past a dead one");
        check(sync.updated.contains(QStringLiteral("agent/run-1")) &&
                  sync.updated.contains(QStringLiteral("agent/run-2")),
              "cloud agent branches are created on the desktop");
        check(revParse(desk, QStringLiteral("refs/heads/agent/run-1")) ==
                  revParse(served, QStringLiteral("refs/heads/agent/run-1")),
              "the created branch is at the served tip");
        check(sync.missing == QStringList{QStringLiteral("agent/absent")},
              "an unpublished branch is reported missing");
        check(sync.present.contains(QStringLiteral("agent/run-1")) &&
                  !sync.present.contains(QStringLiteral("agent/absent")) &&
                  !sync.present.contains(QStringLiteral("main")),
              "present lists only the synced agent branches");
        check(!QFileInfo::exists(
                  QDir(desk).filePath(QStringLiteral(".git/FETCH_HEAD"))),
              "the sync leaves FETCH_HEAD alone");

        sync = forkmesh::upstream::syncServedAgentBranches(
            desk, sources, {QStringLiteral("agent/run-1")});
        check(sync.current == QStringList{QStringLiteral("agent/run-1")} &&
                  sync.updated.isEmpty(),
              "an unchanged branch is left current");

        check(git(runner, {QStringLiteral("checkout"), QStringLiteral("-q"),
                           QStringLiteral("agent/run-1")}) &&
                  runnerPush(QStringLiteral("agent/run-1"), "one v2\n"),
              "runner advances agent/run-1");
        sync = forkmesh::upstream::syncServedAgentBranches(
            desk, sources, {QStringLiteral("agent/run-1")});
        check(sync.updated == QStringList{QStringLiteral("agent/run-1")} &&
                  revParse(desk, QStringLiteral("refs/heads/agent/run-1")) ==
                      revParse(served, QStringLiteral("refs/heads/agent/run-1")),
              "a pushed cloud branch fast-forwards on the desktop");

        check(git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("agent/run-2")}) &&
                  commitFile(desk, QStringLiteral("local.txt"),
                             QByteArrayLiteral("mine\n"),
                             QStringLiteral("local work")) &&
                  git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                             QStringLiteral("main")}),
              "desktop commits on agent/run-2");
        const QString localTwo = revParse(desk, QStringLiteral("refs/heads/agent/run-2"));
        check(git(runner, {QStringLiteral("checkout"), QStringLiteral("-q"),
                           QStringLiteral("agent/run-2")}) &&
                  runnerPush(QStringLiteral("agent/run-2"), "two v2\n"),
              "runner advances agent/run-2");
        const QString worktree = root.filePath(QStringLiteral("cloud-wt"));
        check(git(desk, {QStringLiteral("worktree"), QStringLiteral("add"),
                         QStringLiteral("-q"), worktree,
                         QStringLiteral("agent/run-1")}),
              "agent/run-1 checked out in a worktree");
        const QString checkedOutOne =
            revParse(desk, QStringLiteral("refs/heads/agent/run-1"));
        check(git(runner, {QStringLiteral("checkout"), QStringLiteral("-q"),
                           QStringLiteral("agent/run-1")}) &&
                  runnerPush(QStringLiteral("agent/run-1"), "one v3\n"),
              "runner advances agent/run-1 again");
        sync = forkmesh::upstream::syncServedAgentBranches(
            desk, sources,
            {QStringLiteral("agent/run-1"), QStringLiteral("agent/run-2")});
        check(sync.kept.contains(QStringLiteral("agent/run-1")) &&
                  sync.kept.contains(QStringLiteral("agent/run-2")) &&
                  sync.updated.isEmpty(),
              "diverged and checked-out branches are kept");
        check(revParse(desk, QStringLiteral("refs/heads/agent/run-2")) == localTwo,
              "local commits on a cloud branch are never clobbered");
        check(revParse(desk, QStringLiteral("refs/heads/agent/run-1")) ==
                  checkedOutOne,
              "a branch checked out in a worktree is not moved under it");

        sync = forkmesh::upstream::syncServedAgentBranches(
            desk, {unreachable}, {QStringLiteral("agent/run-1")});
        check(!sync.reachedSource &&
                  sync.present == QStringList{QStringLiteral("agent/run-1")},
              "an unreachable source still reports the local copy");
    }

    {
        const QString core = root.filePath(QStringLiteral("trash-core"));
        const QString desk = root.filePath(QStringLiteral("trash-desk"));
        QDir().mkpath(core);
        check(git(core, {QStringLiteral("init"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("main")}) &&
                  commitFile(core, QStringLiteral("README.md"),
                             QByteArrayLiteral("core\n"),
                             QStringLiteral("core seed")) &&
                  git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                    core, desk}),
              "seed the trash core and the desktop");
        const QString base = revParse(desk, QStringLiteral("main"));
        const auto pushBranch = [&](const QString &branch) {
            return git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                              QStringLiteral("-b"), branch, base}) &&
                   commitFile(desk, QStringLiteral("work.txt"),
                              branch.toUtf8() + '\n',
                              QStringLiteral("work on ") + branch) &&
                   git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                              QStringLiteral("main")}) &&
                   git(desk, {QStringLiteral("push"), QStringLiteral("-q"),
                              core, branch});
        };
        const auto webDelete = [&](const QString &branch, const QString &kind,
                                   const QString &token) {
            const QString tip =
                revParse(core, QStringLiteral("refs/heads/") + branch);
            return !tip.isEmpty() &&
                   git(core, {QStringLiteral("update-ref"),
                              QStringLiteral("refs/forkmesh/trash/") + kind +
                                  QLatin1Char('/') + branch + QLatin1Char('/') +
                                  token,
                              tip}) &&
                   git(core, {QStringLiteral("branch"), QStringLiteral("-q"),
                              QStringLiteral("-D"), branch});
        };
        const auto converge = [&] {
            return forkmesh::upstream::convergeSourceCheckoutFromRoutes(
                desk, {{core, QProcessEnvironment()}});
        };
        const QString deletedOnWeb = QStringLiteral("feature/deleted-on-web");
        const QString agentWeb = QStringLiteral("agent/web-cleaned");
        const QString inWorktree = QStringLiteral("feature/in-worktree");
        const QString advanced = QStringLiteral("feature/advanced-locally");
        const QString hidden = QStringLiteral("feature/hidden-trash");
        for (const QString &branch :
             {deletedOnWeb, agentWeb, inWorktree, advanced, hidden})
            check(pushBranch(branch), "the desktop pushes a branch to the core");
        check(git(desk, {QStringLiteral("branch"),
                         QStringLiteral("feature/never-pushed"), base}),
              "a local branch that was never pushed");
        check(git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("feature/reused-name"),
                         base}) &&
                  commitFile(desk, QStringLiteral("reused.txt"),
                             QByteArrayLiteral("mine\n"),
                             QStringLiteral("never pushed work")) &&
                  git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                             QStringLiteral("main")}),
              "a never-pushed branch whose name the core once deleted");
        const QString reusedTip =
            revParse(desk, QStringLiteral("refs/heads/feature/reused-name"));
        const QString neverPushedTip =
            revParse(desk, QStringLiteral("refs/heads/feature/never-pushed"));
        const auto seenFirst = converge();
        check(seenFirst.error.isEmpty() && seenFirst.branchesPruned == 0,
              "a converge with nothing deleted prunes nothing");

        const QString deletedTip =
            revParse(desk, QStringLiteral("refs/heads/") + deletedOnWeb);
        check(git(core, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         agentWeb}) &&
                  commitFile(core, QStringLiteral("web.txt"),
                             QByteArrayLiteral("web\n"),
                             QStringLiteral("commit made on the web")) &&
                  git(core, {QStringLiteral("checkout"), QStringLiteral("-q"),
                             QStringLiteral("main")}),
              "the web adds a commit before deleting the agent branch");
        const QString agentLocalTip =
            revParse(desk, QStringLiteral("refs/heads/") + agentWeb);
        check(webDelete(deletedOnWeb, QStringLiteral("branch"),
                        QStringLiteral("0123456789abcdef")) &&
                  webDelete(agentWeb, QStringLiteral("agent-branch"),
                            QStringLiteral("fedcba9876543210")) &&
                  webDelete(inWorktree, QStringLiteral("branch"),
                            QStringLiteral("1111111111111111")) &&
                  webDelete(advanced, QStringLiteral("branch"),
                            QStringLiteral("2222222222222222")) &&
                  webDelete(hidden, QStringLiteral("branch"),
                            QStringLiteral("3333333333333333")),
              "the core deletes branches through the executor");
        check(git(core, {QStringLiteral("update-ref"),
                         QStringLiteral("refs/forkmesh/trash/branch/feature/"
                                        "reused-name/4444444444444444"),
                         QStringLiteral("main")}),
              "the core once deleted a branch with the reused name");
        check(commitFile(core, QStringLiteral("README.md"),
                             QByteArrayLiteral("core v2\n"),
                             QStringLiteral("core main advances")) &&
                  git(core, {QStringLiteral("update-ref"),
                             QStringLiteral("refs/forkmesh/trash/branch/feature/"
                                            "never-pushed/5555555555555555"),
                             QStringLiteral("main")}),
              "a trash entry that contains a never-pushed local tip");
        const QString wt = root.filePath(QStringLiteral("trash-wt"));
        check(git(desk, {QStringLiteral("worktree"), QStringLiteral("add"),
                         QStringLiteral("-q"), wt, inWorktree}),
              "a deleted branch is checked out in a worktree");
        check(git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         advanced}) &&
                  commitFile(desk, QStringLiteral("more.txt"),
                             QByteArrayLiteral("more\n"),
                             QStringLiteral("local work after the delete")) &&
                  git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                             QStringLiteral("main")}),
              "the desktop commits on a branch the core deleted");
        const QString advancedTip =
            revParse(desk, QStringLiteral("refs/heads/") + advanced);
        check(git(core, {QStringLiteral("config"), QStringLiteral("--add"),
                         QStringLiteral("uploadpack.hideRefs"),
                         QStringLiteral("refs/forkmesh/")}),
              "the core hides its trash like the served gateway");

        const auto hiddenPass = converge();
        check(hiddenPass.error.isEmpty() && hiddenPass.branchesPruned == 0,
              "hidden trash refs prune nothing");
        check(!revParse(desk, QStringLiteral("refs/heads/") + hidden).isEmpty() &&
                  !revParse(desk, QStringLiteral("refs/heads/") + deletedOnWeb)
                       .isEmpty() &&
                  !revParse(desk, QStringLiteral("refs/heads/") + agentWeb)
                       .isEmpty(),
              "a branch merely absent upstream is kept");

        check(git(core, {QStringLiteral("config"), QStringLiteral("--unset-all"),
                         QStringLiteral("uploadpack.hideRefs")}),
              "the core advertises its trash");
        const auto trashPass = converge();
        check(trashPass.error.isEmpty(), "trash converge reports no error");
        check(trashPass.branchesPruned == 3,
              "only the three covered branches are pruned");
        check(revParse(desk, QStringLiteral("refs/heads/") + deletedOnWeb)
                  .isEmpty(),
              "a branch the core deleted is deleted locally");
        check(revParse(desk, forkmesh::upstream::deletedBranchTombstoneRef(
                                 deletedOnWeb)) == deletedTip,
              "the local tip is kept in the deleted-branch tombstone");
        check(revParse(desk, QStringLiteral("refs/heads/") + agentWeb).isEmpty() &&
                  revParse(desk, forkmesh::upstream::deletedBranchTombstoneRef(
                                     agentWeb)) == agentLocalTip,
              "an agent branch the core trashed past the local tip is deleted");
        check(revParse(desk, QStringLiteral("refs/heads/") + inWorktree) ==
                  revParse(wt, QStringLiteral("HEAD")) &&
                  !revParse(wt, QStringLiteral("HEAD")).isEmpty(),
              "a deleted branch checked out in a worktree is kept");
        check(revParse(desk, QStringLiteral("refs/heads/") + advanced) ==
                  advancedTip,
              "local commits past the core's trash are kept");
        check(revParse(desk, QStringLiteral("refs/heads/") + hidden).isEmpty(),
              "a branch first read with its trash hidden is pruned once visible");
        check(revParse(desk, QStringLiteral("refs/heads/feature/never-pushed")) ==
                  neverPushedTip,
              "a never-pushed branch is kept even when a trash entry contains it");
        check(revParse(desk, QStringLiteral("refs/heads/feature/reused-name")) ==
                  reusedTip,
              "a never-pushed branch reusing a deleted name is kept");

        check(git(core, {QStringLiteral("branch"), deletedOnWeb, deletedTip}),
              "the publisher re-pushes the deleted branch to one core");
        const auto repushed = converge();
        check(repushed.error.isEmpty() &&
                  revParse(desk, QStringLiteral("refs/heads/") + deletedOnWeb)
                      .isEmpty(),
              "a re-pushed copy inside the tombstone is not re-created");

        check(git(core, {QStringLiteral("branch"), QStringLiteral("-q"),
                         QStringLiteral("-D"), deletedOnWeb}) &&
                  git(desk, {QStringLiteral("branch"), deletedOnWeb,
                             forkmesh::upstream::deletedBranchTombstoneRef(
                                 deletedOnWeb)}),
              "the user restores the deleted branch from its tombstone");
        const auto restored = converge();
        check(restored.error.isEmpty() && restored.branchesPruned == 0 &&
                  revParse(desk, QStringLiteral("refs/heads/") + deletedOnWeb) ==
                      deletedTip,
              "a restored branch is not deleted again");
    }

    {
        const QString upstreamPulls = root.filePath(QStringLiteral("pull-upstream"));
        const QString managed = root.filePath(QStringLiteral("pull-managed"));
        QDir().mkpath(upstreamPulls);
        check(git(upstreamPulls, {QStringLiteral("init"), QStringLiteral("-q"),
                                  QStringLiteral("-b"), QStringLiteral("main")}) &&
                  commitFile(upstreamPulls, QStringLiteral("README.md"),
                             QByteArrayLiteral("pulls\n"),
                             QStringLiteral("pull upstream seed")),
              "seed the pull upstream");
        for (const QString &branch : {QStringLiteral("forkmesh/pull/9"),
                                      QStringLiteral("forkmesh/pull/10"),
                                      QStringLiteral("feature/web-deleted")}) {
            check(git(upstreamPulls,
                      {QStringLiteral("checkout"), QStringLiteral("-q"),
                       QStringLiteral("-b"), branch, QStringLiteral("main")}) &&
                      commitFile(upstreamPulls, QStringLiteral("record.txt"),
                                 branch.toUtf8() + '\n', branch) &&
                      git(upstreamPulls,
                          {QStringLiteral("checkout"), QStringLiteral("-q"),
                           QStringLiteral("main")}),
                  "the upstream holds a branch");
        }
        check(git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                QStringLiteral("--branch"), QStringLiteral("main"),
                                QStringLiteral("--single-branch"), upstreamPulls,
                                managed}),
              "clone the managed checkout");
        const auto first = forkmesh::upstream::refreshManagedCheckoutFromUpstream(
            managed, upstreamPulls);
        check(first.error.isEmpty() &&
                  !revParse(managed, QStringLiteral("refs/heads/forkmesh/pull/9"))
                       .isEmpty() &&
                  !revParse(managed, QStringLiteral("refs/heads/forkmesh/pull/10"))
                       .isEmpty(),
              "pull record branches are materialized");
        const QString pullNine =
            revParse(managed, QStringLiteral("refs/heads/forkmesh/pull/9"));
        check(git(managed, {QStringLiteral("update-ref"),
                            forkmesh::upstream::deletedBranchTombstoneRef(
                                QStringLiteral("forkmesh/pull/9")),
                            pullNine}) &&
                  git(managed, {QStringLiteral("branch"), QStringLiteral("-q"),
                                QStringLiteral("-D"),
                                QStringLiteral("forkmesh/pull/9")}) &&
                  git(managed, {QStringLiteral("branch"), QStringLiteral("-q"),
                                QStringLiteral("-D"),
                                QStringLiteral("forkmesh/pull/10")}),
              "the desktop deletes pull 9 with a tombstone and pull 10 without");
        const QString webDeletedTip =
            revParse(managed, QStringLiteral("refs/heads/feature/web-deleted"));
        check(git(upstreamPulls,
                  {QStringLiteral("update-ref"),
                   QStringLiteral("refs/forkmesh/trash/branch/feature/web-deleted/"
                                  "abcdefabcdefabcd"),
                   webDeletedTip}) &&
                  git(upstreamPulls, {QStringLiteral("branch"), QStringLiteral("-q"),
                                      QStringLiteral("-D"),
                                      QStringLiteral("feature/web-deleted")}),
              "the upstream deletes an unmerged branch through the executor");
        const auto second = forkmesh::upstream::refreshManagedCheckoutFromUpstream(
            managed, upstreamPulls);
        check(second.error.isEmpty(), "managed refresh reports no error");
        check(revParse(managed, QStringLiteral("refs/heads/forkmesh/pull/9"))
                  .isEmpty(),
              "a tombstoned pull record branch is not re-created");
        check(revParse(managed, QStringLiteral("refs/heads/forkmesh/pull/10")) ==
                  revParse(upstreamPulls,
                           QStringLiteral("refs/heads/forkmesh/pull/10")),
              "a pull record branch without a tombstone is still force-tracked");
        check(revParse(managed, QStringLiteral("refs/heads/feature/web-deleted"))
                      .isEmpty() &&
                  revParse(managed, forkmesh::upstream::deletedBranchTombstoneRef(
                                        QStringLiteral("feature/web-deleted"))) ==
                      webDeletedTip,
              "an unmerged branch the upstream trashed is pruned with a tombstone");
        check(git(upstreamPulls,
                  {QStringLiteral("checkout"), QStringLiteral("-q"),
                   QStringLiteral("forkmesh/pull/9")}) &&
                  commitFile(upstreamPulls, QStringLiteral("record.txt"),
                             QByteArrayLiteral("pull 9 reopened\n"),
                             QStringLiteral("pull 9 gains a record")) &&
                  git(upstreamPulls,
                      {QStringLiteral("checkout"), QStringLiteral("-q"),
                       QStringLiteral("main")}),
              "the upstream advances pull 9 past the tombstone");
        const auto third = forkmesh::upstream::refreshManagedCheckoutFromUpstream(
            managed, upstreamPulls);
        check(third.error.isEmpty() &&
                  revParse(managed, QStringLiteral("refs/heads/forkmesh/pull/9")) ==
                      revParse(upstreamPulls,
                               QStringLiteral("refs/heads/forkmesh/pull/9")),
              "a pull record that moved past its tombstone comes back");
    }

    {
        const QString oidA = QString(40, QLatin1Char('a'));
        const QString oidB = QString(40, QLatin1Char('b'));
        QHash<QString, QStringList> parsed;
        const QByteArray listing =
            QByteArrayLiteral("{\"ok\":true,\"owner\":\"forkmesh\",\"repo\":"
                              "\"forkmesh\",\"cores\":[\"fmnext-n1\"],"
                              "\"unanswered\":[],\"truncated\":false,"
                              "\"entries\":[{\"branch\":\"feature/x\",\"oid\":\"") +
            oidA.toUtf8() +
            QByteArrayLiteral("\"},{\"branch\":\"feature/x\",\"oid\":\"") +
            oidB.toUtf8() + QByteArrayLiteral("\"}]}");
        check(forkmesh::upstream::parseServedBranchTrash(
                  listing, QStringLiteral("ForkMesh"), QStringLiteral("forkmesh"),
                  &parsed) &&
                  parsed.value(QStringLiteral("feature/x")) ==
                      QStringList({oidA, oidB}),
              "a served trash listing parses");
        const auto rejects = [&](QByteArray body, const char *message) {
            QHash<QString, QStringList> out{{QStringLiteral("x"), {oidA}}};
            check(!forkmesh::upstream::parseServedBranchTrash(
                      body, QStringLiteral("forkmesh"),
                      QStringLiteral("forkmesh"), &out) &&
                      out.isEmpty(),
                  message);
        };
        rejects(QByteArray(listing).replace("\"ok\":true", "\"ok\":false"),
                "a listing that is not ok is refused");
        rejects(QByteArray(listing).replace("\"owner\":\"forkmesh\"",
                                            "\"owner\":\"other\""),
                "a listing for another owner is refused");
        rejects(QByteArray(listing).replace("\"repo\":\"forkmesh\"",
                                            "\"repo\":\"other\""),
                "a listing for another repository is refused");
        rejects(QByteArray(listing).replace("[\"fmnext-n1\"]", "[]"),
                "a listing no core answered is refused");
        rejects(QByteArray(listing).replace("feature/x", "../x"),
                "a listing with a bad branch name is refused");
        rejects(QByteArray(listing).replace(oidA.toUtf8(), "abc"),
                "a listing with a bad object id is refused");
        rejects(QByteArrayLiteral("<html>"), "a non-JSON answer is refused");
        check(forkmesh::upstream::branchTrashRequestCanonical(
                  QStringLiteral(" ForkMesh "), QStringLiteral("forkmesh"),
                  QStringLiteral("1790000000000")) ==
                  QByteArrayLiteral("forkmesh-branch-trash-v1\nforkmesh\n"
                                    "forkmesh\n1790000000000"),
              "the signed request text matches the app");

        const QString core = root.filePath(QStringLiteral("served-core"));
        const QString desk = root.filePath(QStringLiteral("served-desk"));
        QDir().mkpath(core);
        check(git(core, {QStringLiteral("init"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("main")}) &&
                  commitFile(core, QStringLiteral("README.md"),
                             QByteArrayLiteral("served\n"),
                             QStringLiteral("served seed")) &&
                  git(core, {QStringLiteral("config"), QStringLiteral("--add"),
                             QStringLiteral("uploadpack.hideRefs"),
                             QStringLiteral("refs/forkmesh/")}) &&
                  git(temp.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                    core, desk}),
              "seed a core that hides its trash");
        const QString base = revParse(desk, QStringLiteral("main"));
        QHash<QString, QString> tips;
        for (const QString &branch :
             {QStringLiteral("feature/served-delete"),
              QStringLiteral("feature/served-unlisted"),
              QStringLiteral("feature/served-advanced")}) {
            check(git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                             QStringLiteral("-b"), branch, base}) &&
                      commitFile(desk, QStringLiteral("served.txt"),
                                 branch.toUtf8() + '\n', branch) &&
                      git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                                 QStringLiteral("main")}) &&
                      git(desk, {QStringLiteral("push"), QStringLiteral("-q"),
                                 core, branch}),
                  "the desktop pushes a branch to the served core");
            tips.insert(branch, revParse(desk, QStringLiteral("refs/heads/") + branch));
        }
        check(git(desk, {QStringLiteral("branch"),
                         QStringLiteral("feature/served-never-pushed"), base}),
              "a never-pushed branch beside the served ones");
        forkmesh::upstream::MirrorRoute route{core, QProcessEnvironment(), {}, {}};
        check(forkmesh::upstream::convergeSourceCheckoutFromRoutes(desk, {route})
                  .error.isEmpty(),
              "the desktop reads the served core once");
        for (const QString &branch : tips.keys())
            check(git(core, {QStringLiteral("update-ref"),
                             QStringLiteral("refs/forkmesh/trash/branch/") + branch +
                                 QStringLiteral("/0123456789abcdef"),
                             tips.value(branch)}) &&
                      git(core, {QStringLiteral("branch"), QStringLiteral("-q"),
                                 QStringLiteral("-D"), branch}),
                  "the cores delete the branch and hide its trash");
        check(git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("feature/served-advanced")}) &&
                  commitFile(desk, QStringLiteral("more.txt"),
                             QByteArrayLiteral("more\n"),
                             QStringLiteral("local work after the served delete")) &&
                  git(desk, {QStringLiteral("checkout"), QStringLiteral("-q"),
                             QStringLiteral("main")}),
              "the desktop commits on a branch the cores deleted");
        const QString advancedTip =
            revParse(desk, QStringLiteral("refs/heads/feature/served-advanced"));

        const auto unlisted =
            forkmesh::upstream::convergeSourceCheckoutFromRoutes(desk, {route});
        check(unlisted.error.isEmpty() && unlisted.branchesPruned == 0,
              "without the served listing nothing is deleted");

        route.deletedBranches = {
            {QStringLiteral("feature/served-delete"),
             {tips.value(QStringLiteral("feature/served-delete"))}},
            {QStringLiteral("feature/served-advanced"),
             {tips.value(QStringLiteral("feature/served-advanced"))}},
            {QStringLiteral("feature/served-never-pushed"),
             {tips.value(QStringLiteral("feature/served-unlisted"))}},
        };
        const auto served =
            forkmesh::upstream::convergeSourceCheckoutFromRoutes(desk, {route});
        check(served.error.isEmpty() && served.branchesPruned == 1,
              "the served listing prunes exactly the covered branch");
        check(revParse(desk, QStringLiteral("refs/heads/feature/served-delete"))
                      .isEmpty() &&
                  revParse(desk, forkmesh::upstream::deletedBranchTombstoneRef(
                                     QStringLiteral("feature/served-delete"))) ==
                      tips.value(QStringLiteral("feature/served-delete")),
              "a branch the served listing names is deleted with a tombstone");
        check(revParse(desk, QStringLiteral("refs/heads/feature/served-unlisted")) ==
                  tips.value(QStringLiteral("feature/served-unlisted")),
              "a branch the listing does not name is kept");
        check(revParse(desk, QStringLiteral("refs/heads/feature/served-advanced")) ==
                  advancedTip,
              "local commits past the served trash are kept");
        check(revParse(desk,
                       QStringLiteral("refs/heads/feature/served-never-pushed")) ==
                  base,
              "a never-pushed branch is kept against the served listing");
    }

    {
        const QString repo = root.filePath(QStringLiteral("restore-deleted"));
        QDir().mkpath(repo);
        check(git(repo, {QStringLiteral("init"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("main")}) &&
                  commitFile(repo, QStringLiteral("a.txt"),
                             QByteArrayLiteral("a\n"), QStringLiteral("base")),
              "seed the restore repository");
        const QString branch = QStringLiteral("2368-agent-branch");
        const QString tombstone =
            forkmesh::upstream::deletedBranchTombstoneRef(branch);
        check(git(repo, {QStringLiteral("checkout"), QStringLiteral("-q"),
                         QStringLiteral("-b"), branch}) &&
                  commitFile(repo, QStringLiteral("b.txt"),
                             QByteArrayLiteral("b\n"), QStringLiteral("work")) &&
                  git(repo, {QStringLiteral("checkout"), QStringLiteral("-q"),
                             QStringLiteral("main")}),
              "commit agent work on its branch");
        const QString work = revParse(repo, branch);
        check(git(repo, {QStringLiteral("update-ref"), tombstone, work}) &&
                  git(repo, {QStringLiteral("branch"), QStringLiteral("-D"),
                             branch}),
              "delete the agent branch behind a tombstone");
        check(forkmesh::upstream::restoreDeletedBranch(repo, branch) == work,
              "restoring returns the tombstoned tip");
        check(revParse(repo, QStringLiteral("refs/heads/") + branch) == work,
              "the agent branch is back at its deleted tip");
        check(revParse(repo, tombstone).isEmpty(),
              "the tombstone is cleared so the sync keeps the branch");
        check(forkmesh::upstream::restoreDeletedBranch(repo, branch).isEmpty(),
              "a branch that already exists is left alone");
        check(forkmesh::upstream::restoreDeletedBranch(
                  repo, QStringLiteral("never-deleted"))
                  .isEmpty() &&
                  revParse(repo, QStringLiteral("refs/heads/never-deleted"))
                      .isEmpty(),
              "a branch without a tombstone is not invented");
    }

    if (failures == 0)
        std::printf("test_upstream_checkout_sync: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

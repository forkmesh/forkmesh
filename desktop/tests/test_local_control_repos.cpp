#include "LocalControlHosts.h"
#include "LocalControlRepos.h"

#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::localcontrol::repos;
namespace hostctl = forkmesh::localcontrol::hosts;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void testPrefixAndValidation()
{
    check(familyPrefix() == QLatin1String("repos."), "prefix");
    QString owner;
    QString name;
    check(parseRepoSlug(QStringLiteral("alice/app"), &owner, &name), "slug parses");
    check(owner == QLatin1String("alice") && name == QLatin1String("app"), "slug parts");
    check(!parseRepoSlug(QStringLiteral("a/.."), nullptr, nullptr), "dot dot name");
    check(!parseRepoSlug(QStringLiteral("a/b/c"), nullptr, nullptr), "too deep");

    check(validWorkflowPath(QStringLiteral(".forkmesh/deploy.yml")), "yml workflow");
    check(validWorkflowPath(QStringLiteral(".forkmesh/ci.yaml")), "yaml workflow");
    check(!validWorkflowPath(QStringLiteral(".forkmesh/../x.yml")), "escape");
    check(!validWorkflowPath(QStringLiteral("/etc/deploy.yml")), "absolute");
    check(!validWorkflowPath(QStringLiteral(".forkmesh/sub/deploy.yml")), "nested");
    check(!validWorkflowPath(QStringLiteral(".forkmesh/deploy.sh")), "not yaml");

    check(validBranchName(QStringLiteral("main")), "main");
    check(validBranchName(QStringLiteral("feature/x-1")), "slash branch");
    check(!validBranchName(QStringLiteral("-x")), "option-like");
    check(!validBranchName(QStringLiteral("a..b")), "dot dot");
    check(!validBranchName(QStringLiteral("a b")), "space");
    check(!validBranchName(QStringLiteral("x.lock")), "lock");
    check(!validBranchName(QString()), "empty");

    check(branchFromRef(QStringLiteral("refs/heads/main")) == QLatin1String("main"), "heads");
    check(branchFromRef(QStringLiteral("refs/tags/v1")) == QLatin1String("v1"), "tags");

    check(clampLimit(QJsonValue(), 50, 200) == 50, "default limit");
    check(clampLimit(QJsonValue(900), 50, 200) == 200, "limit capped");
    check(clampLimit(QJsonValue(0), 50, 200) == 1, "limit floored");
}

static void testCommits()
{
    const QByteArray out =
        "0123456789abcdef0123456789abcdef01234567\x1f" "Ada\x1f" "1700000000\x1f" "Fix the\tbuild\x1e\n"
        "fedcba9876543210fedcba9876543210fedcba98\x1f" "Bob\x1f" "1700000100\x1f" "Add a | b\x1e\n";
    const QJsonArray commits = parseCommits(out, 10);
    check(commits.size() == 2, "two commits");
    const QJsonObject first = commits.at(0).toObject();
    check(first.value(QStringLiteral("sha")).toString().startsWith(QLatin1String("0123")), "sha");
    check(first.value(QStringLiteral("author")).toString() == QLatin1String("Ada"), "author");
    check(first.value(QStringLiteral("atMs")).toDouble() == 1700000000000.0, "time in ms");
    check(first.value(QStringLiteral("subject")).toString() == QLatin1String("Fix the build"),
          "subject flattened");
    check(parseCommits(out, 1).size() == 1, "commit limit");
    check(parseCommits("garbage", 10).isEmpty(), "garbage ignored");
}

static void testBranches()
{
    int ahead = -1;
    int behind = -1;
    bool gone = true;
    parseTrack(QStringLiteral("[ahead 2, behind 5]"), &ahead, &behind, &gone);
    check(ahead == 2 && behind == 5 && !gone, "ahead and behind");
    parseTrack(QStringLiteral("[gone]"), &ahead, &behind, &gone);
    check(ahead == 0 && behind == 0 && gone, "gone upstream");

    const QByteArray out = "*\x1fmain\x1f" "aaaa\x1f" "1700000000\x1forigin/main\x1f[behind 1]\x1fTip\n"
                           " \x1f" "feature/x\x1f" "bbbb\x1f" "1600000000\x1f\x1f\x1fWork\n";
    const QJsonArray branches = parseBranches(out, 10);
    check(branches.size() == 2, "two branches");
    const QJsonObject main = branches.at(0).toObject();
    check(main.value(QStringLiteral("current")).toBool(), "current branch");
    check(main.value(QStringLiteral("behind")).toInt() == 1, "behind count");
    check(main.value(QStringLiteral("upstream")).toString() == QLatin1String("origin/main"),
          "upstream");
    const QJsonObject feature = branches.at(1).toObject();
    check(!feature.value(QStringLiteral("current")).toBool(), "not current");
    check(feature.value(QStringLiteral("upstream")).toString().isEmpty(), "no upstream");
    check(parseBranches(out, 1).size() == 1, "branch limit");
}

static void testRunsAndSummary()
{
    RunFacts run;
    run.id = 7;
    run.workflowName = QStringLiteral("Deploy");
    run.workflowPath = QStringLiteral(".forkmesh/deploy.yml");
    run.status = QStringLiteral("running");
    run.ref = QStringLiteral("refs/heads/main");
    run.createdAtMs = 200;
    run.stepNames = {QStringLiteral("Build"), QStringLiteral("Ship")};
    run.stepStatuses = {QStringLiteral("success")};
    const QJsonObject row = runRow(run);
    check(row.value(QStringLiteral("branch")).toString() == QLatin1String("main"), "run branch");
    const QJsonArray steps = row.value(QStringLiteral("steps")).toArray();
    check(steps.size() == 2, "steps");
    check(steps.at(1).toObject().value(QStringLiteral("status")).toString() ==
              QLatin1String("pending"),
          "missing step status is pending");
    check(!row.contains(QStringLiteral("pull")), "no pull on a push run");

    RunFacts waiting = run;
    waiting.id = 8;
    waiting.status = QStringLiteral("awaiting-approval");
    waiting.createdAtMs = 300;
    waiting.workflowName = QStringLiteral("Release");
    RunFacts queued = run;
    queued.status = QStringLiteral("queued");
    queued.createdAtMs = 100;
    const QJsonObject summary = actionsSummary({run, waiting, queued});
    check(summary.value(QStringLiteral("running")).toInt() == 1, "running count");
    check(summary.value(QStringLiteral("awaiting")).toInt() == 1, "awaiting count");
    check(summary.value(QStringLiteral("queued")).toInt() == 1, "queued count");
    check(summary.value(QStringLiteral("latestWorkflow")).toString() == QLatin1String("Release"),
          "latest run");
    check(!actionsSummary({}).contains(QStringLiteral("latestStatus")), "no runs, no latest");

    WorkflowFacts workflow;
    workflow.path = QStringLiteral(".forkmesh/ci.yml");
    workflow.valid = true;
    workflow.manual = true;
    const QJsonObject wf = workflowRow(workflow);
    check(wf.value(QStringLiteral("name")).toString() == workflow.path, "name falls back to path");
    check(wf.value(QStringLiteral("manual")).toBool(), "manual");
}

static void testLogSlice()
{
    const QString log = QStringLiteral("0123456789");
    LogSlice whole = logSlice(log, -1, 100);
    check(whole.text == log && whole.from == 0 && whole.next == 10 && !whole.reset, "whole log");
    LogSlice tail = logSlice(log, -1, 4);
    check(tail.text == QLatin1String("6789") && tail.from == 6, "tail capped");
    LogSlice delta = logSlice(log, 7, 100);
    check(delta.text == QLatin1String("789") && !delta.reset, "delta");
    LogSlice rewritten = logSlice(log, 50, 100);
    check(rewritten.reset && rewritten.text == log, "rewritten log resets");
}

static void testActionsAndConfirmations()
{
    check(actions().contains(QStringLiteral("workflow.run")), "workflow run action");
    check(!actionTargetsRun(QStringLiteral("workflow.run")), "workflow run has no run");
    check(actionTargetsRun(QStringLiteral("run.stop")), "stop targets a run");
    check(!actionTargetsRun(QStringLiteral("rm")), "unknown action");
    check(runStatusAllows(QStringLiteral("run.approve"), QStringLiteral("awaiting-approval")),
          "approve waiting");
    check(!runStatusAllows(QStringLiteral("run.approve"), QStringLiteral("running")),
          "approve running refused");
    check(runStatusAllows(QStringLiteral("run.stop"), QStringLiteral("queued")), "stop queued");
    check(!runStatusAllows(QStringLiteral("run.stop"), QStringLiteral("success")),
          "stop finished refused");
    check(runStatusAllows(QStringLiteral("run.rerun"), QStringLiteral("failed")), "rerun failed");
    check(!runStatusAllows(QStringLiteral("run.rerun"), QStringLiteral("running")),
          "rerun running refused");

    Intent run;
    run.action = QStringLiteral("workflow.run");
    run.repo = QStringLiteral("alice/app");
    run.path = QStringLiteral(".forkmesh/deploy.yml");
    run.branch = QStringLiteral("main");
    check(actionSummary(run, QStringLiteral("Deploy")).contains(QLatin1String("\"Deploy\"")),
          "summary names the workflow");
    check(actionSummary(run, QString()).startsWith(QLatin1String("Run .forkmesh/deploy.yml on main")),
          "summary falls back to the path");

    check(actions().contains(QStringLiteral("workflow.disable")), "workflow disable action");
    check(!actionTargetsRun(QStringLiteral("workflow.disable")), "workflow switch has no run");
    check(actionSwitchesWorkflow(QStringLiteral("workflow.enable")), "enable switches");
    check(!actionSwitchesWorkflow(QStringLiteral("workflow.run")), "run does not switch");
    Intent off;
    off.action = QStringLiteral("workflow.disable");
    off.repo = QStringLiteral("alice/app");
    off.path = QStringLiteral(".forkmesh/deploy.yml");
    check(intentSubject(off) == QLatin1String("alice/app:.forkmesh/deploy.yml"),
          "switch subject has no branch");
    check(actionSummary(off, QStringLiteral("Deploy")).startsWith(QLatin1String("Turn off \"Deploy\"")),
          "switch summary");

    ActionConfirmations confirmations;
    const auto issued = confirmations.issue(QStringLiteral("phone"), run, 1000);
    check(!issued.token.isEmpty(), "token issued");
    Intent other;
    check(confirmations.consume(issued.token, QStringLiteral("laptop"), 1001, &other) ==
              hostctl::ConfirmationStore::Result::Mismatch,
          "other device refused");
    check(confirmations.consume(issued.token, QStringLiteral("phone"), 1002, &other) ==
              hostctl::ConfirmationStore::Result::Missing,
          "token spent after a refusal");

    const auto second = confirmations.issue(QStringLiteral("phone"), run, 2000);
    Intent spent;
    check(confirmations.consume(second.token, QStringLiteral("phone"), 2001, &spent) ==
              hostctl::ConfirmationStore::Result::Ok,
          "token accepted");
    check(spent.path == run.path && spent.branch == run.branch && spent.repo == run.repo,
          "intent returned");
    check(confirmations.consume(second.token, QStringLiteral("phone"), 2002, &spent) ==
              hostctl::ConfirmationStore::Result::Missing,
          "token single use");

    const auto late = confirmations.issue(QStringLiteral("phone"), run, 3000);
    check(confirmations.consume(late.token, QStringLiteral("phone"),
                                3000 + hostctl::ConfirmationStore::kLifetimeMs + 1, &spent) ==
              hostctl::ConfirmationStore::Result::Expired,
          "expired token refused");
    check(confirmations.outstanding() == 0, "nothing outstanding");
}

int main()
{
    testPrefixAndValidation();
    testCommits();
    testBranches();
    testRunsAndSummary();
    testLogSlice();
    testActionsAndConfirmations();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control repos tests passed\n";
    return 0;
}

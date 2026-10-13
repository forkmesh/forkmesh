#include "LocalControlAgentOps.h"
#include "LocalControlHosts.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <iostream>
#include <limits>

using namespace forkmesh::localcontrol::agentops;
namespace hostctl = forkmesh::localcontrol::hosts;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject askEvent(const QString &id, const QString &question, double ts)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("_ts"), ts},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("content"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("type"), QStringLiteral("tool_use")},
                          {QStringLiteral("id"), id},
                          {QStringLiteral("name"), QStringLiteral("AskUserQuestion")},
                          {QStringLiteral("input"),
                           QJsonObject{{QStringLiteral("questions"),
                                        QJsonArray{QJsonObject{
                                            {QStringLiteral("question"),
                                             question}}}}}}}}}}}};
}

static void testActions()
{
    check(familyPrefix() == QLatin1String("agentops."), "prefix");
    for (const QString &a : actions())
        check(isKnownAction(a), "every listed action is known");
    check(!isKnownAction(QStringLiteral("destroy")), "unknown action refused");
    check(isDestructiveAction(QStringLiteral("merge")), "merge is destructive");
    check(!isDestructiveAction(QStringLiteral("approve")), "approve is not destructive");
    check(isKnownAction(QStringLiteral("delete")) && isDestructiveAction(QStringLiteral("delete")),
          "delete is known and destructive");
    check(deleteSummary(QStringLiteral("fm/x"), QStringLiteral("a/b")).contains(QStringLiteral("fm/x")),
          "delete summary names the unmerged branch");
    check(!deleteSummary(QString(), QStringLiteral("a/b")).contains(QStringLiteral("branch")),
          "a merged agent's delete summary does not warn about a branch");
    check(deleteSummary(QStringLiteral("fm/x"), QStringLiteral("a/b"), true)
                  .startsWith(QStringLiteral("Stop this agent and delete it")) &&
              !deleteSummary(QStringLiteral("fm/x"), QStringLiteral("a/b")).contains(QStringLiteral("Stop")),
          "a running agent's delete summary says it is stopped first");
    DeleteFacts facts;
    facts.agentGone = true;
    facts.branch = QStringLiteral("agent/9-x");
    facts.branchExisted = true;
    facts.worktree = QStringLiteral("/w/9");
    facts.worktreeExisted = true;
    facts.issue = 40;
    facts.issueFound = true;
    facts.issueStatus = QStringLiteral("closed");
    facts.pull = 8;
    facts.task = QStringLiteral("ab12");
    facts.taskStatus = QStringLiteral("deleted");
    const QJsonArray parts = deletePartsJson(deleteParts(facts));
    const auto partStatus = [&parts](const QString &kind) {
        for (const QJsonValue &value : parts)
            if (value.toObject().value(QStringLiteral("kind")).toString() == kind)
                return value.toObject().value(QStringLiteral("status")).toString();
        return QString();
    };
    check(parts.size() == 6, "a delete reports the agent, branch, worktree, issue, pull request and task");
    check(partStatus(QStringLiteral("agent")) == QLatin1String("deleted") &&
              partStatus(QStringLiteral("branch")) == QLatin1String("deleted") &&
              partStatus(QStringLiteral("work")) == QLatin1String("deleted") &&
              partStatus(QStringLiteral("issue")) == QLatin1String("closed") &&
              partStatus(QStringLiteral("pull")) == QLatin1String("deleted") &&
              partStatus(QStringLiteral("task")) == QLatin1String("deleted"),
          "everything a finished delete removed is reported deleted, the issue closed");
    check(parts.at(1).toObject().value(QStringLiteral("name")).toString() == QLatin1String("agent/9-x"),
          "the branch part names the branch");
    DeleteFacts stuck = facts;
    stuck.agentGone = false;
    stuck.settled = false;
    stuck.branchLeft = true;
    stuck.worktreeExisted = false;
    stuck.issueStatus = QStringLiteral("open");
    stuck.pullMerged = true;
    stuck.taskStatus.clear();
    const QList<DeletePart> pending = deleteParts(stuck);
    check(pending.at(0).status == QLatin1String("pending") && pending.at(1).status == QLatin1String("pending") &&
              pending.at(2).status == QLatin1String("absent") && pending.at(3).status == QLatin1String("pending") &&
              pending.at(4).status == QLatin1String("kept") && pending.at(4).reason == QLatin1String("merged") &&
              pending.at(5).status == QLatin1String("pending"),
          "a delete still running reports what is left as pending and a merged pull request as kept");
    DeleteFacts bare;
    bare.agentGone = true;
    check(deleteParts(bare).size() == 1, "an agent with nothing attached reports only itself");
    const QString outside = outsideDeleteSummary(QStringLiteral("Claude Code"), QStringLiteral("/home/u/src"));
    check(outside.contains(QStringLiteral("Claude Code")) && outside.contains(QStringLiteral("/home/u/src")) &&
              outside.contains(QStringLiteral("outside ForkMesh")),
          "an outside agent's delete summary names the CLI and its folder");
    const QJsonArray files = numstatRows(QStringLiteral("3\t1\tsrc/a.cpp\n-\t-\timg.png\nbad\n"));
    check(files.size() == 2, "numstat rows skip malformed lines");
    check(files.at(0).toObject().value(QStringLiteral("added")).toInt() == 3 &&
              files.at(0).toObject().value(QStringLiteral("removed")).toInt() == 1 &&
              files.at(0).toObject().value(QStringLiteral("path")).toString() == QLatin1String("src/a.cpp"),
          "numstat counts");
    check(files.at(1).toObject().value(QStringLiteral("binary")).toBool(), "binary file marked");
    bool truncated = false;
    check(capDiff(QStringLiteral("ab\ncd\nef\n"), 7, &truncated) == QLatin1String("ab\ncd\n") &&
              truncated,
          "a capped diff ends on a whole line");
    truncated = false;
    check(capDiff(QStringLiteral("ab\n"), 10, &truncated) == QLatin1String("ab\n") && !truncated,
          "a short diff is untouched");
    check(capDiff(QStringLiteral("ab\n"), 0, &truncated).isEmpty() && truncated,
          "no budget left truncates");
    check(untrackedPaths(QStringLiteral(" M a\n?? new.txt\n?? dir/b\n")) ==
              QStringList({QStringLiteral("new.txt"), QStringLiteral("dir/b")}),
          "untracked paths from porcelain status");
    check(diffImageMime(QStringLiteral("docs/Logo.PNG")) == QLatin1String("image/png"),
          "image mime ignores case");
    check(diffImageMime(QStringLiteral("office/public/logo.svg")) == QLatin1String("image/svg+xml"),
          "svg mime");
    check(diffImageMime(QStringLiteral("a.png/readme")).isEmpty(), "a directory dot is no extension");
    check(validDiffImagePath(QStringLiteral("www/img/logo.png")), "a repo image path");
    check(!validDiffImagePath(QStringLiteral("src/main.cpp")), "a source file is no image");
    check(!validDiffImagePath(QStringLiteral("../outside.png")), "parent paths are refused");
    check(!validDiffImagePath(QStringLiteral("a/./b.png")), "dot components are refused");
    check(!validDiffImagePath(QStringLiteral("/etc/x.png")), "absolute paths are refused");
    check(!validDiffImagePath(QStringLiteral("a//b.png")), "empty components are refused");
    check(!validDiffImagePath(QString()), "an empty path is refused");
    check(confirmationHolder(QStringLiteral("K"), 7) == QLatin1String("K#7"),
          "holder binds the device and the connection");
}

static void testAnswers()
{
    check(answerText(false, QStringLiteral("persistent")) == QLatin1String("Deny"),
          "deny ignores the lifetime");
    check(answerText(true, QStringLiteral("once")) == QLatin1String("Allow once"), "once");
    check(answerText(true, QStringLiteral("session")) == QLatin1String("Allow for session"),
          "session");
    check(answerText(true, QStringLiteral("worktree")) == QLatin1String("Allow for worktree"),
          "worktree");
    check(answerText(true, QStringLiteral("persistent")) == QLatin1String("Always allow"),
          "persistent");
    check(validLifetime(QStringLiteral("once")) && !validLifetime(QStringLiteral("forever")),
          "lifetimes are a closed set");
    const QString id(32, QLatin1Char('a'));
    check(answerTarget(id, true, QStringLiteral("once")) !=
              answerTarget(id, true, QStringLiteral("persistent")),
          "a token for once cannot be spent on persistent");
    check(answerTarget(id, false, QStringLiteral("once")) ==
              answerTarget(id, false, QStringLiteral("persistent")),
          "deny targets do not depend on a lifetime");
    check(approvalSummary(true, QStringLiteral("persistent"), QStringLiteral("Run ls"),
                          QStringLiteral("Fix it"))
              .contains(QStringLiteral("revoke")),
          "persistent summary says it is saved");
    check(approvalSummary(false, QStringLiteral("once"), QStringLiteral("Run ls"), {})
              .startsWith(QStringLiteral("Deny")),
          "deny summary");
}

static void testConfirmationsWithHostsStore()
{
    hostctl::ConfirmationStore store;
    const QString holder = confirmationHolder(QStringLiteral("dev"), 1);
    const QString target = answerTarget(QString(32, QLatin1Char('b')), true,
                                        QStringLiteral("once"));
    const QString token = store.issue(holder, QStringLiteral("approve"), target, 1000);
    check(store.consume(token, confirmationHolder(QStringLiteral("dev"), 2),
                        QStringLiteral("approve"), target, 1001) ==
              hostctl::ConfirmationStore::Result::Mismatch,
          "another connection cannot spend the token");
    check(store.consume(token, holder, QStringLiteral("approve"), target, 1002) ==
              hostctl::ConfirmationStore::Result::Missing,
          "a token is spent even by a mismatch");
    const QString again = store.issue(holder, QStringLiteral("approve"), target, 1000);
    check(store.consume(again, holder, QStringLiteral("approve"), target, 1001) ==
              hostctl::ConfirmationStore::Result::Ok,
          "the right holder spends it");
}

static void testApprovalIds()
{
    const QByteArray salt("salt");
    const QString a = approvalId(3, QStringLiteral("req-1"), salt);
    check(validApprovalId(a), "approval id is 32 hex");
    check(a == approvalId(3, QStringLiteral("req-1"), salt), "stable within a run");
    check(a != approvalId(4, QStringLiteral("req-1"), salt), "bound to the session");
    check(a != approvalId(3, QStringLiteral("req-1"), QByteArray("other")),
          "another run of the desktop gives other ids");
    check(!a.contains(QStringLiteral("req")), "says nothing about the request id");
    check(!validApprovalId(QStringLiteral("../etc")), "junk id refused");
    check(approvalsSignature({QStringLiteral("b"), QStringLiteral("a")}) ==
              approvalsSignature({QStringLiteral("a"), QStringLiteral("b")}),
          "signature ignores order");
    check(approvalsSignature({}) != approvalsSignature({QStringLiteral("a")}),
          "signature changes with the set");
}

static void testQuestions()
{
    QList<QJsonObject> events;
    events << askEvent(QStringLiteral("old"), QStringLiteral("Old?"), 10);
    events << QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_ask_answer")},
                          {QStringLiteral("tool_use_id"), QStringLiteral("old")}};
    events << askEvent(QStringLiteral("r1"),
                       QStringLiteral("Run command:\nrm -rf build\n\nReason: clean"), 20);
    events << askEvent(QStringLiteral("r2"), QStringLiteral("Allow network?"), 30);
    events << askEvent(QStringLiteral("r2"), QStringLiteral("dup"), 31);
    const QList<PendingQuestion> found = questionsInEvents(events);
    check(found.size() == 2, "answered and duplicate questions dropped");
    check(found.value(0).requestId == QLatin1String("r2"), "newest first");
    check(found.value(0).question == QLatin1String("dup"), "the newest copy wins");
    check(found.value(0).input.value(QStringLiteral("questions")).toArray().size() == 1,
          "pending question retains its answer choices");
    check(found.value(1).askedAtMs == 20, "asked time from _ts");
    check(commandFromQuestion(found.value(1).question) == QLatin1String("rm -rf build"),
          "command pulled out of the card");
    check(commandFromQuestion(QStringLiteral("Allow?")).isEmpty(), "no command in plain text");
    check(capabilityFromQuestion(found.value(1).question) == QLatin1String("process.execute"),
          "a command card is process.execute");
    check(capabilityFromQuestion(QStringLiteral("Allow network.connect for example.com:443?")) ==
              QLatin1String("network.connect"),
          "the generic card names its capability");
    check(capabilityFromQuestion(QStringLiteral("May I?")).isEmpty(), "unknown capability");
    check(questionsInEvents(events, 1).size() == 1, "scan is bounded");

    ApprovalFacts facts;
    facts.id = approvalId(1, QStringLiteral("r1"), "s");
    facts.sessionId = 1;
    facts.capability = QStringLiteral("process.execute");
    facts.question = QString(5000, QLatin1Char('x'));
    const QJsonObject row = approvalRow(facts);
    check(row.value(QStringLiteral("kind")).toString() == QLatin1String("command"), "kind");
    check(row.value(QStringLiteral("question")).toString().size() <= kMaxQuestionChars,
          "question bounded");
    check(row.value(QStringLiteral("lifetimes")).toArray().size() == 4, "lifetimes offered");
    check(approvalKind(QStringLiteral("network.connect")) == QLatin1String("network"),
          "network kind");
    check(approvalKind(QStringLiteral("weird")) == QLatin1String("other"), "other kind");
}

static void testBulk()
{
    QList<int> ids;
    check(canonicalIds(QJsonArray{3, 1, 3, 2}, &ids), "ids accepted");
    check(ids == QList<int>({1, 2, 3}), "sorted and unique");
    check(bulkTarget(QStringLiteral("stop"), ids) == QLatin1String("stop:1,2,3"), "target");
    check(!canonicalIds(QJsonArray{}, &ids), "empty refused");
    check(!canonicalIds(QJsonArray{1, -2}, &ids), "negative refused");
    check(!canonicalIds(QJsonArray{1, 1.5}, &ids), "fraction refused");
    check(!canonicalIds(QJsonArray{QStringLiteral("1")}, &ids), "string refused");
    QJsonArray many;
    for (int i = 1; i <= kMaxBulkIds + 1; ++i)
        many.append(i);
    check(!canonicalIds(many, &ids), "too many refused");
    check(bulkSummary(QStringLiteral("stop"), 1).contains(QStringLiteral("1 agent ")),
          "singular summary");
}

static void testBin()
{
    check(isKnownAction(QStringLiteral("purge")) && isDestructiveAction(QStringLiteral("purge")),
          "purge is known and destructive");
    QList<int> ids;
    QJsonArray many;
    for (int i = 1; i <= kMaxBulkIds + 1; ++i)
        many.append(i);
    check(canonicalIds(many, &ids, kMaxBinAgents) && ids.size() == kMaxBulkIds + 1,
          "the bin takes more ids than a bulk action");
    QJsonArray tooMany;
    for (int i = 1; i <= kMaxBinAgents + 1; ++i)
        tooMany.append(i);
    check(!canonicalIds(tooMany, &ids, kMaxBinAgents), "the bin caps its ids");
    check(binTitle(7, QStringLiteral("Fix login"), QStringLiteral("prompt")) == QLatin1String("Fix login"),
          "issue title wins");
    check(binTitle(0, QStringLiteral("Fix login"), QStringLiteral("first  line\nsecond")) ==
              QLatin1String("first line"),
          "prompt's first line, simplified");
    check(binTitle(0, {}, QString(200, QLatin1Char('a'))).size() == 90, "long titles are cut");
    const qint64 hour = qint64(60) * 60 * 1000;
    const qint64 now = 1000 * hour;
    BinEntry entry;
    entry.id = 4;
    entry.owner = QStringLiteral("me");
    entry.name = QStringLiteral("app");
    entry.deletedAtMs = now - 3 * hour;
    const QJsonObject row = binRow(entry, 24 * hour, now);
    check(row.value(QStringLiteral("id")).toInt() == 4, "row id");
    check(qint64(row.value(QStringLiteral("removeInMs")).toDouble()) == 21 * hour, "time left");
    entry.deletedAtMs = now - 30 * hour;
    check(binRow(entry, 24 * hour, now).value(QStringLiteral("removeInMs")).toDouble() == 0,
          "overdue rows clamp to zero");
    entry.deletedAtMs = 0;
    check(binRow(entry, 24 * hour, now).value(QStringLiteral("removeInMs")).toDouble() == -1,
          "unknown delete time");
    QList<BinEntry> entries;
    for (int i = 0; i < kMaxBinAgents + 2; ++i)
        entries.append(entry);
    const QJsonObject payload = binPayload(entries, 24, 1, 720, now);
    check(payload.value(QStringLiteral("agents")).toArray().size() == kMaxBinAgents, "rows capped");
    check(payload.value(QStringLiteral("count")).toInt() == kMaxBinAgents + 2, "count is the whole bin");
    check(payload.value(QStringLiteral("truncated")).toBool(), "truncated flagged");
    check(payload.value(QStringLiteral("retentionHours")).toInt() == 24 &&
              payload.value(QStringLiteral("maxHours")).toInt() == 720,
          "retention range");
    check(purgeSummary(1).contains(QStringLiteral("1 deleted agent ")) &&
              purgeSummary(3).contains(QStringLiteral("3 deleted agents")),
          "purge summary counts");
}

static void testWorktrees()
{
    const QString porcelain = QStringLiteral(
        "worktree /home/me/src/app\nHEAD 0123456789abcdef0123\nbranch refs/heads/main\n\n"
        "worktree /home/me/src/app/.worktrees/agent-7\nHEAD fedcba9876543210\n"
        "branch refs/heads/agent/fix-7\nlocked reason\n\n"
        "worktree /tmp/detached\nHEAD 1111\ndetached\nprunable gitdir file points nowhere\n");
    const QList<WorktreeEntry> list = parseWorktreePorcelain(porcelain);
    check(list.size() == 3, "three worktrees");
    check(list.value(1).branch == QLatin1String("agent/fix-7"), "branch without refs/heads");
    check(list.value(1).locked, "locked");
    check(list.value(2).detached && list.value(2).prunable, "detached and prunable");
    check(parseWorktreePorcelain(porcelain, 1).size() == 1, "bounded");
    const QJsonObject row = worktreeRow(list.value(1), false, 5, 3, 7);
    check(row.value(QStringLiteral("name")).toString() == QLatin1String("agent-7"),
          "folder name only");
    check(!QString::fromUtf8(QJsonDocument(row).toJson()).contains(QStringLiteral("/home")),
          "no path leaves the desktop");
    check(row.value(QStringLiteral("sessionId")).toInt() == 7, "session linked");
    check(row.value(QStringLiteral("head")).toString().size() <= 12, "short head");
    int behind = -1;
    int ahead = -1;
    check(parseLeftRight(QStringLiteral("3\t5\n"), &behind, &ahead) && behind == 3 &&
              ahead == 5,
          "left-right counts");
    check(!parseLeftRight(QStringLiteral("x"), &behind, &ahead), "junk counts refused");
}

static void testUsage()
{
    UsageProvider claude;
    claude.id = QStringLiteral("claude-code");
    claude.label = QStringLiteral("Claude Code");
    claude.windows << UsageWindow{QStringLiteral("5h"), QStringLiteral("5-hour"), 42,
                                  5000, false};
    claude.windows << UsageWindow{QStringLiteral("weekly"), QStringLiteral("Weekly"), 100,
                                  500, true};
    claude.windows << UsageWindow{QStringLiteral("fable"), QStringLiteral("Fable"), -1, 0,
                                  false};
    claude.accounts << UsageAccount{QStringLiteral("default"), QStringLiteral("Work"), true,
                                    true, QStringLiteral("me@example.com")};
    claude.creditsEnabled = true;
    claude.creditsUsed = 3.5;
    claude.creditsLimit = 20;
    claude.creditsCurrency = QStringLiteral("USD");
    UsageProvider codex;
    codex.id = QStringLiteral("codex");
    codex.label = QStringLiteral("Codex");
    codex.windows = codexWindows(QJsonArray{
        QJsonObject{{QStringLiteral("label"), QStringLiteral("5-hour")},
                    {QStringLiteral("usedPercent"), 99},
                    {QStringLiteral("resetAt"), 9000.0}},
        QJsonObject{{QStringLiteral("usedPercent"), 1}}});
    check(codex.windows.size() == 1, "codex rows without a label dropped");
    const QJsonObject summary = usageSummary(
        {claude, codex}, 12.25, std::numeric_limits<double>::quiet_NaN(), 1000);
    const QJsonArray providers = summary.value(QStringLiteral("providers")).toArray();
    check(providers.size() == 2, "two providers");
    const QJsonArray cw = providers.at(0).toObject().value(QStringLiteral("windows")).toArray();
    check(cw.at(0).toObject().value(QStringLiteral("state")).toString() == QLatin1String("ok"),
          "42% is ok");
    check(cw.at(0).toObject().value(QStringLiteral("resetAtMs")).toDouble() == 5000,
          "future reset kept");
    check(cw.at(1).toObject().value(QStringLiteral("usedPercent")).toInt() == 0,
          "a passed reset reads as full again");
    check(!cw.at(1).toObject().contains(QStringLiteral("resetAtMs")), "passed reset dropped");
    check(cw.at(2).toObject().value(QStringLiteral("state")).toString() ==
              QLatin1String("unknown"),
          "unknown reading");
    const QJsonArray xw = providers.at(1).toObject().value(QStringLiteral("windows")).toArray();
    check(xw.at(0).toObject().value(QStringLiteral("state")).toString() ==
              QLatin1String("exhausted"),
          "99% is exhausted");
    check(providers.at(0).toObject().value(QStringLiteral("credits")).toObject()
                  .value(QStringLiteral("limit")).toDouble() == 20,
          "credits carried");
    const QJsonObject spend = summary.value(QStringLiteral("spend")).toObject();
    check(spend.value(QStringLiteral("claudeUsd")).toDouble() == 12.25, "claude spend");
    check(!spend.contains(QStringLiteral("openAiUsd")), "unknown spend omitted");
    check(providers.at(0).toObject().value(QStringLiteral("accounts")).toArray().size() == 1,
          "accounts listed");
    const QJsonObject account = providers.at(0)
                                    .toObject()
                                    .value(QStringLiteral("accounts"))
                                    .toArray()
                                    .at(0)
                                    .toObject();
    check(account.value(QStringLiteral("signedIn")).toBool() &&
              account.value(QStringLiteral("identity")).toString() ==
                  QLatin1String("me@example.com"),
          "an account carries its sign-in state and identity");
}

static void testGrants()
{
    const QJsonArray grants{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("0b3f5a2e-1c4d-4e5f-9a8b-7c6d5e4f3a2b")},
                    {QStringLiteral("capability"), QStringLiteral("file.write")},
                    {QStringLiteral("resource"), QStringLiteral("/home/me/secret/notes.txt")},
                    {QStringLiteral("decision"), QStringLiteral("allow")},
                    {QStringLiteral("lifetime"), QStringLiteral("persistent")},
                    {QStringLiteral("worktreePath"), QStringLiteral("/home/me/wt/agent-3")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("not-a-uuid")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("1b3f5a2e-1c4d-4e5f-9a8b-7c6d5e4f3a2b")},
                    {QStringLiteral("capability"), QStringLiteral("process.execute")},
                    {QStringLiteral("resource"), QStringLiteral("command:sha256:abcd")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("2b3f5a2e-1c4d-4e5f-9a8b-7c6d5e4f3a2b")},
                    {QStringLiteral("capability"), QStringLiteral("file.read")},
                    {QStringLiteral("resource"), QStringLiteral("C:\\Users\\me\\a.txt")}},
    };
    const QJsonArray rows = grantRows(grants);
    check(rows.size() == 3, "a grant with a bad id is dropped");
    check(rows.at(0).toObject().value(QStringLiteral("resource")).toString() ==
              QLatin1String("notes.txt"),
          "file grant shows only the file name");
    check(rows.at(0).toObject().value(QStringLiteral("worktree")).toString() ==
              QLatin1String("agent-3"),
          "worktree folder name only");
    check(rows.at(1).toObject().value(QStringLiteral("resource")).toString() ==
              QLatin1String("command:sha256:abcd"),
          "non-path resource kept");
    check(rows.at(2).toObject().value(QStringLiteral("resource")).toString() ==
              QLatin1String("a.txt"),
          "windows path shortened");
    check(grantRows(grants, 1).size() == 1, "bounded");
    check(revokeSummary(QStringLiteral("file.write"), QStringLiteral("/a/b/c.txt"))
              .contains(QStringLiteral("c.txt")),
          "revoke summary names the file only");
    check(!revokeSummary(QStringLiteral("file.write"), QStringLiteral("/a/b/c.txt"))
               .contains(QStringLiteral("/a/b")),
          "revoke summary hides the path");
}

static void testApprovalPings()
{
    const QByteArray salt = newApprovalSalt();
    check(salt.size() == kApprovalSaltBytes, "salt size");
    check(approvalSaltFromStored(QString::fromLatin1(salt.toHex())) == salt, "salt round trip");
    check(approvalSaltFromStored(QStringLiteral("zz")).isEmpty(), "bad stored salt refused");
    check(approvalSaltFromStored(QString(64, QLatin1Char('A'))).isEmpty(), "upper hex refused");
    check(approvalId(4, QStringLiteral("toolu_1"), approvalSaltFromStored(
                                                     QString::fromLatin1(salt.toHex()))) ==
              approvalId(4, QStringLiteral("toolu_1"), salt),
          "a stored salt keeps approval ids stable across a relaunch");

    ApprovalFacts facts;
    facts.id = approvalId(12, QStringLiteral("toolu_9"), salt);
    facts.sessionId = 12;
    facts.title = QStringLiteral("Fix the build");
    facts.repo = QStringLiteral("alice/app");
    facts.provider = QStringLiteral("claude-code");
    facts.question = QStringLiteral("Run command:\ncurl -H 'Authorization: Bearer sk-secret' x");
    facts.capability = capabilityFromQuestion(facts.question);
    const QJsonObject row = approvalRow(facts);
    const ApprovalPing ping = approvalPingFor(row);
    check(ping.sessionId == QLatin1String("12"), "ping session id");
    check(ping.approvalId == facts.id, "ping carries the agentops.approvals id");
    check(ping.agent == QLatin1String("Claude Code"), "agent label");
    check(ping.summary == QLatin1String("Wants to run a command in alice/app"), "summary");
    check(!ping.summary.contains(QStringLiteral("sk-secret")), "summary never carries the command");
    check(ping.summary.size() <= kMaxPingSummaryChars, "summary bounded");

    check(approvalPingSummary(QStringLiteral("file.write"), QStringLiteral("a/b")) ==
              QLatin1String("Wants to write a file in a/b"),
          "file write summary");
    check(approvalPingSummary(QStringLiteral("secret.read"), QStringLiteral("/home/u/repo")) ==
              QLatin1String("Wants to read a secret"),
          "a path is never a repo");
    check(approvalPingSummary(QString(), QStringLiteral("a/b\nc")) ==
              QLatin1String("Wants your approval"),
          "odd repo dropped");
    check(approvalPingAgent(QStringLiteral("codex")) == QLatin1String("Codex"), "codex");
    check(approvalPingAgent(QString(300, QLatin1Char('x'))).size() == kMaxPingAgentChars,
          "agent bounded");
    check(approvalPingAgent(QStringLiteral("local\nllama")) == QLatin1String("local llama"),
          "agent flattened");

    check(approvalPingProof() == QLatin1String("forkmesh-account-agent-approval-ping-v1"),
          "proof matches the server");
    check(approvalPingPath() == QLatin1String("/api/notifications/agent-approval"), "path");
    const QString canonical = approvalPingCanonical(QStringLiteral("alice"), ping,
                                                    QStringLiteral("1700000000000"));
    check(canonical == QStringLiteral("forkmesh-account-agent-approval-ping-v1\nalice\n12\n") +
                           facts.id +
                           QStringLiteral("\nClaude Code\nWants to run a command in alice/app\n"
                                          "1700000000000"),
          "canonical layout matches agent_approval_canonical");
    check(canonical == approvalPingProof() + QStringLiteral("\nalice\n") +
                           approvalPingResource(ping) + QStringLiteral("\n1700000000000"),
          "signOrgTaskProof layout");
    const QJsonObject body = approvalPingBody(ping);
    check(body.size() == 4, "body has four fields");
    check(body.value(QStringLiteral("approvalId")).toString() == facts.id, "body approval");
    check(body.value(QStringLiteral("sessionId")).toString() == QLatin1String("12"), "body session");

    check(validPingField(QStringLiteral("12")), "session id valid");
    check(!validPingField(QString()), "empty invalid");
    check(!validPingField(QString(65, QLatin1Char('a'))), "long invalid");

    check(!approvalPingRetryable(200, false), "ok is final");
    check(!approvalPingRetryable(400, false), "bad request is final");
    check(!approvalPingRetryable(401, false), "unauthorized is final");
    check(approvalPingRetryable(503, false), "server error retried");
    check(approvalPingRetryable(429, false), "rate limit retried");
    check(approvalPingRetryable(0, true), "network error retried");

    ApprovalFacts other = facts;
    other.id = approvalId(12, QStringLiteral("toolu_10"), salt);
    const QJsonArray rows{row, approvalRow(other), row};
    ApprovalPingLedger ledger;
    QList<ApprovalPing> unsent = ledger.unsent(rows);
    check(unsent.size() == 2, "each approval once per pass");
    check(ledger.add(facts.id), "first add");
    check(!ledger.add(facts.id), "never twice");
    unsent = ledger.unsent(rows);
    check(unsent.size() == 1 && unsent.first().approvalId == other.id, "sent one skipped");
    ledger.remove(facts.id);
    check(ledger.unsent(rows).size() == 2, "a retryable failure sends again");
    check(!ledger.add(QStringLiteral("nope")), "invalid id refused");

    ApprovalPingLedger restored(QStringList{facts.id, QStringLiteral("junk"), facts.id});
    check(restored.ids() == QStringList{facts.id}, "stored ledger cleaned");
    check(restored.unsent(rows).size() == 1, "stored ledger survives a relaunch");

    ApprovalPingLedger small({}, 2);
    small.add(approvalId(1, QStringLiteral("a"), salt));
    small.add(approvalId(2, QStringLiteral("b"), salt));
    small.add(approvalId(3, QStringLiteral("c"), salt));
    check(small.ids().size() == 2, "ledger bounded");
    check(!small.contains(approvalId(1, QStringLiteral("a"), salt)), "oldest dropped");

    QJsonObject bad = row;
    bad.insert(QStringLiteral("sessionId"), 0);
    check(ApprovalPingLedger().unsent(QJsonArray{bad}).isEmpty(), "no session, no ping");
}

static void testMergeStats()
{
    const QString log = QStringLiteral(
        "\x1e" "aaa\x1f" "Merge branch 'fm/one' into main\n\n"
        " 3 files changed, 40 insertions(+), 2 deletions(-)\n"
        "\x1e" "bbb\x1f" "Merge fm/two\n\n 1 file changed, 5 deletions(-)\n"
        "\x1e" "ccc\x1f" "Merge empty\n");
    const QList<MergeShortstat> merges = parseMergeShortstatLog(log);
    check(merges.size() == 3, "every merge record parsed");
    check(merges.at(0).sha == QLatin1String("aaa") &&
              merges.at(0).subject == QLatin1String("Merge branch 'fm/one' into main") &&
              merges.at(0).files == 3 && merges.at(0).insertions == 40 &&
              merges.at(0).deletions == 2,
          "shortstat counts");
    check(merges.at(1).files == 1 && merges.at(1).insertions == 0 &&
              merges.at(1).deletions == 5,
          "missing insertions read as zero");
    check(merges.at(2).files == 0, "a merge with no churn");

    MergeStats stats;
    stats.base = QStringLiteral("main");
    stats.velocity = {{QStringLiteral("1h"), QStringLiteral("1 hour"), 3, 1, 0.123, {}},
                      {QStringLiteral("7d"), QStringLiteral("7 days"), 4, 4, 1.0,
                       {0, 1, 0, 0, 2, 0, 1}}};
    QJsonObject json = mergeStatsJson(stats);
    check(!json.contains(QStringLiteral("commit")) && !json.contains(QStringLiteral("commits")),
          "unknown landing omits commit fields");
    const QJsonObject hour = json.value(QStringLiteral("velocity")).toArray().at(0).toObject();
    check(hour.value(QStringLiteral("top")).toInt() == 3, "top is never below the count");
    check(hour.value(QStringLiteral("avg")).toDouble() == 0.1, "avg rounded to one place");
    check(!hour.contains(QStringLiteral("series")), "no buckets omits the series");
    const QJsonArray week = json.value(QStringLiteral("velocity"))
                                .toArray()
                                .at(1)
                                .toObject()
                                .value(QStringLiteral("series"))
                                .toArray();
    check(week.size() == 7 && week.at(4).toInt() == 2 && week.at(6).toInt() == 1,
          "buckets oldest first");
    stats.commit = QStringLiteral("aaa");
    stats.commits = 4;
    stats.files = 3;
    json = mergeStatsJson(stats);
    check(json.value(QStringLiteral("commits")).toInt() == 4 &&
              json.value(QStringLiteral("commit")).toString() == QLatin1String("aaa") &&
              json.value(QStringLiteral("files")).toInt() == 3 &&
              json.value(QStringLiteral("base")).toString() == QLatin1String("main"),
          "landed merge fields");
}

int main()
{
    testActions();
    testMergeStats();
    testAnswers();
    testConfirmationsWithHostsStore();
    testApprovalIds();
    testQuestions();
    testBulk();
    testBin();
    testWorktrees();
    testUsage();
    testGrants();
    testApprovalPings();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control agentops: ok\n";
    return 0;
}

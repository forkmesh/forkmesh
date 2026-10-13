// Cloud agent sessions in the Agents list (adhoc #259): the relay's
// agent-sessions payload becomes list rows, the run's history becomes a
// transcript, and the canonical the account key signs matches the relay's
// (app/src/route_handlers.py, _owner_agent_signed_session).
#include "AgentCloudSessions.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::agents;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject payload()
{
    return QJsonDocument::fromJson(R"json({
      "ok": true,
      "sessions": [
        {"id": "s-run", "provider": "claude-code", "status": "running",
         "targetNode": "Mirror-EWR", "title": "Fix the login page",
         "createdBy": "alice", "createdAt": 1000, "updatedAt": 5000,
         "localAgentId": 7, "issueNumber": 12, "requestedModel": "sonnet",
         "createPr": true,
         "history": [
           {"role": "user", "author": "alice", "text": "Fix the login page",
            "at": 1000, "state": "queued"},
           {"role": "assistant", "author": "mirror-ewr", "text": "On it.",
            "at": 2000},
           {"role": "assistant", "text": "   "}
         ],
         "agentInfo": {"branchName": "agent/web-7-fix-the-login-page",
                       "baseBranch": "main", "prNumber": 31, "model": "opus",
                       "totalTokens": 420, "costUsd": 0.25,
                       "startedAt": 1500, "durationMs": 900},
         "diagnostic": {"code": "running", "level": "active",
                        "message": "The mirror is running this agent session."}},
        {"id": "s-mine", "provider": "codex", "status": "completed",
         "targetNode": "desk", "title": "Runs on this desktop"},
        {"id": "s-wait", "provider": "codex", "status": "security_pending",
         "targetNode": "mirror-ams", "title": "",
         "history": [{"role": "user", "text": "Bump the deps\nand test",
                      "at": 10}],
         "diagnostic": {"level": "attention", "message": "Not claimed."}},
        {"id": "s-bad", "provider": "claude-code", "status": "rejected",
         "targetNode": "mirror-ams", "localAgentId": 3,
         "history": [{"role": "assistant",
                      "text": "API Error: 400 too old\n\nClaude Code exited with status 1\n"}],
         "agentInfo": {"lastError": "Claude Code exited with status 1"},
         "diagnostic": {"level": "attention",
                        "message": "Claude Code exited with status 1"}},
        {"id": "", "status": "running"}
      ]})json").object();
}

int main(int argc, char **argv)
{
    using forkmesh::agents::cloudAgentTaskId;
    check(cloudAgentTaskId(QStringLiteral("task:7e8452f4e85b599d4c27786ff3b0226c"),
                           QString()) ==
              QStringLiteral("7e8452f4e85b599d4c27786ff3b0226c"),
          "a relay taskKey names the task");
    check(cloudAgentTaskId(QString(),
                           QStringLiteral("[task:6B65B62111D408C402D4CD3B00D00666] Fix")) ==
              QStringLiteral("6b65b62111d408c402d4cd3b00d00666"),
          "a [task:id] prompt prefix names the task");
    check(cloudAgentTaskId(QString(), QStringLiteral("No tag here")).isEmpty(),
          "a run without a task has no task link");
    {
        forkmesh::agents::CloudAgentSession linked;
        linked.sessionId = QStringLiteral("s1");
        linked.owner = QStringLiteral("forkmesh");
        linked.name = QStringLiteral("forkmesh");
        linked.taskId = QStringLiteral("7e8452f4e85b599d4c27786ff3b0226c");
        check(forkmesh::agents::cloudAgentListEntry(linked, -5).orgTaskId ==
                  linked.taskId,
              "a cloud row links back to its task like a local run");
    }

    using forkmesh::agents::cloudAgentDisplayTitle;
    using forkmesh::agents::cloudAgentShortTitle;
    check(cloudAgentDisplayTitle(QStringLiteral(
              "[task:29bd3db4f33b382bd7a9a3ac6a1a42ee] [Review and prune] Review x")) ==
              QStringLiteral("[Review and prune] Review x"),
          "the [task:id] tag is dropped from a cloud run's title");
    check(cloudAgentDisplayTitle(QStringLiteral("  Fix   the  login ")) ==
              QStringLiteral("Fix the login"),
          "a title without a tag is only tidied");
    check(cloudAgentDisplayTitle(QStringLiteral("[task:xyz] keep")) ==
              QStringLiteral("[task:xyz] keep"),
          "only a hex task id counts as the tag");
    check(cloudAgentShortTitle(QStringLiteral("short"), 60) ==
              QStringLiteral("short"),
          "a short title is left whole");
    const QString cut = cloudAgentShortTitle(
        QStringLiteral("Review and prune the release branch that shipped host assets"), 30);
    check(cut.endsWith(QString::fromUtf8("\xE2\x80\xA6")) && cut.size() <= 31 &&
              !cut.contains(QStringLiteral(" \xE2")),
          "a long title is cut at a word boundary with an ellipsis");

    QCoreApplication app(argc, argv);

    {
        check(cloudAgentAccountFinishedBefore(QJsonObject{
                  {QStringLiteral("finishedBefore"), 4200.0}}) == 4200,
              "the account list's finished cut-off is read");
        check(cloudAgentAccountFinishedBefore(QJsonObject{}) == 0,
              "an uncut account list has no cut-off");
        CloudAgentSession old;
        old.status = QStringLiteral("completed");
        old.updatedAtMs = 4000;
        check(cloudAgentBeyondListedWindow(old, 4200),
              "a finished run older than the cut-off stays in the list");
        check(!cloudAgentBeyondListedWindow(old, 0),
              "without a cut-off an unlisted finished run was deleted");
        CloudAgentSession newer = old;
        newer.updatedAtMs = 5000;
        check(!cloudAgentBeyondListedWindow(newer, 4200),
              "an unlisted finished run newer than the cut-off was deleted");
        CloudAgentSession live = old;
        live.status = QStringLiteral("queued");
        check(!cloudAgentBeyondListedWindow(live, 4200),
              "an unlisted active run was deleted, whatever its age");
    }

    {
        CloudAgentSession pool;
        pool.sessionId = QStringLiteral("a");
        pool.owner = QStringLiteral("fmnext-n1");
        pool.name = QStringLiteral("forkmesh");
        CloudAgentSession own = pool;
        own.sessionId = QStringLiteral("b");
        own.owner = QStringLiteral("jett");
        own.name = QStringLiteral("notes");
        CloudAgentSession twice = pool;
        twice.sessionId = QStringLiteral("c");
        twice.name = QStringLiteral("shared");
        QList<CloudAgentSession> filed{pool, own, twice};
        const QList<QPair<QString, QString>> local{
            {QStringLiteral("forkmesh"), QStringLiteral("forkmesh")},
            {QStringLiteral("jett"), QStringLiteral("notes")},
            {QStringLiteral("jett"), QStringLiteral("shared")},
            {QStringLiteral("forkmesh"), QStringLiteral("shared")},
        };
        fileCloudAgentSessionsUnderLocalRepos(filed, local);
        check(filed.at(0).owner == QStringLiteral("forkmesh"),
              "a node-owned run is filed under the one local repository of "
              "that name");
        check(filed.at(0).relayRepoOwner() == QStringLiteral("fmnext-n1"),
              "a filed run still addresses the relay under the owner the "
              "relay keeps it under");
        check(filed.at(1).owner == QStringLiteral("jett") &&
                  filed.at(1).relayRepoOwner() == QStringLiteral("jett"),
              "a run whose owner has a local copy keeps its owner");
        check(filed.at(2).owner == QStringLiteral("fmnext-n1"),
              "two local repositories of the name leave the run where it is");
        fileCloudAgentSessionsUnderLocalRepos(filed, local);
        check(filed.at(0).owner == QStringLiteral("forkmesh") &&
                  filed.at(0).relayRepoOwner() == QStringLiteral("fmnext-n1"),
              "filing twice never loses the relay's owner");
        fileCloudAgentSessionsUnderLocalRepos(filed, {});
        check(filed.at(0).owner == QStringLiteral("fmnext-n1"),
              "a run whose local copy is gone goes back under the relay's "
              "owner");
    }

    const QList<CloudAgentSession> sessions =
        parseCloudAgentSessions(payload(), "alice", "site", "Desk");
    check(sessions.size() == 4, "only rows without an id are left out");
    if (sessions.size() != 4)
        return 1;
    // A run queued for this desktop is kept and marked: nothing starts it
    // while ForkMesh is closed, so the list is where a person finds it
    // waiting. Which of them is already running here is the window's
    // question, answered from its own sessions, not from the node name.
    check(!sessions.at(0).targetsThisNode && sessions.at(1).targetsThisNode,
          "a run aimed at this desktop is marked, not dropped");

    const CloudAgentSession &run = sessions.at(0);
    check(run.sessionId == "s-run" && run.owner == "alice" && run.name == "site",
          "a run is addressed to the repository it was fetched for");
    check(run.key() == "alice/site/s-run", "the key names repository and run");
    check(run.targetNode == "Mirror-EWR", "the claiming node is kept as sent");
    check(run.branchName == "agent/web-7-fix-the-login-page" &&
              run.prNumber == 31 && run.baseBranch == "main",
          "branch and pull request come from the node's own report");
    check(run.model == "opus", "the node's model wins over the requested one");
    check(run.history.size() == 2, "blank turns are dropped");
    check(run.promptable(), "a started, running run takes a follow-up");
    check(!run.needsAttention && !run.diagnostic.isEmpty(),
          "the relay's diagnostic is carried");

    const CloudAgentSession &waiting = sessions.at(2);
    check(!waiting.promptable(), "a run no node started takes no follow-up");
    check(waiting.needsAttention, "attention level is read off the diagnostic");
    check(!sessions.at(3).promptable(),
          "a run the preflight rejected takes no follow-up");
    check(sessions.at(3).history.size() == 1 &&
              sessions.at(3).history.at(0).text == "API Error: 400 too old",
          "an older node's error trailer is not repeated in the transcript");

    // What "Continue on this desktop" is offered for: a run still waiting for
    // a node, wherever it was queued. Once a node has started one, its
    // conversation is there and a follow-up is the way to reach it.
    check(waiting.claimable(),
          "a run waiting for a node can be continued on this desktop");
    check(!run.claimable(), "a run a node started is not taken over");
    check(!sessions.at(3).claimable(),
          "a run the preflight rejected is not taken over");
    check(!sessions.at(1).claimable(),
          "a run already completed somewhere is not taken over");

    check(cloudAgentLocalStatus("running") == AgentStatus::Running &&
              cloudAgentLocalStatus("completed") == AgentStatus::Success &&
              cloudAgentLocalStatus("failed") == AgentStatus::Failed &&
              cloudAgentLocalStatus("rejected") == AgentStatus::Failed &&
              cloudAgentLocalStatus("queued") == AgentStatus::Queued &&
              cloudAgentLocalStatus("security_pending") == AgentStatus::Queued &&
              cloudAgentLocalStatus("something-new") == AgentStatus::Queued,
          "relay statuses map onto the list's");

    const AgentSession row = cloudAgentListEntry(run, -1000000);
    check(row.cloud && row.cloudSessionId == "s-run" &&
              row.cloudNode == "Mirror-EWR" && !row.cloudHere,
          "the row is marked cloud and names its node");
    check(cloudAgentListEntry(sessions.at(1), -1000003).cloudHere,
          "a row queued for this desktop says so, for the hover card");
    check(row.id == -1000000 && row.owner == "alice" && row.name == "site",
          "the row carries the synthetic id and repository");
    check(row.status == AgentStatus::Running && row.issueNumber == 12 &&
              row.prNumber == 31 && row.totalTokens == 420 && row.createPr,
          "status, issue, pull request and usage reach the row");
    check(row.issueTitle == "Fix the login page" &&
              row.prompt == "Fix the login page",
          "title and first prompt reach the row");
    check(row.finishedAtMs == 5000,
          "the relay's last update dates a row the node has not finished");
    const AgentSession untitled = cloudAgentListEntry(waiting, -1000001);
    check(untitled.issueTitle == "Bump the deps",
          "an untitled run is named after the first line of its prompt");
    check(cloudAgentListEntry(sessions.at(3), -1000002).issueTitle ==
              "Cloud agent",
          "a run with nothing to name it still has a title");

    // A cloud row is the relay's record; the store never writes it.
    QTemporaryDir tmp;
    AgentStore store(tmp.path());
    check(!store.saveSession(row), "a cloud row is never saved to the store");
    check(store.loadAllSessions().isEmpty(), "and nothing landed on disk");

    // The durable link the list uses to show a taken-over run once: a local
    // session remembers which website run it is, and goes on remembering it
    // after the relay lease it started from has been handed back.
    AgentSession local;
    local.id = 4;
    local.owner = "alice";
    local.name = "site";
    local.webSessionId = "s-run";
    check(store.saveSession(local), "a local session is saved");
    AgentSession legacy;
    legacy.id = 5;
    legacy.owner = "alice";
    legacy.name = "site";
    legacy.orgAgentJob = QJsonObject{{"sessionId", "s-wait"}};
    check(store.saveSession(legacy),
          "a session holding only its lease is saved");
    QHash<int, QString> links;
    for (const AgentSession &loaded : store.loadAllSessions())
        links.insert(loaded.id, loaded.webSessionId);
    check(links.value(4) == "s-run", "the web session id survives a reload");
    check(links.value(5) == "s-wait",
          "a record written before the field reads its link off the lease");

    AgentSession external;
    external.id = -1005;
    external.owner = "alice";
    external.name = "site";
    external.branchName = "4-shared-branch";
    const QString ghostDir =
        tmp.path() + "/sessions/" + external.repoKey() + "/-1005";
    check(!store.saveSession(external),
          "an external Claude Code row is never saved to the store");
    check(!QDir(ghostDir).exists(),
          "and no directory is made for it");
    QDir().mkpath(ghostDir);
    QFile ghost(ghostDir + "/meta.json");
    QJsonObject ghostMeta = local.toJson();
    ghostMeta.insert("id", -1005);
    ghostMeta.insert("branchName", "4-shared-branch");
    check(ghost.open(QIODevice::WriteOnly) &&
              ghost.write(QJsonDocument(ghostMeta).toJson()) > 0,
          "write a ghost record left by an older build");
    ghost.close();
    bool ghostLoaded = false;
    for (const AgentSession &loaded : store.loadAllSessions())
        ghostLoaded = ghostLoaded || loaded.id <= 0;
    check(!ghostLoaded, "a stored record with a non-positive id is not loaded");

    const QList<QJsonObject> events = cloudAgentTranscriptEvents(run);
    check(events.size() == 2, "one transcript event per turn");
    if (events.size() == 2) {
        check(events.at(0).value("type").toString() == "user" &&
                  events.at(0).value("message").toObject().value("content")
                          .toString() == "Fix the login page" &&
                  events.at(0).value("_ts").toDouble() == 1000,
              "a user turn is a dated user event");
        const QJsonObject block = events.at(1).value("message").toObject()
                                      .value("content").toArray().at(0).toObject();
        check(events.at(1).value("type").toString() == "assistant" &&
                  block.value("type").toString() == "text" &&
                  block.value("text").toString() == "On it.",
              "a node reply is an assistant text block");
    }

    CloudAgentSession runner;
    runner.sessionId = "s-tools";
    runner.history.append(CloudAgentTurn{
        "assistant", "fmnext-r1",
        QString::fromUtf8("Looking around.\n\n\u25b8 Bash: git status\n\n"
                          "\u25b8 Read: src/app.cpp\n\n\u25b8 TodoWrite\n\n"
                          "Done.\n\nAll good."),
        3000});
    const QList<QJsonObject> runnerEvents = cloudAgentTranscriptEvents(runner);
    check(runnerEvents.size() == 3,
          "a runner reply splits into prose, its tool calls, then prose");
    if (runnerEvents.size() == 3) {
        const QJsonArray tools = runnerEvents.at(1).value("message").toObject()
                                     .value("content").toArray();
        check(tools.size() == 3 &&
                  tools.at(0).toObject().value("type").toString() == "tool_use" &&
                  tools.at(0).toObject().value("name").toString() == "Bash" &&
                  tools.at(0).toObject().value("input").toObject()
                          .value("command").toString() == "git status" &&
                  tools.at(1).toObject().value("input").toObject()
                          .value("file_path").toString() == "src/app.cpp" &&
                  tools.at(2).toObject().value("input").toObject().keys() ==
                      QStringList{"_summaryOnly"} &&
                  tools.at(0).toObject().value("id").toString() !=
                      tools.at(1).toObject().value("id").toString(),
              "a runner tool line becomes a tool_use block the view draws");
        check(runnerEvents.at(2).value("message").toObject().value("content")
                      .toArray().at(0).toObject().value("text").toString() ==
                  "Done.\n\nAll good." &&
                  runnerEvents.at(2).value("_ts").toDouble() == 3000,
              "prose after the tools stays one dated text block");
    }
    runner.history.first().text = QString::fromUtf8("\u25b8 Grep: TODO");
    const QList<QJsonObject> trailing = cloudAgentTranscriptEvents(runner);
    check(trailing.size() == 2 &&
              trailing.at(1).value("message").toObject().value("content")
                      .toArray().at(0).toObject().value("type").toString() ==
                  "text",
          "a reply ending on a tool call is closed so the view stops its spinner");

    const QString path = "/api/repo/alice/site/agent-sessions";
    // adhoc #404: the account-wide answer covers every repository at once, so
    // each row addresses itself and one that cannot is not a row at all.
    const QJsonObject accountPayload = QJsonDocument::fromJson(R"json({
      "ok": true, "account": "alice",
      "sessions": [
        {"id": "s-one", "org": "alice", "repo": "site", "status": "running",
         "targetNode": "mirror-ewr", "title": "Fix the login page"},
        {"id": "s-two", "org": "alice", "repo": "notes", "status": "queued",
         "targetNode": "fmnext-r1", "onRunner": true, "title": "Sort the inbox"},
        {"id": "s-mine", "org": "alice", "repo": "site", "status": "running",
         "targetNode": "desk", "title": "Runs on this desktop"},
        {"id": "s-nowhere", "status": "running", "targetNode": "mirror-ams"},
        {"id": "s-web", "org": "acme", "repo": "site", "status": "running",
         "scope": "organization", "targetNode": "mirror-ewr",
         "title": "Queued on the website"}
      ]})json").object();
    const QList<CloudAgentSession> account =
        parseCloudAgentAccountSessions(accountPayload, "Desk");
    check(account.size() == 4,
          "the account answer keeps every repository's runs, this desktop's "
          "own included, minus any row it cannot address");
    check(!account.at(0).targetsThisNode && !account.at(1).targetsThisNode &&
              account.at(2).targetsThisNode && account.at(2).sessionId == "s-mine",
          "a run aimed at this desktop is marked, not dropped");
    check(!account.at(0).onRunner && account.at(1).onRunner &&
              !account.at(2).onRunner,
          "a run the relay sent to a runner is marked, so it stays there");
    check(account.at(0).owner == "alice" && account.at(0).name == "site" &&
              account.at(1).name == "notes",
          "each account row is addressed by the repository it names");
    check(account.at(1).key() == "alice/notes/s-two",
          "a row from another repository keys to that repository");
    check(!account.at(0).organization && account.at(3).organization &&
              account.at(3).owner == "acme" && account.at(3).name == "site",
          "an organization's web run is listed and marked as the org's");

    check(cloudAgentProofCanonical(cloudAgentAccountListProof(), "alice",
                                   "/api/account/agent-sessions", QString(),
                                   "1700") ==
              QByteArray("forkmesh-owner-agent-account-list-v1\nalice\n"
                         "/api/account/agent-sessions\n1700"),
          "the account list canonical is its own proof, user, path, time");
    check(cloudAgentClaimPath(path + "/s-wait") ==
              "/api/repo/alice/site/agent-sessions/s-wait/claim",
          "a claim is the session's own path plus /claim");
    check(cloudAgentProofCanonical(cloudAgentListProof(), "alice", path,
                                   QString(), "1700") ==
              QByteArray("forkmesh-owner-agent-list-v1\nalice\n"
                         "/api/repo/alice/site/agent-sessions\n1700"),
          "the list canonical is proof, user, path, time");
    check(cloudAgentProofCanonical(cloudAgentDeleteProof(), "alice",
                                   path + "/s-run", "ignored", "1700") ==
              QByteArray("forkmesh-owner-agent-delete-v1\nalice\n"
                         "/api/repo/alice/site/agent-sessions/s-run\n1700"),
          "only a follow-up binds a prompt");
    const QString prompt = QString::fromUtf8("also fix the tests\nand rebase \xE2\x9C\x93");
    const QByteArray digest =
        QCryptographicHash::hash(prompt.toUtf8(), QCryptographicHash::Sha256)
            .toHex();
    check(cloudAgentProofCanonical(cloudAgentPromptProof(), "alice",
                                   path + "/s-run", prompt, "1700") ==
              QByteArray("forkmesh-owner-agent-prompt-v1\nalice\n"
                         "/api/repo/alice/site/agent-sessions/s-run\n") +
                  digest + QByteArray("\n1700"),
          "a follow-up binds the SHA-256 of its UTF-8 prompt");
    const QString queueText = cloudAgentQueueText(
        "claude-code", "claude-opus-5-5", true, "desk", prompt);
    check(queueText == QString("claude-code\nclaude-opus-5-5\n1\ndesk\n") + prompt,
          "a queued run's signed text is provider, model, pull request, "
          "device and prompt, one per line");
    check(cloudAgentProofCanonical(cloudAgentQueueProof(), "alice", path,
                                   queueText, "1700") ==
              QByteArray("forkmesh-owner-agent-queue-v1\nalice\n"
                         "/api/repo/alice/site/agent-sessions\n") +
                  QCryptographicHash::hash(queueText.toUtf8(),
                                           QCryptographicHash::Sha256)
                      .toHex() +
                  QByteArray("\n1700"),
          "a queued run binds the SHA-256 of its whole signed text");

    const QJsonObject statePayload = QJsonDocument::fromJson(R"json({
      "ok": true,
      "sessions": [
        {"id": "q", "provider": "claude-code", "status": "queued",
         "title": "Queued run",
         "history": [{"role": "user", "text": "Add a README", "at": 1}]},
        {"id": "r", "provider": "claude-code", "status": "running",
         "localAgentId": 4, "title": "Running run",
         "history": [{"role": "user", "text": "Refactor", "at": 1}]},
        {"id": "f-cred", "provider": "claude-code", "status": "failed",
         "targetNode": "fmnext-n1", "localAgentId": 9, "title": "Fix login",
         "requestedModel": "opus", "createPr": true,
         "history": [
           {"role": "user", "author": "jett", "text": "Fix the login page", "at": 1},
           {"role": "assistant", "author": "fmnext-n1", "text": "Looking at auth.py", "at": 2},
           {"role": "user", "author": "jett", "text": "Also the logout", "at": 3}
         ],
         "diagnostic": {"code": "provider_credential_missing",
                        "level": "attention",
                        "message": "OAuth session expired"}},
        {"id": "f-other", "provider": "codex", "status": "failed",
         "localAgentId": 2, "title": "Broken",
         "history": [{"role": "user", "text": "Break it", "at": 1}],
         "agentInfo": {"lastError": "tests failed"},
         "diagnostic": {"code": "agent_failed", "level": "attention",
                        "message": "tests failed"}},
        {"id": "f-oauth", "provider": "claude-code", "status": "failed",
         "localAgentId": 3, "title": "Auth text only",
         "history": [{"role": "user", "text": "Go", "at": 1}],
         "agentInfo": {"lastError": "API Error: 401 OAuth session expired. Please run /login"},
         "diagnostic": {"code": "agent_failed", "level": "attention",
                        "message": "API Error: 401 OAuth session expired. Please run /login"}},
        {"id": "done", "provider": "claude-code", "status": "completed",
         "localAgentId": 5, "title": "Done run",
         "history": [{"role": "user", "text": "Ship it", "at": 1}]}
      ]})json").object();
    const QList<CloudAgentSession> states =
        parseCloudAgentSessions(statePayload, "jett", "site", "desk");
    check(states.size() == 6, "every state row is parsed");
    if (states.size() == 6) {
        using A = CloudAgentAction;
        check(cloudAgentActions(states.at(0)) == QList<A>{A::ClaimHere},
              "a queued run nobody started is only claimed here");
        check(cloudAgentActions(states.at(1)) == QList<A>{A::FollowUp},
              "a running run only takes a follow-up");
        check(states.at(2).diagnosticCode == "provider_credential_missing",
              "the diagnostic code is parsed");
        check(cloudAgentNeedsRunnerSignIn(states.at(2)),
              "a missing credential asks for a runner sign-in");
        check(cloudAgentActions(states.at(2)) ==
                  (QList<A>{A::SignInRunner, A::ContinueLocally, A::FollowUp,
                            A::RetryInCloud}),
              "a credential failure offers sign-in first, then continue and "
              "retry");
        check(!cloudAgentNeedsRunnerSignIn(states.at(3)),
              "an ordinary failure does not ask for a sign-in");
        check(states.at(3).lastError == "tests failed",
              "the run's last error is kept");
        check(cloudAgentActions(states.at(3)) ==
                  (QList<A>{A::ContinueLocally, A::FollowUp, A::RetryInCloud}),
              "an ordinary failure offers continue and retry");
        check(cloudAgentNeedsRunnerSignIn(states.at(4)),
              "an expired OAuth error text also asks for a sign-in");
        check(cloudAgentActions(states.at(5)) ==
                  (QList<A>{A::ContinueLocally, A::FollowUp, A::RetryInCloud}),
              "a completed run can be continued or run again");
        check(!cloudAgentFinished(states.at(0)) &&
                  !cloudAgentFinished(states.at(1)) &&
                  cloudAgentFinished(states.at(5)),
              "only ended runs count as finished");

        const CloudAgentSession &failed = states.at(2);
        check(cloudAgentOriginalPrompt(failed) == "Fix the login page",
              "the original prompt is the first user turn");
        const QString seeded = cloudAgentContinuationPrompt(failed);
        check(seeded.contains("f-cred") && seeded.contains("fmnext-n1") &&
                  seeded.contains("OAuth session expired"),
              "the continuation names the run, the node and why it stopped");
        check(seeded.contains("The original request:\n\nFix the login page"),
              "the continuation carries the original prompt");
        const int reply =
            seeded.indexOf("Assistant (fmnext-n1):\nLooking at auth.py");
        const int followUp = seeded.indexOf("User (jett):\nAlso the logout");
        check(reply > 0 && followUp > reply,
              "the conversation after the prompt follows in order");
        check(seeded.count("Fix the login page") == 1,
              "the original prompt is not repeated in the history");
        check(cloudAgentContinuationTitle(failed) == "Continue: Fix login",
              "the local run is titled after the cloud run");
        check(cloudAgentQueueRuntime("claude-code") == "claude-code" &&
                  cloudAgentQueueRuntime("codex") == "codex" &&
                  cloudAgentQueueRuntime("openai").isEmpty(),
              "only Claude Code and Codex can be queued again");

        CloudAgentSession longRun = failed;
        longRun.history.clear();
        longRun.history.append({"user", "jett", "Start", 1});
        for (int i = 0; i < 200; ++i)
            longRun.history.append(
                {"assistant", "n1",
                 QString(400, QLatin1Char('x')) + QString::number(i),
                 qint64(i + 2)});
        const QString bounded = cloudAgentContinuationPrompt(longRun);
        check(bounded.size() < 30 * 1024, "a long conversation is bounded");
        check(bounded.contains("older turns left out") &&
                  bounded.contains("x199") &&
                  bounded.contains("The original request:\n\nStart"),
              "the newest turns and the original prompt are kept");

        CloudAgentSession truncated = failed;
        truncated.historyTruncated = true;
        check(cloudAgentOriginalPrompt(truncated) == "Fix login",
              "a truncated history falls back to the title");
    }

    {
        const QJsonObject runnerPayload = QJsonDocument::fromJson(R"json({
          "sessions": [
            {"id": "r-done", "org": "fmnext-n1", "repo": "forkmesh",
             "provider": "claude-code", "status": "failed",
             "targetNode": "fmnext-n1", "localAgentId": 9,
             "requestedModel": "claude-opus-5-5", "title": "Fix login",
             "history": [{"role": "user", "text": "Fix the login page"},
                         {"role": "assistant", "text": "Half done."}],
             "agentInfo": {},
             "branch": "agent/r-done", "baseBranch": "main",
             "runner": "fmnext-r1",
             "diffStats": {"files": 3, "added": 40, "removed": 2,
                           "ahead": 2, "known": true},
             "merge": {"state": ""}, "canMerge": true},
            {"id": "r-merged", "org": "fmnext-n1", "repo": "forkmesh",
             "provider": "claude-code", "status": "completed",
             "localAgentId": 4, "branch": "agent/r-merged",
             "baseBranch": "main", "merge": {"state": "merged"}},
            {"id": "r-queued", "org": "fmnext-n1", "repo": "forkmesh",
             "provider": "claude-code", "status": "queued",
             "localAgentId": 5, "branch": "agent/r-queued",
             "baseBranch": "main"},
            {"id": "r-waiting", "org": "fmnext-n1", "repo": "forkmesh",
             "provider": "codex", "status": "queued"},
            {"id": "r-main", "org": "fmnext-n1", "repo": "forkmesh",
             "provider": "codex", "status": "failed", "localAgentId": 6,
             "agentInfo": {"branchName": "main"}}
          ]})json").object();
        const QList<CloudAgentSession> runs =
            parseCloudAgentAccountSessions(runnerPayload, QStringLiteral("desk"));
        check(runs.size() == 5, "every runner row is parsed");
        const CloudAgentSession &done = runs.at(0);
        check(done.branchName == "agent/r-done" && done.baseBranch == "main",
              "the runner projection's branch and base are read");
        check(done.runner == "fmnext-r1" && cloudAgentRunnerName(done) ==
                                                "fmnext-r1",
              "the runner that ran it is named");
        check(done.model == "claude-opus-5-5", "the run's model is kept");
        check(done.diffKnown && done.diffFiles == 3 && done.diffAhead == 2 &&
                  done.canMerge && !done.merged,
              "diff stats and merge state are read");
        check(runs.at(1).merged && runs.at(1).mergeState == "merged",
              "merge.state merged marks the run merged");

        const CloudAgentContinuePlan onTip = cloudAgentContinuePlan(done, true);
        check(onTip.start == CloudContinueStart::SessionBranch &&
                  onTip.startBranch == "agent/r-done" &&
                  onTip.landingBase == "main",
              "a fetched tip is where the local branch starts, landing on main");
        check(cloudAgentContinueBranch(done) == "agent/r-done",
              "the session branch is the one fetched");
        const QString onTipPrompt =
            cloudAgentBranchContinuationPrompt(done, onTip);
        check(onTipPrompt.contains("continues a ForkMesh cloud agent run "
                                   "(session r-done)"),
              "the prompt says it continues the cloud session");
        check(onTipPrompt.contains("pushed to branch agent/r-done (started "
                                   "from main)") &&
                  onTipPrompt.contains("2 commits ahead, with 3 files changed "
                                       "(+40/-2)") &&
                  onTipPrompt.contains("already here"),
              "the prompt summarizes the branch it starts on");
        check(onTipPrompt.indexOf("already here") <
                  onTipPrompt.indexOf("The original request:\n\nFix the login "
                                      "page"),
              "the branch summary comes before the original prompt");
        check(onTipPrompt.contains("Half done."),
              "the conversation so far is carried");

        const CloudAgentContinuePlan missing =
            cloudAgentContinuePlan(done, false);
        check(missing.start == CloudContinueStart::BranchMissing &&
                  missing.startBranch == "main" &&
                  missing.notice.contains("agent/r-done is not on the mirror") &&
                  missing.notice.contains("starts from main"),
              "a branch missing on the mirror falls back to the base branch");
        check(cloudAgentBranchContinuationPrompt(done, missing)
                  .contains("None of the cloud run's changes"),
              "the fallback prompt says nothing from the cloud run is here");

        const CloudAgentContinuePlan merged =
            cloudAgentContinuePlan(runs.at(1), true);
        check(cloudAgentContinueBranch(runs.at(1)).isEmpty(),
              "a merged branch is not fetched");
        check(merged.start == CloudContinueStart::BranchMerged &&
                  merged.startBranch == "main" &&
                  merged.notice.contains("already merged"),
              "a merged branch starts from the base branch and says so");

        const CloudAgentContinuePlan none =
            cloudAgentContinuePlan(runs.at(3), false);
        check(none.start == CloudContinueStart::NoBranch &&
                  none.startBranch.isEmpty() &&
                  none.notice.contains("left no branch") &&
                  none.notice.contains("the default branch"),
              "a run without a branch starts from the default branch");
        check(cloudAgentContinueBranch(runs.at(4)).isEmpty() &&
                  cloudAgentContinuePlan(runs.at(4), true).start ==
                      CloudContinueStart::NoBranch,
              "a branch outside agent/ is never fetched over a local one");

        check(cloudAgentStoppable(runs.at(2)),
              "a started run queued with follow-ups can be stopped");
        check(!cloudAgentStoppable(runs.at(3)) && runs.at(3).claimable(),
              "a run no node started is claimed, not stopped");
        check(!cloudAgentStoppable(done) && !cloudAgentStoppable(runs.at(1)),
              "an ended run has nothing to stop");
        CloudAgentSession running = runs.at(2);
        running.status = QStringLiteral("running");
        check(cloudAgentStoppable(running), "a running run can be stopped");
        check(cloudAgentContinuePlan(runs.at(2), true).startBranch ==
                  "agent/r-queued",
              "a stopped queued run continues on its branch");

        const QByteArray stop = cloudAgentProofCanonical(
            cloudAgentStopProof(), "alice",
            "/api/repo/alice/project/agent-sessions/s1", "ignored", "1700");
        check(stop == "forkmesh-owner-agent-stop-v1\nalice\n"
                      "/api/repo/alice/project/agent-sessions/s1\n1700",
              "the stop canonical binds the operation, signer, path and time");
    }

    {
        const QJsonObject gone{{"ok", false}, {"error", "not_found"}};
        const QJsonObject forbidden{{"ok", false}, {"error", "forbidden"}};
        check(cloudAgentDeleteFoundNothing(404, gone),
              "a 404 not_found delete means the relay has nothing to delete");
        check(!cloudAgentDeleteFoundNothing(403, forbidden),
              "a refused delete is still an error");
        check(!cloudAgentDeleteFoundNothing(404, QJsonObject()),
              "a bare 404 without the relay's not_found is still an error");
        check(!cloudAgentDeleteFoundNothing(500, gone),
              "only the 404 status counts");
    }

    {
        const QJsonObject runs = QJsonDocument::fromJson(R"json({
          "ok": true,
          "sessions": [
            {"id": "open", "org": "alice", "repo": "site", "status": "completed",
             "branch": "agent/3-x", "baseBranch": "main", "runner": "fmnext-r1",
             "canMerge": true, "canOpenPull": true, "prNumber": 0,
             "agentInfo": {"prNumber": 0}, "merge": {"state": ""}},
            {"id": "linked", "org": "alice", "repo": "site", "status": "completed",
             "branch": "agent/4-x", "canOpenPull": true, "prNumber": 57,
             "agentInfo": {"prNumber": 0}, "merge": {"state": ""}},
            {"id": "landed", "org": "alice", "repo": "site", "status": "completed",
             "branch": "agent/5-x", "merge": {"state": "merged",
             "branchDeleted": true, "cleanupPending": ["fmnext-n2"]}},
            {"id": "clean", "org": "alice", "repo": "site", "status": "completed",
             "branch": "agent/6-x", "merge": {"state": "merged",
             "branchDeleted": true, "cleanupPending": []}}
          ]})json").object();
        const QList<CloudAgentSession> rows =
            parseCloudAgentAccountSessions(runs, "desk");
        check(rows.size() == 4, "every run with a repository is listed");
        check(rows.at(0).canOpenPull && rows.at(0).prNumber == 0 &&
                  rows.at(0).canMerge,
              "an unmerged run with no pull request offers Create PR");
        check(rows.at(1).prNumber == 57 && !rows.at(1).canOpenPull,
              "a linked pull request outlives the runner's report of 0");
        check(rows.at(2).merged && rows.at(2).canCleanup,
              "a merge with a core still holding the branch offers Clean up");
        check(rows.at(3).merged && !rows.at(3).canCleanup,
              "a fully cleaned merge offers nothing more");
        check(cloudAgentListEntry(rows.at(1), -1000001).prNumber == 57,
              "the list row shows the linked pull request");

        const QJsonObject merge = cloudAgentMergeBody("ab-123456789012");
        check(cloudAgentBranchActionText(merge) == "merge\n1\n\nab-123456789012\n",
              "a merge signs its action, cleanup flag and request id");
        check(cloudAgentBranchActionText(cloudAgentCleanupBody("ab-1")) ==
                  "cleanup\n\n\nab-1\n",
              "a clean up signs no flags it did not send");
        check(cloudAgentBranchActionText(cloudAgentLinkPullBody(57)) ==
                  "link-pull\n\n\n\n57",
              "a pull link signs its number");
        check(cloudAgentBranchActionText(
                  QJsonObject{{"action", "merge"}, {"prompt", "x"}})
                  .isEmpty(),
              "nothing else may ride along on a branch action");
        check(cloudAgentBranchActionText(cloudAgentUpdateBody("ab-2")) ==
                  "update\n\n\nab-2\n",
              "an update from the base signs its request id");
        CloudAgentSession landed = rows.at(0);
        check(cloudAgentMergedTaskNote(
                  landed,
                  QJsonDocument::fromJson(R"json({"status": "merged",
                    "base": "main", "mergeOid": "abcdef1234567",
                    "message": "Merged agent/3-x into main and cleaned up."})json")
                      .object()) ==
                  "Merged agent/3-x into main (abcdef1). Merged agent/3-x into "
                  "main and cleaned up.",
              "the task note names the branch, base and merge commit");
        check(cloudAgentBranchActionText(QJsonObject{{"action", "stop"}})
                  .isEmpty(),
              "stop keeps its own proof");
        check(cloudAgentBranchActionText(
                  QJsonObject{{"action", "merge"}, {"cleanup", "yes"}})
                  .isEmpty(),
              "flags are booleans");
        const QString text = cloudAgentBranchActionText(merge);
        const QString path = "/api/repo/alice/site/agent-sessions/open";
        check(cloudAgentProofCanonical(cloudAgentBranchProof(), "alice", path,
                                       text, "1700") ==
                  QByteArray("forkmesh-owner-agent-branch-v1\nalice\n") +
                      path.toUtf8() + QByteArray("\n") +
                      QCryptographicHash::hash(text.toUtf8(),
                                               QCryptographicHash::Sha256)
                          .toHex() +
                      QByteArray("\n1700"),
              "a branch action binds the SHA-256 of its signed text");
        check(QJsonDocument(cloudAgentLinkPullBody(57)).toJson(QJsonDocument::Compact) ==
                  QByteArray("{\"action\":\"link-pull\",\"number\":57}"),
              "the pull number travels as a JSON integer");

        const QJsonObject merged = QJsonDocument::fromJson(R"json({
          "ok": true, "status": "merged",
          "message": "Merged agent/3-x into main and cleaned up.",
          "stats": {"commits": 3, "files": 5, "insertions": 40, "deletions": 2}
        })json").object();
        check(cloudAgentMergeOutcome(merged, "agent/3-x", "main") ==
                  QString::fromUtf8("Merged agent/3-x into main and cleaned up. "
                                    "3 commits \xC2\xB7 5 files +40 \xE2\x88\x92" "2."),
              "a merge reads like the desktop's celebration");
        const QJsonObject conflict = QJsonDocument::fromJson(R"json({
          "ok": false, "status": "failed", "error": "merge_conflict",
          "message": "agent/3-x conflicts with main.",
          "conflicts": ["a.txt", "b.txt"]
        })json").object();
        check(cloudAgentMergeOutcome(conflict, "agent/3-x", "main") ==
                  "agent/3-x conflicts with main. Conflicts: a.txt, b.txt.",
              "a conflict names its files");
    }

    {
        const QJsonObject withImages = QJsonDocument::fromJson(R"json({
          "ok": true,
          "sessions": [
            {"id": "s-img", "org": "alice", "repo": "site", "status": "failed",
             "targetNode": "fmnext-r1",
             "history": [
               {"role": "user", "author": "alice", "text": "Make it look like this",
                "at": 1000,
                "attachments": [
                  {"id": "0123456789abcdef0123456789abcdef", "name": "a.png",
                   "mediaType": "image/png", "size": 12},
                  {"id": "../../etc/passwd", "mediaType": "image/png", "size": 12},
                  {"id": "fedcba9876543210fedcba9876543210",
                   "mediaType": "text/html", "size": 12},
                  {"id": "11111111111111111111111111111111",
                   "mediaType": "image/jpeg", "size": 99999999}
                ]},
               {"role": "user", "author": "alice", "text": "", "at": 2000,
                "attachments": [
                  {"id": "22222222222222222222222222222222",
                   "mediaType": "image/webp", "size": 40}]},
               {"role": "assistant", "text": "Done.", "at": 3000,
                "attachments": [
                  {"id": "33333333333333333333333333333333",
                   "mediaType": "image/png", "size": 40}]}
             ]}
          ]})json").object();
        const QList<CloudAgentSession> rows =
            parseCloudAgentAccountSessions(withImages, QStringLiteral("desk"));
        check(rows.size() == 1, "the session with images is a row");
        if (rows.size() == 1) {
            const CloudAgentSession &run = rows.first();
            check(run.history.size() == 3,
                  "a user turn carrying only images is kept");
            check(run.history.size() == 3 && run.history.at(0).images.size() == 1 &&
                      run.history.at(0).images.first().id ==
                          "0123456789abcdef0123456789abcdef" &&
                      run.history.at(0).images.first().size == 12,
                  "only well-formed prompt images are kept");
            check(run.history.size() == 3 && run.history.at(2).images.isEmpty(),
                  "an assistant turn never carries prompt images");
            const QString first =
                cloudAgentImageFileName(run, run.history.at(0).images.first());
            check(first == "cloud-s-img-0123456789abcdef0123456789abcdef.png",
                  "a cached image is named by session and attachment");
            CloudAgentSession hostile = run;
            hostile.sessionId = "../x/../y";
            check(!cloudAgentImageFileName(hostile, run.history.at(0).images.first())
                       .contains('/'),
                  "a session id cannot steer the cache path");

            const QList<QJsonObject> bare = cloudAgentTranscriptEvents(run);
            check(bare.size() == 3 &&
                      bare.at(0).value("message").toObject().value("content")
                              .toString() == "Make it look like this",
                  "without a cache folder the prompt is its text");

            QTemporaryDir dir;
            QFile cached(dir.filePath(first));
            check(cached.open(QIODevice::WriteOnly) && cached.write("png") == 3,
                  "a cached image is written");
            cached.close();
            const QList<QJsonObject> shown = cloudAgentTranscriptEvents(run, dir.path());
            check(shown.size() == 3 &&
                      shown.at(0).value("message").toObject().value("content")
                              .toString() ==
                          "Make it look like this\nAttached image: " +
                              dir.filePath(first),
                  "a downloaded image is shown under its prompt");
            check(shown.size() == 3 &&
                      shown.at(1).value("message").toObject().value("content")
                              .toString()
                              .isEmpty(),
                  "an image not downloaded yet is left out");
        }
    }

    if (failures == 0)
        std::cout << "agent cloud sessions: all checks passed\n";
    return failures == 0 ? 0 : 1;
}

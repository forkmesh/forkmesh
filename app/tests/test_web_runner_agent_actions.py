#!/usr/bin/env python3

import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
BUNDLE = (ROOT / "public/dashboard.js").read_text()


def _function(source, name):
    start = source.index(f"function {name}(")
    cursor = source.index("{", start)
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(f"unterminated JavaScript function: {name}")


def _constant(name):
    text = REPO_UI[REPO_UI.index(f"const {name}"):]
    return text[:text.index(";") + 1]


def _run(expression, functions):
    script = "\n".join([
        _constant("REPO_AGENT_ACTIVE_STATUSES"),
        _constant("ORG_AGENT_FINISHED_STATUSES"),
        "const formatCount = (n) => String(n);",
        *(_function(REPO_UI, name) for name in functions),
        f"console.log(JSON.stringify({expression}));",
    ])
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def _facts(session):
    return _run(
        f"orgAgentBranchFacts({json.dumps(session)})",
        ["orgAgentBranchFacts"])


def test_the_detail_offers_the_desktop_actions_for_a_runner_session():
    detail = _function(REPO_UI, "renderOrgAgentSessionDetail")
    for action, label in (
        ("diff", "Diff"), ("stop", "Stop"), ("continue", "Continue"),
        ("merge", "Merge & clean up"), ("cleanup", "Clean up"),
    ):
        assert f'orgAgentOpsButton("{action}"' in detail
        assert f'"{label}"' in detail
    assert "Runner · ${escapeHtml(branchFacts.runner)}" in detail
    assert "branchFacts.branch)} → ${escapeHtml(branchFacts.base" in detail
    assert "orgAgentStatsHtml(branchFacts)" in detail
    assert "renderOrgAgentRunnerDiff(branchFacts)" in detail


def test_the_actions_send_the_app_requests():
    run = _function(REPO_UI, "runOrgAgentAction")
    assert '{ action: "stop" }' in run
    assert "{ prompt: ORG_AGENT_CONTINUE_PROMPT }" in run
    assert '{ action: "merge", cleanup: true, requestId: orgAgentRequestId() }' in run
    assert '{ action: "cleanup", requestId: orgAgentRequestId() }' in run
    assert 'const ORG_AGENT_CONTINUE_PROMPT = "Continue where you left off.";' in REPO_UI
    diff = _function(REPO_UI, "orgAgentLoadDiffPage")
    assert "/diff?" in diff and "pageSize" in diff
    poll = _function(REPO_UI, "orgAgentPostUntilDone")
    assert 'payload?.status !== "processing"' in poll
    assert re.search(r"orgAgentRequest\(\"POST\", orgAgentEndpoint\(repo, sessionId\), body\)", poll)
    wire = _function(REPO_UI, "wireOrgAgentSessionActions")
    assert "[data-org-agent-action]" in wire and "runOrgAgentAction" in wire


def test_merge_and_cleanup_follow_the_session_state():
    running = _facts({"status": "running", "branch": "agent/1-x", "localAgentId": 3})
    assert running["active"] and not running["canMerge"] and not running["canContinue"]
    done = _facts({
        "status": "completed", "branch": "agent/1-x", "baseBranch": "main",
        "runner": "fmnext-r1", "localAgentId": 3, "canMerge": True})
    assert done["canMerge"] and done["canContinue"] and not done["canCleanup"]
    assert done["runner"] == "fmnext-r1" and done["base"] == "main"
    merged = _facts({
        "status": "completed", "branch": "agent/1-x", "canMerge": False,
        "merge": {"state": "merged", "branchDeleted": True,
                  "cleanupPending": ["fmnext-n2"]}})
    assert merged["merged"] and merged["canCleanup"] and not merged["canMerge"]
    cleaned = _facts({
        "status": "completed", "branch": "agent/1-x",
        "merge": {"state": "merged", "branchDeleted": True,
                  "cleanupPending": []}})
    assert not cleaned["canCleanup"] and not cleaned["canMerge"]


def test_the_merge_line_reads_like_the_desktop_celebration():
    text = _run(
        'orgAgentMergeStatsText("agent/1-x", "main", {branchDeleted: true, '
        'stats: {base: "main", commits: 3, files: 5, insertions: 40, deletions: 2}})',
        ["orgAgentMergeStatsText"])
    assert text == ("Merged agent/1-x into main and cleaned up. "
                    "3 commits · 5 files +40 −2.")
    pending = _run(
        'orgAgentMergeStatsText("agent/1-x", "main", {branchDeleted: true, '
        'cleanupPending: ["fmnext-n2"]})',
        ["orgAgentMergeStatsText"])
    assert pending.endswith("Clean up still pending on fmnext-n2.")


def test_the_bundle_carries_the_new_actions():
    for name in ("runOrgAgentAction", "orgAgentPostUntilDone",
                 "renderOrgAgentRunnerDiff", "orgAgentBranchFacts"):
        assert f"function {name}(" in BUNDLE


def test_a_merge_still_publishing_waits_and_can_be_finished():
    assert "const ORG_AGENT_MERGE_POLLS = 90;" in REPO_UI
    assert "const ORG_AGENT_MERGE_POLL_MS = 2000;" in REPO_UI
    poll = _function(REPO_UI, "orgAgentPostUntilDone")
    assert "ORG_AGENT_MERGE_POLL_MS" in poll and "Finish merge" in poll
    facts = _function(REPO_UI, "orgAgentBranchFacts")
    assert 'merge.state === "merging"' in facts
    detail = _function(REPO_UI, "renderOrgAgentSessionDetail")
    assert 'branchFacts.merging ? "Finish merge" : "Merge & clean up"' in detail
    assert "Merging…" in detail


def test_the_merged_run_lists_its_actions_with_their_own_buttons():
    detail = _function(REPO_UI, "renderOrgAgentSessionDetail")
    assert "renderOrgAgentMergeActions(repo, session, branchFacts, canManage)" in detail
    panel = _function(REPO_UI, "renderOrgAgentMergeActions")
    for kind in ("dispatch", "dispatch-all", "rerun", "cancel", "approve",
                 "refresh", "open"):
        assert f'small("{kind}"' in panel
    load = REPO_UI[REPO_UI.index("async function orgAgentLoadMergeActions("):
                   REPO_UI.index("async function orgAgentMergeActionsControl(")]
    assert "/actions/runs" in load
    control = _function(REPO_UI, "orgAgentMergeActionsControl")
    assert "/actions/control" in control and "node: view.node" in control
    action = _function(REPO_UI, "runOrgAgentMergeAction")
    assert "refs/heads/${base}" in action
    run = _function(REPO_UI, "runOrgAgentAction")
    assert "landed = true" in run and "orgAgentLoadMergeActions(repo, sessionId)" in run
    wire = _function(REPO_UI, "wireOrgAgentSessionActions")
    assert "[data-org-agent-merge-action]" in wire
    runs = _function(REPO_UI, "orgAgentMergeRuns")
    assert "run.commit === mergeOid" in runs
    for name in ("renderOrgAgentMergeActions", "orgAgentLoadMergeActions",
                 "runOrgAgentMergeAction"):
        assert f"function {name}(" in BUNDLE


def test_a_runner_session_offers_and_links_its_pull_request():
    detail = _function(REPO_UI, "renderOrgAgentSessionDetail")
    assert 'orgAgentOpsButton("pull", "git-pull-request", "Create PR"' in detail
    assert "data-org-agent-pull" in detail and "PR #${prNumber}" in detail
    run = _function(REPO_UI, "runOrgAgentAction")
    assert '"pull"].includes(action)' in run
    assert "orgAgentOpenPull(repo, session, branchFacts)" in run
    offered = _facts({"status": "completed", "branch": "agent/1-x",
                      "canOpenPull": True, "prNumber": 0})
    assert offered["canOpenPull"] and offered["prNumber"] == 0
    linked = _facts({"status": "completed", "branch": "agent/1-x",
                     "canOpenPull": True, "prNumber": 12})
    assert linked["prNumber"] == 12 and not linked["canOpenPull"]
    reported = _facts({"status": "completed", "branch": "agent/1-x",
                       "agentInfo": {"prNumber": 4}})
    assert reported["prNumber"] == 4 and not reported["canOpenPull"]


def _open_pull(submit):
    stubs = "\n".join([
        "const sent = [];",
        f"const submitWebPullOpen = {submit};",
        "const orgAgentRequest = async (method, url, body) => {"
        " sent.push({method, url, body});"
        " return {message: `Opened pull request #${body.number} for agent/1-x.`}; };",
        "const orgAgentEndpoint = (repo, id) => `/api/repo/alice/proj/agent-sessions/${id}`;",
        "const repoDefaultBranch = () => 'main';",
    ])
    session = {
        "id": "s1", "title": "Fix login", "runner": "fmnext-r1",
        "history": [{"role": "user", "text": "fix it"},
                    {"role": "assistant", "text": "Fixed the login form."}],
    }
    facts = {"branch": "agent/1-x", "base": "main", "runner": "fmnext-r1"}
    script = "\n".join([
        stubs,
        _function(REPO_UI, "orgAgentPullDescription"),
        _function(REPO_UI, "orgAgentProviderLabel"),
        "async " + _function(REPO_UI, "orgAgentOpenPull"),
        "let result;",
        f"try {{ result = await orgAgentOpenPull({{}}, {json.dumps(session)}, {json.dumps(facts)}); }}",
        "catch (error) { result = {error: error.message}; }",
        "console.log(JSON.stringify({result, sent}));",
    ])
    output = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True)
    return json.loads(output.stdout)


def test_create_pr_signs_the_branch_then_links_the_number():
    calls = _open_pull(
        "async (repo, title, body, base, head) => "
        "({ok: true, number: 57, title, body, base, head})")
    assert calls["result"]["number"] == 57
    assert calls["sent"] == [{
        "method": "POST", "url": "/api/repo/alice/proj/agent-sessions/s1",
        "body": {"action": "link-pull", "number": 57}}]
    description = _run(
        'orgAgentPullDescription({history: [{role: "assistant", text: "Done."}]},'
        ' {runner: "fmnext-r1", branch: "agent/1-x"})',
        ["orgAgentPullDescription"])
    assert description == ("Opened from the agent run on the fmnext-r1 runner "
                           "(agent/1-x).\n\nDone.")


def test_create_pr_explains_a_branch_with_nothing_to_propose():
    calls = _open_pull(
        "async () => { throw new Error('no_changes'); }")
    assert "has no changes against main" in calls["result"]["error"]
    assert calls["sent"] == []


def test_the_agent_speaks_markdown_in_the_transcript():
    entry = _function(REPO_UI, "renderOrgAgentTranscriptEntry")
    assert 'role === "assistant"' in entry
    assert "renderRecordMarkdown(text, repo)" in entry
    assert "escapeHtml(text)" in entry


def _drive_action(action, session, replies, polls=5):
    stubs = "\n".join([
        "const sent = [];",
        f"const replies = {json.dumps(replies)};",
        "const orgAgentRequest = async (method, url, body) => {"
        " sent.push({method, url, body});"
        " const next = replies.shift() || {};"
        " if (next.error) { const error = new Error(next.error.message || next.error.code);"
        " error.code = next.error.code; error.payload = next.error; throw error; }"
        " return next; };",
        "const orgAgentEndpoint = (repo, id) => `/api/repo/alice/proj/agent-sessions/${id}`;",
        "const orgAgentRequestId = () => 'req-0123456789ab';",
        "const orgAgentOps = {busy: new Set(), notice: '', tone: 'ok',"
        " diff: {id: '', open: false}, actions: null};",
        "const patchOrgAgentDetail = () => {};",
        "const refreshOrgAgentsLive = async () => {};",
        "const orgAgentMergeActionsState = () => ({});",
        "const orgAgentLoadMergeActions = async () => {};",
        "const window = {confirm: () => true};",
        "const formatCount = (n) => String(n);",
        f"const ORG_AGENT_MERGE_POLLS = {polls};",
        "const ORG_AGENT_MERGE_POLL_MS = 0;",
        _constant("REPO_AGENT_ACTIVE_STATUSES"),
        _constant("ORG_AGENT_FINISHED_STATUSES"),
        _constant("ORG_AGENT_TASK_KEY"),
        _constant("ORG_AGENT_TASK_TAG"),
    ])
    plain = ("orgAgentBranchFacts", "orgAgentSetNotice", "orgAgentMergeStatsText",
             "orgAgentMergeErrorText", "orgAgentTaskId",
             "orgAgentTaskCompletionNote")
    script = "\n".join([
        stubs,
        *(_function(REPO_UI, name) for name in plain),
        "async " + _function(REPO_UI, "orgAgentPostUntilDone"),
        "async " + _function(REPO_UI, "orgAgentCompleteTask"),
        "async " + _function(REPO_UI, "runOrgAgentAction"),
        f"await runOrgAgentAction({{}}, {json.dumps(session)}, {json.dumps(action)});",
        "console.log(JSON.stringify({sent, notice: orgAgentOps.notice, tone: orgAgentOps.tone}));",
    ])
    output = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True)
    return json.loads(output.stdout)


RUN = {"id": "s1", "status": "completed", "branch": "agent/1-x",
       "baseBranch": "main", "canMerge": True, "localAgentId": 3}
TASK = "0123456789abcdef0123456789abcdef"


def test_the_detail_offers_update_from_the_base_while_mergeable():
    detail = _function(REPO_UI, "renderOrgAgentSessionDetail")
    assert ('branchFacts.canMerge && canManage && !branchFacts.merging '
            '&& branchFacts.branch ? orgAgentOpsButton("update"') in detail
    assert "`Update from ${branchFacts.base || repoDefaultBranch(repo)}`" in detail
    run = _function(REPO_UI, "runOrgAgentAction")
    assert '{ action: "update", requestId: orgAgentRequestId() }' in run


def test_update_polls_the_same_request_until_the_core_answers():
    result = _drive_action("update", RUN, [
        {"ok": True, "status": "processing", "requestId": "req-0123456789ab"},
        {"error": {"code": "merge_status_pending", "message": "Not known yet."}},
        {"ok": True, "status": "updated", "branch": "agent/1-x", "base": "main",
         "message": "Updated agent/1-x from main."},
    ])
    assert [item["body"] for item in result["sent"]] == [
        {"action": "update", "requestId": "req-0123456789ab"}] * 3
    assert all(item["method"] == "POST"
               and item["url"] == "/api/repo/alice/proj/agent-sessions/s1"
               for item in result["sent"])
    assert result["notice"] == "Updated agent/1-x from main."
    assert result["tone"] == "ok"


def test_an_update_that_conflicts_names_the_files():
    result = _drive_action("update", RUN, [
        {"error": {"code": "merge_conflict",
                   "message": "agent/1-x conflicts with main.",
                   "conflicts": ["src/a.py", "src/b.py"]}},
    ])
    assert result["tone"] == "error"
    assert result["notice"] == ("agent/1-x conflicts with main. "
                                "Conflicts: src/a.py, src/b.py.")


def test_a_merged_run_completes_its_task_with_a_note():
    merged = {"ok": True, "status": "merged", "branch": "agent/1-x",
              "base": "main", "mergeOid": "c" * 40, "branchDeleted": True,
              "message": "Merged agent/1-x into main and cleaned up."}
    result = _drive_action("merge", dict(RUN, taskKey=f"task:{TASK}"),
                           [merged, {"ok": True}])
    assert result["sent"][0]["body"] == {
        "action": "merge", "cleanup": True, "requestId": "req-0123456789ab"}
    assert result["sent"][1] == {
        "method": "POST", "url": f"/api/tasks/{TASK}/complete",
        "body": {"completionNote": "Merged agent/1-x into main (ccccccc). "
                                   "Merged agent/1-x into main and cleaned up."}}
    assert result["notice"].endswith("Its task is marked done.")
    assert result["tone"] == "ok"


def test_the_task_tag_in_the_title_or_prompt_is_found_and_a_failure_stays_secondary():
    merged = {"ok": True, "status": "merged", "base": "main", "message": ""}
    titled = _drive_action(
        "merge", dict(RUN, title=f"[task:{TASK.upper()}] Fix login"),
        [merged, {"error": {"code": "not_found"}}])
    assert titled["sent"][1]["url"] == f"/api/tasks/{TASK}/complete"
    assert titled["sent"][1]["body"] == {
        "completionNote": "Merged agent/1-x into main."}
    assert titled["tone"] == "ok"
    assert "could not be marked done" in titled["notice"]
    prompted = _drive_action(
        "merge", dict(RUN, history=[
            {"role": "user", "text": f"  [task:{TASK}] Fix login"}]),
        [merged, {"ok": True}])
    assert prompted["sent"][1]["url"] == f"/api/tasks/{TASK}/complete"
    untracked = _drive_action("merge", dict(RUN, taskKey="task:codex-world"),
                              [merged])
    assert len(untracked["sent"]) == 1

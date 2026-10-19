#!/usr/bin/env python3
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JS_DIR = ROOT / "public" / "dashboard" / "js"
FRAGMENT = JS_DIR / "07-repo-pull-batch.js"
HELPERS = JS_DIR / "02-helpers.js"


def _escape_html_source():
    text = HELPERS.read_text(encoding="utf-8")
    start = text.index("  function escapeHtml(")
    end = text.index("\n  }\n", start) + 4
    return text[start:end]


PRELUDE = r"""
const store = new Map();
globalThis.localStorage = {
  getItem: (key) => (store.has(key) ? store.get(key) : null),
  setItem: (key, value) => { store.set(key, String(value)); },
};
globalThis.location = { host: "code.example.org", pathname: "/" };
function repoDisplayKey(repo) { return `${repo.owner}/${repo.name}`; }
function loadingHtml(label) { return label; }
function webPullSubmitErrorText(error) { return `failed: ${error.message}`; }
function submitRemotePullOpen() { throw new Error("unexpected"); }
const repo = { owner: "drew", name: "bolt" };
const remote = { url: "https://example.net/acme-bolt/bolt", origin: "https://example.net", owner: "acme-bolt", name: "bolt", host: "example.net" };
function row(branch, extra = {}) {
  return { branch, base: "main", title: `Title ${branch}`, body: `Body ${branch}`, dependsOn: [], include: true, missing: false, ...extra };
}
function fakeSend(calls, failures = {}) {
  let next = 40;
  return async (r, target, title, body, base, head, compareBase = "") => {
    calls.push({ title, body, base, head, compareBase, target: target.url });
    if (failures[head]) {
      const error = new Error(failures[head]);
      error.code = failures[head];
      throw error;
    }
    next += 1;
    return { number: next };
  };
}
"""


def _run(body):
    script = (
        _escape_html_source() + PRELUDE + FRAGMENT.read_text(encoding="utf-8")
        + "\nconst out = await (async () => {\n" + body + "\n})();\n"
        + "console.log(JSON.stringify(out));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def test_the_batch_file_is_parsed_and_every_field_sanitized():
    raw = json.dumps([
        {"branch": "pr/a", "title": "  Fix\nthe‮  admin\u0000 sign-in  ", "body": "line\r\none\u0007\r\n\r\n", "dependsOn": None},
        {"branch": "pr/b", "base": "main", "title": "x" * 400, "body": "b", "dependsOn": ["pr/a", "pr/a", "#12", "pr/b", "../evil"]},
        {"branch": "pr/a", "title": "duplicate"},
        {"branch": "../etc", "title": "bad"},
        {"branch": "-x", "title": "bad"},
        {"branch": "has space", "title": "bad"},
        {"branch": "pr/c", "base": "bad..base", "description": "uses description", "dependsOn": "pr/b"},
        "not an object",
    ])
    out = _run(f"return parsePullBatchFile({json.dumps(raw)}, 'main');")
    assert out["error"] == ""
    entries = out["entries"]
    assert [entry["branch"] for entry in entries] == ["pr/a", "pr/b", "pr/c"]
    assert entries[0]["title"] == "Fix the admin sign-in"
    assert entries[0]["body"] == "line\none"
    assert entries[0]["dependsOn"] == []
    assert len(entries[1]["title"]) == 240
    assert entries[1]["dependsOn"] == ["pr/a", "#12"]
    assert entries[2]["base"] == "main"
    assert entries[2]["body"] == "uses description"
    assert entries[2]["dependsOn"] == ["pr/b"]


def test_a_broken_batch_file_reports_why():
    assert "not valid JSON" in _run("return parsePullBatchFile('{nope', 'main');")["error"]
    assert "must be a list" in _run("return parsePullBatchFile('{\"a\": 1}', 'main');")["error"]
    wrapped = _run("return parsePullBatchFile(JSON.stringify({pulls: [{branch: 'pr/x', title: 'X'}]}), 'trunk');")
    assert wrapped["entries"][0]["base"] == "trunk"


def test_rows_follow_the_file_then_the_other_branches_under_the_prefix():
    out = _run("""
      const entries = parsePullBatchFile(JSON.stringify([
        {branch: "pr/zeta", title: "Zeta"}, {branch: "pr/gone", title: "Gone"}, {branch: "other", title: "Other"},
      ]), "main").entries;
      return buildPullBatchRows(entries, ["main", "pr/zeta", "pr/alpha-fix", "other", "pr/beta"], "main", "pr/")
        .map((r) => [r.branch, r.title, r.missing]);
    """)
    assert out == [
        ["pr/zeta", "Zeta", False],
        ["pr/gone", "Gone", True],
        ["pr/alpha-fix", "Alpha fix", False],
        ["pr/beta", "Beta", False],
    ]


def test_dependencies_are_sent_first_and_gate_what_cannot_be_sent():
    out = _run("""
      const rows = [
        row("pr/child", { dependsOn: ["pr/parent"] }),
        row("pr/parent"),
        row("pr/untitled", { title: "" }),
        row("pr/needs-untitled", { dependsOn: ["pr/untitled"] }),
        row("pr/off", { include: false }),
        row("pr/needs-off", { dependsOn: ["pr/off"] }),
        row("pr/loop-a", { dependsOn: ["pr/loop-b"] }),
        row("pr/loop-b", { dependsOn: ["pr/loop-a"] }),
        row("pr/outside", { dependsOn: ["pr/elsewhere"] }),
        row("pr/after-sent", { dependsOn: ["pr/elsewhere-sent", "#7"] }),
        row("pr/missing", { missing: true }),
      ];
      const { order, plan } = pullBatchPlan(rows, { "pr/elsewhere-sent": { number: 3 } });
      return { order: order.map((r) => r.branch), plan: Object.fromEntries([...plan].map(([k, v]) => [k, v])) };
    """)
    order = out["order"]
    assert order.index("pr/parent") < order.index("pr/child")
    plan = out["plan"]
    assert plan["pr/parent"]["state"] == "ready"
    assert plan["pr/child"] == {"state": "after", "waitsFor": ["pr/parent"], "numbers": []}
    assert plan["pr/untitled"]["state"] == "blocked" and plan["pr/untitled"]["reason"] == "Needs a title."
    assert plan["pr/needs-untitled"]["state"] == "blocked"
    assert "cannot be sent yet" in plan["pr/needs-untitled"]["reason"]
    assert plan["pr/off"]["state"] == "skipped"
    assert "which is not selected" in plan["pr/needs-off"]["reason"]
    assert plan["pr/loop-a"]["state"] == plan["pr/loop-b"]["state"] == "blocked"
    assert "loop" in plan["pr/loop-a"]["reason"]
    assert "not in this list" in plan["pr/outside"]["reason"]
    assert plan["pr/after-sent"] == {"state": "ready", "waitsFor": [], "numbers": [3, 7]}
    assert "not a branch" in plan["pr/missing"]["reason"]


def test_the_depends_on_line_leads_the_body_and_the_footer_stays_last():
    footer = "🤖 Generated with [Claude Code](https://claude.com/claude-code)"
    body = f"Depends on: #1\n\n- Change one.\n\n{footer}"
    out = _run(f"return [pullBatchBody({json.dumps(body)}, [12, 12, 13]), pullBatchBody('Plain', []), pullBatchBody('', [5])];")
    stacked, plain, bare = out
    assert stacked.startswith("Depends on: #12, #13\n\n")
    assert "#1\n" not in stacked
    assert stacked.count("Depends on:") == 1
    assert "commits already on the base branch are skipped" in stacked
    assert stacked.endswith(footer)
    assert plain == "Plain"
    assert bare.startswith("Depends on: #5\n\n") and "that pull request merges" in bare


def test_a_run_sends_in_order_and_names_each_parent_number():
    out = _run("""
      const calls = [];
      const batch = { repo, remote, rows: [row("pr/b", { dependsOn: ["pr/a"] }), row("pr/a"), row("pr/c", { dependsOn: ["pr/b"] })], sent: {}, outcome: {} };
      const sentNow = await runPullBatch(batch, { send: fakeSend(calls) });
      return { sentNow, calls, sent: batch.sent, stored: JSON.parse(localStorage.getItem(pullBatchStorageKey(repo, remote))) };
    """)
    assert out["sentNow"] == 3
    assert [call["head"] for call in out["calls"]] == ["pr/a", "pr/b", "pr/c"]
    assert all(call["base"] == "main" and call["target"].endswith("/acme-bolt/bolt") for call in out["calls"])
    assert [call["compareBase"] for call in out["calls"]] == ["", "pr/a", "pr/b"]
    assert not out["calls"][0]["body"].startswith("Depends on")
    assert out["calls"][1]["body"].startswith("Depends on: #41\n")
    assert out["calls"][2]["body"].startswith("Depends on: #42\n")
    assert {branch: value["number"] for branch, value in out["stored"].items()} == {"pr/a": 41, "pr/b": 42, "pr/c": 43}


def test_an_existing_target_pull_number_is_not_used_as_a_source_compare_ref():
    out = _run("""
      const calls = [];
      const batch = { repo, remote, rows: [row("pr/a", { dependsOn: ["#12"] })], sent: {}, outcome: {} };
      await runPullBatch(batch, { send: fakeSend(calls) });
      return calls;
    """)
    assert out[0]["compareBase"] == ""
    assert out[0]["body"].startswith("Depends on: #12\n")


def test_a_second_run_resumes_without_sending_anything_twice():
    out = _run("""
      const first = [];
      const rows = () => [row("pr/a"), row("pr/b", { dependsOn: ["pr/a"] }), row("pr/c")];
      await runPullBatch({ repo, remote, rows: rows(), sent: {}, outcome: {} }, { send: fakeSend(first, { "pr/b": "comparison_unavailable" }) });
      const second = [];
      const reloaded = { repo, remote, rows: rows(), sent: loadPullBatchSent(repo, remote), outcome: {} };
      await runPullBatch(reloaded, { send: fakeSend(second) });
      const third = [];
      await runPullBatch({ repo, remote, rows: rows(), sent: loadPullBatchSent(repo, remote), outcome: {} }, { send: fakeSend(third) });
      const otherTarget = { ...remote, url: "https://example.net/erin/bolt" };
      return { first: first.map((c) => c.head), second: second.map((c) => [c.head, c.body.split("\\n")[0]]), third: third.length, other: loadPullBatchSent(repo, otherTarget) };
    """)
    assert out["first"] == ["pr/a", "pr/b", "pr/c"]
    assert out["second"] == [["pr/b", "Depends on: #41"]]
    assert out["third"] == 0
    assert out["other"] == {}


def test_a_failure_holds_back_its_dependents_and_an_unreachable_target_stops_the_run():
    out = _run("""
      const calls = [];
      const batch = { repo, remote, rows: [row("pr/a"), row("pr/b", { dependsOn: ["pr/a"] }), row("pr/c")], sent: {}, outcome: {} };
      await runPullBatch(batch, { send: fakeSend(calls, { "pr/a": "pull_too_large" }) });
      const plan = pullBatchPlan(batch.rows, batch.sent, batch.outcome).plan;
      const stopCalls = [];
      const stopped = { repo, remote: { ...remote, url: "https://example.net/x/y" }, rows: [row("pr/a"), row("pr/c")], sent: {}, outcome: {} };
      await runPullBatch(stopped, { send: fakeSend(stopCalls, { "pr/a": "remote_unreachable" }) });
      return { calls: calls.map((c) => c.head), b: plan.get("pr/b"), a: plan.get("pr/a").state, stopCalls: stopCalls.map((c) => c.head), notice: stopped.notice };
    """)
    assert out["calls"] == ["pr/a", "pr/c"]
    assert out["a"] == "failed"
    assert out["b"]["state"] == "blocked" and "could not be sent" in out["b"]["reason"]
    assert out["stopCalls"] == ["pr/a"]
    assert out["notice"].startswith("Stopped after pr/a")


def test_a_branch_with_nothing_new_does_not_hold_back_its_dependents():
    out = _run("""
      const calls = [];
      const batch = { repo, remote, rows: [row("pr/a"), row("pr/b", { dependsOn: ["pr/a"] })], sent: {}, outcome: {} };
      await runPullBatch(batch, { send: fakeSend(calls, { "pr/a": "no_changes" }) });
      return { calls: calls.map((c) => [c.head, c.body]), stored: JSON.parse(localStorage.getItem(pullBatchStorageKey(repo, remote))) };
    """)
    assert out["calls"] == [["pr/a", "Body pr/a"], ["pr/b", "Body pr/b"]]
    assert list(out["stored"]) == ["pr/b"]


def test_stop_ends_the_run_after_the_pull_request_in_flight():
    out = _run("""
      const calls = [];
      const batch = { repo, remote, rows: [row("pr/a"), row("pr/b"), row("pr/c")], sent: {}, outcome: {} };
      const send = fakeSend(calls);
      await runPullBatch(batch, { send: async (...args) => { const r = await send(...args); batch.stopRequested = true; return r; } });
      return { calls: calls.map((c) => c.head), notice: batch.notice };
    """)
    assert out["calls"] == ["pr/a"]
    assert "Press Send all to continue" in out["notice"]


def test_rows_escape_titles_bodies_and_branches():
    out = _run("""
      const batch = { remote, running: false, sending: "" };
      const evil = row("pr/x", { title: '<img src=x onerror=alert(1)>', body: '</textarea><script>alert(1)</script>' });
      const step = pullBatchPlan([evil], {}).plan.get("pr/x");
      return pullBatchRowHtml(batch, evil, step, 0) + pullBatchStatusHtml(batch, evil, { state: "failed", reason: "<b>no</b>", waitsFor: [], numbers: [] });
    """)
    assert "<img" not in out and "<script>" not in out and "<b>no</b>" not in out
    assert "&lt;img src=x onerror=alert(1)&gt;" in out
    assert "&lt;/textarea&gt;" in out


def test_the_page_is_wired_into_the_pull_requests_tab():
    source = FRAGMENT.read_text(encoding="utf-8")
    assert source.count(" bg-cta ") == 1
    assert re.search(r'data-pull-batch-send class="[^"]*bg-cta', source)
    compose = (JS_DIR / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    assert 'data-repo-pull-batch-open href="${escapeHtml(pullBatchUrl(repo))}" class="hidden ' in compose
    assert "pullBatchRouteActive(repo)" in compose
    network = (JS_DIR / "08-repo-detail-network.js").read_text(encoding="utf-8")
    assert 'routeKind === "pulls" && routeParts[3] === "send"' in network
    assert 'kind === "pulls" && recordNumber === "send"' in network
    assert "void openPullBatch(state.selectedRepo);" in network
    content = (JS_DIR / "06-repo-content.js").read_text(encoding="utf-8")
    assert "syncRepoPullBatchEntry(repo);" in content
    assert "state.repoRemoteForks.canManage" in source
    assert "submitRemotePullOpen" in source

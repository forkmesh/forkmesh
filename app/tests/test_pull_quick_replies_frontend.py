"""Quick review replies in the merge box and the author's conflict banner."""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
CONTENT = (ROOT / "app/public/dashboard/js/06-repo-content.js").read_text(encoding="utf-8")
NETWORK = (ROOT / "app/public/dashboard/js/08-repo-detail-network.js").read_text(encoding="utf-8")
IO = (ROOT / "app/public/dashboard/js/03-issues-profile-io.js").read_text(encoding="utf-8")
SHELL = (ROOT / "app/public/dashboard/shell.html").read_text(encoding="utf-8")
BUNDLE = (ROOT / "app/public/dashboard.js").read_text(encoding="utf-8")
DOCS = (ROOT / "docs/app/pull-quick-replies.md").read_text(encoding="utf-8")

needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is required")

BASE = "2" * 40
HEAD = "3" * 40


def _function(name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), CONTENT)
    assert match, name
    end = CONTENT.index("\n  }\n", match.start())
    return CONTENT[match.start():end + 4]


def _between(start, end):
    first = CONTENT.index(start)
    return CONTENT[first:CONTENT.index(end, first)]


PRELUDE = r"""
const formatCount = (value) => String(Number(value) || 0);
const escapeHtml = (value) => String(value ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const publicSiteOrigin = () => "https://forkmesh.test";
const cloneUrl = () => "https://forkmesh.test/alice/widgets.git";
const repoKey = (repo) => `${repo.owner}/${repo.name}`;
const repoApiBase = () => "/api/repo/alice/widgets";
const repoRecordUrl = (repo, kind, number, section) => `/alice/widgets/pull/${number}${section ? `/${section}` : ""}`;
const repoPathUrl = (repo, kind, path) => `/alice/widgets/${kind}/${path}`;
const repoDefaultBranch = () => "main";
const blobText = (blob) => String(blob?.content || "");
const sessionOwnsRepo = (repo) => repo.owner === state.session?.nodeName;
const repoViewerPermissions = new Map();
const repoViewerPermissionKey = (repo) => `${state.session?.nodeName}|${repo.owner}/${repo.name}`;
let browserKey = "";
const webIssueKeyPublic = () => browserKey;
const storage = new Map();
globalThis.localStorage = {
  getItem: (key) => (storage.has(key) ? storage.get(key) : null),
  setItem: (key, value) => storage.set(key, String(value)),
};
globalThis.CSS = { escape: (value) => value };
globalThis.document = { querySelector: (selector) => (selector.includes('data-comment-id="body"') ? { selector } : null), activeElement: null };
globalThis.window = { setTimeout: () => 1, clearTimeout: () => {} };
globalThis.state = { session: { nodeName: "carol", sessionToken: "token" }, repoRecordDetail: null };
const world = { sent: [], posts: [], reply: null, panels: 0, local: [], blobs: {} };
async function submitWebPullEvent(repo, number, type, fields, delivery) {
  world.sent.push({ number, type, fields, delivery });
  if (world.reply) throw Object.assign(new Error(world.reply), { code: world.reply });
  return { ok: true, event: { author: "carol-key" } };
}
async function fetchRepoBlobs(repo, paths, options) {
  world.blobRequest = { paths, options };
  return world.blobs;
}
function applyRepoPullLocalEvent(repo, number, event) { world.local.push(event); }
function updateRepoPullMergePanel() { world.panels += 1; }
const pullDetailIsCurrent = () => true;
const pullAiModel = (values, body) => String(values?.aiModel || "") || (/AI model: (.+)/i.exec(String(body || ""))?.[1] || "");
const commentReactionSubject = (kind, number) => `pull:${number}`;
function sayCommentThanks(bar) { world.thanks = (world.thanks || 0) + 1; }
globalThis.fetch = async (url, init = {}) => {
  world.posts.push({ url, init });
  return { ok: true, json: async () => ({ ok: true, sent: world.serverSent || [] }) };
};
const REPO = { owner: "alice", name: "widgets" };
function pull(overrides = {}) {
  return {
    values: { status: "open", base: "main", head: "bob/leaderboard-api", derive: "branch", authorName: "bob", author: "bob-key", ...(overrides.values || {}) },
    pullReadiness: { available: true, baseOid: "BASE", headOid: "HEAD", clean: true, behind: 0, ahead: 1, conflicts: [], ...(overrides.readiness || {}) },
    pullChecks: overrides.checks || { available: true, runs: [] },
    pullConversation: overrides.conversation || [],
    pullMergeAuthorized: overrides.authorized ?? true,
    pullQuickReplies: overrides.config === undefined ? { source: "default", replies: PULL_QUICK_REPLY_DEFAULTS, contributingPath: "" } : overrides.config,
    repoCollabSettings: overrides.settings,
    pullQuickReplyUi: overrides.ui,
  };
}
const ids = (model) => (model ? model.items.map((item) => item.id) : null);
""".replace("BASE", BASE).replace("HEAD", HEAD)


def _harness():
    return "\n".join([
        PRELUDE,
        _function("immutableGitCommit"),
        _function("pullHeadBranchRef"),
        _between("  const PULL_CHECK_STATES = {", "  function renderRepoPullChecks("),
        _between("  const PULL_QUICK_REPLY_PATH = ", "  const repoPullQuickReplyPress = "),
    ])


def _run(body):
    script = "(async () => {\n" + _harness() + "\n" + body + "\n})().catch((error) => { console.error(error); process.exit(1); });"
    result = subprocess.run(["node", "-e", script], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@needs_node
def test_the_config_file_is_validated_and_missing_means_the_defaults():
    out = _run(r"""
const custom = parsePullQuickReplies(JSON.stringify({ replies: [
  { id: "docs", label: "Add docs", body: "Please document {head}.", emoji: "📚" },
  { id: "docs", label: "Duplicate", body: "x" },
  { id: "Bad id", label: "x", body: "x" },
  { id: "wip", label: "Not ready", body: "Marking as not ready.", state: "commented", when: "behind" },
  { id: "nope", label: "x", body: "x", state: "merged" },
  { id: "when", label: "x", body: "x", when: "sometimes" },
  { id: "empty", label: "", body: "x" },
] }));
const many = parsePullQuickReplies(JSON.stringify(Array.from({ length: 20 }, (_, index) => ({ id: `r${index}`, label: "L", body: "B" }))));
console.log(JSON.stringify({
  defaults: PULL_QUICK_REPLY_DEFAULTS.map((reply) => [reply.id, reply.state, reply.when, Boolean(reply.emoji)]),
  custom,
  bare: parsePullQuickReplies('[{"id":"lgtm","label":"LGTM","body":"Looks good","state":"approved"}]'),
  off: parsePullQuickReplies('{"replies": []}'),
  broken: parsePullQuickReplies("{not json"),
  shape: parsePullQuickReplies('{"other": 1}'),
  many: many.length,
}));
""")
    assert out["defaults"] == [
        ["conflict", "changes_requested", "conflict", True],
        ["behind", "changes_requested", "behind", True],
        ["checks", "changes_requested", "checks_failing", True],
        ["ai-model", "changes_requested", "ai_missing", True],
        ["tests", "changes_requested", "always", True],
        ["issue", "changes_requested", "always", True],
        ["split", "changes_requested", "always", True],
        ["screenshot", "changes_requested", "always", True],
        ["contributing", "changes_requested", "contributing", True],
        ["thanks", "approved", "always", True],
    ]
    assert out["custom"] == [
        {"id": "docs", "label": "Add docs", "body": "Please document {head}.", "state": "changes_requested", "when": "always", "emoji": "📚"},
        {"id": "wip", "label": "Not ready", "body": "Marking as not ready.", "state": "commented", "when": "behind", "emoji": ""},
    ]
    assert out["bare"] == [{"id": "lgtm", "label": "LGTM", "body": "Looks good", "state": "approved", "when": "always", "emoji": ""}]
    assert out["off"] == []
    assert out["broken"] is None and out["shape"] is None
    assert out["many"] == 12


@needs_node
def test_conditional_replies_follow_the_merge_trial_checks_and_settings():
    out = _run(r"""
const failed = { available: true, runs: [{ id: 1, workflow: "ci", status: "failed" }] };
console.log(JSON.stringify({
  ready: ids(repoPullQuickReplyModel(REPO, 7, pull())),
  conflict: ids(repoPullQuickReplyModel(REPO, 7, pull({ readiness: { clean: false, behind: 4, conflicts: ["a.py"] } }))),
  behind: ids(repoPullQuickReplyModel(REPO, 7, pull({ readiness: { behind: 4 } }))),
  unknown: ids(repoPullQuickReplyModel(REPO, 7, pull({ readiness: { available: false, clean: false } }))),
  checks: ids(repoPullQuickReplyModel(REPO, 7, pull({ checks: failed }))),
  ai: ids(repoPullQuickReplyModel(REPO, 7, pull({ settings: { requireAiDisclosure: true } }))),
  aiStated: ids(repoPullQuickReplyModel(REPO, 7, pull({ settings: { requireAiDisclosure: true }, values: { aiModel: "None" } }))),
  aiInBody: ids(repoPullQuickReplyModel(REPO, 7, { ...pull({ settings: { requireAiDisclosure: true } }), body: "AI model: Claude Opus 5.5" })),
  contributing: ids(repoPullQuickReplyModel(REPO, 7, pull({ config: { replies: PULL_QUICK_REPLY_DEFAULTS, contributingPath: ".github/CONTRIBUTING.md" } }))),
  disabled: repoPullQuickReplyModel(REPO, 7, pull({ config: { replies: [], contributingPath: "" } })),
  unloaded: repoPullQuickReplyModel(REPO, 7, pull({ config: null })),
}));
""")
    always = ["tests", "issue", "split", "screenshot", "thanks"]
    assert out["ready"] == always
    assert out["conflict"] == ["conflict"] + always
    assert out["behind"] == ["behind"] + always
    assert out["unknown"] == always
    assert out["checks"] == ["checks"] + always
    assert out["ai"] == ["ai-model"] + always
    assert out["aiStated"] == always
    assert out["aiInBody"] == always
    assert out["contributing"] == ["tests", "issue", "split", "screenshot", "contributing", "thanks"]
    assert out["disabled"] is None and out["unloaded"] is None


@needs_node
def test_conflict_commands_quote_hostile_branch_names():
    script = "\n".join([
        PRELUDE,
        _function("pullHeadBranchRef"),
        _function("pullShellWord"),
        _function("repoPullConflictCommands"),
        "console.log(JSON.stringify(repoPullConflictCommands(REPO, {derive: 'branch', base: 'main;id', head: 'bob:x;id'})));",
    ])
    result = subprocess.run(["node", "-e", script], capture_output=True,
                            text=True, check=True, timeout=60)
    assert json.loads(result.stdout)[1:3] == [
        "git switch 'x;id'", "git merge 'origin/main;id'"]


@needs_node
def test_only_reviewers_and_maintainers_who_are_not_the_author_see_them():
    out = _run(r"""
const who = {};
who.maintainer = Boolean(repoPullQuickReplyModel(REPO, 7, pull()));
who.visitor = Boolean(repoPullQuickReplyModel(REPO, 7, pull({ authorized: false })));
repoViewerPermissions.set("carol|alice/widgets", { permission: "write", viewerCanAdmin: false });
who.writer = Boolean(repoPullQuickReplyModel(REPO, 7, pull({ authorized: false })));
repoViewerPermissions.set("carol|alice/widgets", { permission: "read", viewerCanAdmin: false });
who.reader = Boolean(repoPullQuickReplyModel(REPO, 7, pull({ authorized: false })));
who.reviewer = Boolean(repoPullQuickReplyModel(REPO, 7, pull({ authorized: false, conversation: [{ type: "review", state: "approved", authorName: "Carol" }] })));
who.closed = Boolean(repoPullQuickReplyModel(REPO, 7, pull({ values: { status: "closed" } })));
who.author = Boolean(repoPullQuickReplyModel(REPO, 7, pull({ values: { authorName: "carol" } })));
browserKey = "bob-key";
who.authorKey = Boolean(repoPullQuickReplyModel(REPO, 7, pull()));
browserKey = "";
state.session = null;
who.signedOut = Boolean(repoPullQuickReplyModel(REPO, 7, pull()));
state.session = { nodeName: "alice", sessionToken: "t" };
who.owner = Boolean(repoPullQuickReplyModel(REPO, 7, pull({ authorized: false })));
console.log(JSON.stringify(who));
""")
    assert out == {
        "maintainer": True, "visitor": False, "writer": True, "reader": False,
        "reviewer": True, "closed": False, "author": False, "authorKey": False,
        "signedOut": False, "owner": True,
    }


@needs_node
def test_reply_text_names_the_branch_and_the_exact_commands():
    out = _run(r"""
const conflict = repoPullQuickReplyModel(REPO, 7, pull({ readiness: { clean: false } })).items[0];
const behind = repoPullQuickReplyModel(REPO, 7, pull({ readiness: { behind: 1 } })).items[0];
const patch = repoPullQuickReplyModel(REPO, 7, pull({ readiness: { clean: false }, values: { derive: "", head: "bob:fix", headOid: "HEAD" } })).items[0];
const checks = repoPullQuickReplyModel(REPO, 7, pull({ checks: { available: true, runs: [{ workflow: "ci", status: "failed" }] } })).items[0];
const guide = repoPullQuickReplyModel(REPO, 7, pull({ config: { replies: PULL_QUICK_REPLY_DEFAULTS, contributingPath: "CONTRIBUTING.md" } })).items.find((item) => item.id === "contributing");
console.log(JSON.stringify({ conflict: conflict.text, behind: behind.text, patch: patch.text, checks: checks.text, guide: guide.text,
  custom: repoPullQuickReplyText("Hi @{author}, #{number} {head} -> {base}", REPO, 7, pull()) }));
""".replace("HEAD", HEAD))
    assert out["conflict"] == (
        "Merge conflict with main — please rebase or merge main and push again: "
        "`git fetch origin && git switch bob/leaderboard-api && git merge origin/main`, "
        "fix any conflicts, commit and `git push`.")
    assert out["behind"].startswith("This branch is 1 commit behind main — please update it: `git fetch origin")
    assert "`git fetch https://forkmesh.test/alice/widgets.git main && git rebase FETCH_HEAD`" in out["patch"]
    assert out["patch"].endswith("submit the pull request again.")
    assert out["checks"] == "Please make the checks pass (see the Checks tab: https://forkmesh.test/alice/widgets/pull/7/checks)."
    assert out["guide"] == "Please follow the contributing guide: https://forkmesh.test/alice/widgets/blob/CONTRIBUTING.md"
    assert out["custom"] == "Hi @bob, #7 bob/leaderboard-api -> main"


@needs_node
def test_the_chips_are_labelled_buttons_with_an_edit_affordance_and_a_sent_state():
    out = _run(r"""
const parsed = pull({ readiness: { clean: false }, ui: { sent: { ["HEAD"]: ["tests"] } } });
const html = renderRepoPullQuickReplies(REPO, 7, parsed);
const editing = renderRepoPullQuickReplies(REPO, 7, pull({ ui: { editing: "split", draft: "Please split <this>" } }));
const visitor = renderRepoPullQuickReplies(REPO, 7, pull({ authorized: false }));
console.log(JSON.stringify({ html, editing, visitor }));
""".replace("HEAD", HEAD))
    html = out["html"]
    assert 'role="group" aria-labelledby="fm-merge-quick-title"' in html
    assert "Quick replies to @bob" in html
    assert 'data-repo-pull-quick-reply="conflict"' in html
    assert 'aria-label="Request changes: Merge conflict with main — please rebase or merge main and push again:' in html
    assert 'data-repo-pull-quick-reply-edit="split"' in html
    assert 'aria-label="Edit “Split it up” before sending"' in html
    assert 'data-repo-pull-quick-reply="tests"' not in html
    assert 'data-sent="true"' in html and "Sent</span>" in html and "disabled" in html
    assert 'aria-label="Approve: Thanks! Looks good."' in html
    assert 'data-state="approved"' in html
    assert 'role="status" aria-live="polite"' in html
    assert "press and hold" in html
    assert '<span class="fm-quick-reply-key" data-state="changes_requested">requests changes</span>' in html
    editing = out["editing"]
    assert 'data-repo-pull-quick-reply-form="split"' in editing
    assert '<label class="fm-quick-reply-form-label" for="fm-quick-reply-draft">' in editing
    assert "Please split &lt;this&gt;</textarea>" in editing
    assert "data-repo-pull-quick-reply-cancel" in editing
    assert out["visitor"] == ""


@needs_node
def test_one_tap_sends_a_signed_review_once_and_marks_it_sent():
    out = _run(r"""
const parsed = pull({ readiness: { clean: false } });
state.repoRecordDetail = { kind: "pulls", number: 7, parsed };
await sendRepoPullQuickReply(REPO, "conflict");
const first = world.sent[0];
const afterFirst = ids(repoPullQuickReplyModel(REPO, 7, parsed));
const sentChip = repoPullQuickReplyModel(REPO, 7, parsed).items.find((item) => item.id === "conflict").sent;
await sendRepoPullQuickReply(REPO, "conflict");
const repeat = world.sent.length;
await sendRepoPullQuickReply(REPO, "thanks");
const approve = world.sent[1];
await sendRepoPullQuickReply(REPO, "tests", "Please add a test for the empty leaderboard.");
const edited = world.sent[2];
world.reply = "quick_reply_sent";
await sendRepoPullQuickReply(REPO, "split");
const duplicateHint = parsed.pullQuickReplyUi.hint;
const splitSent = repoPullQuickReplyModel(REPO, 7, parsed).items.find((item) => item.id === "split").sent;
world.reply = "inbox_full";
await sendRepoPullQuickReply(REPO, "issue");
const errorHint = parsed.pullQuickReplyUi.hint;
const issueSent = repoPullQuickReplyModel(REPO, 7, parsed).items.find((item) => item.id === "issue").sent;
world.reply = null;
const fresh = pull({ readiness: { clean: false } });
state.repoRecordDetail = { kind: "pulls", number: 7, parsed: fresh };
const remembered = repoPullQuickReplyModel(REPO, 7, fresh).items.find((item) => item.id === "conflict").sent;
const otherHead = pull({ readiness: { clean: false, headOid: "f".repeat(40) } });
const otherHeadSent = repoPullQuickReplyModel(REPO, 7, otherHead).items.find((item) => item.id === "conflict").sent;
const custom = pull({ config: { replies: [{ id: "note", label: "Note", body: "Noted.", state: "commented", when: "always", emoji: "" }], contributingPath: "" } });
state.repoRecordDetail = { kind: "pulls", number: 7, parsed: custom };
await sendRepoPullQuickReply(REPO, "note");
console.log(JSON.stringify({ thanks: world.thanks || 0, first, afterFirst, sentChip, repeat, approve, edited, duplicateHint, splitSent, errorHint, issueSent, remembered, otherHeadSent,
  comment: world.sent[world.sent.length - 1], local: world.local, stored: Object.keys(JSON.parse(storage.get(PULL_QUICK_REPLY_SENT_KEY))) }));
""")
    first = out["first"]
    assert first["number"] == 7 and first["type"] == "review"
    assert first["fields"]["state"] == "changes_requested"
    assert first["fields"]["body"].startswith("Merge conflict with main — please rebase or merge main and push again: `git fetch origin")
    assert first["delivery"] == {"relayOnly": True, "extra": {"quickReply": {"id": "conflict", "head": HEAD}}}
    assert out["sentChip"] is True and out["repeat"] == 1
    assert out["approve"]["fields"] == {"state": "approved", "body": "Thanks! Looks good."}
    assert out["thanks"] == 1
    assert out["edited"]["fields"] == {"state": "changes_requested", "body": "Please add a test for the empty leaderboard."}
    assert out["duplicateHint"]["text"] == "You already sent that reply on this commit."
    assert out["splitSent"] is True
    assert out["errorHint"] == {"tone": "bad", "text": "The maintainer's inbox is full. Try again later."}
    assert out["issueSent"] is False
    assert out["remembered"] is True and out["otherHeadSent"] is False
    assert out["comment"]["type"] == "comment" and out["comment"]["fields"] == {"body": "Noted."}
    assert [event["type"] for event in out["local"]] == ["review", "review", "review", "comment"]
    assert out["local"][0]["state"] == "changes_requested" and out["local"][1]["state"] == "approved"
    assert "alice/widgets#7@%s:conflict" % HEAD in out["stored"]


@needs_node
def test_the_author_sees_a_conflict_banner_with_the_commands():
    out = _run(r"""
const conflict = { readiness: { clean: false, conflicts: ["a.py", "b.py", "c.py", "d.py", "e.py", "f.py"] } };
state.session = { nodeName: "bob", sessionToken: "t" };
const author = renderRepoPullConflictBanner(REPO, 7, pull(conflict));
const clean = renderRepoPullConflictBanner(REPO, 7, pull());
const closed = renderRepoPullConflictBanner(REPO, 7, pull({ ...conflict, values: { status: "closed" } }));
const patch = renderRepoPullConflictBanner(REPO, 7, pull({ ...conflict, values: { derive: "", head: "bob:fix" } }));
state.session = { nodeName: "carol", sessionToken: "t" };
const reviewer = renderRepoPullConflictBanner(REPO, 7, pull(conflict));
console.log(JSON.stringify({ author, clean, closed, patch, reviewer }));
""")
    author = out["author"]
    assert 'aria-labelledby="fm-conflict-banner-title"' in author
    assert "Your pull request has conflicts with main</h2>" in author
    assert "Rebase or merge main into bob/leaderboard-api and push again." in author
    assert "git fetch origin\ngit switch bob/leaderboard-api\ngit merge origin/main" in author
    assert 'data-dashboard-copy="git fetch origin\ngit switch bob/leaderboard-api\ngit merge origin/main\ngit add -A &amp;&amp; git commit\ngit push"' in author
    assert "<code>e.py</code><span>and 1 more</span>" in author
    assert 'tabindex="0" aria-label="Commands to resolve the conflicts"' in author
    assert "Rebase your change onto the newest main and submit it again." in out["patch"]
    assert "git rebase FETCH_HEAD" in out["patch"]
    assert out["clean"] == "" and out["closed"] == "" and out["reviewer"] == ""


@needs_node
def test_the_page_reports_each_merge_trial_once_and_reads_sent_replies():
    out = _run(r"""
reportRepoPullMergeability(REPO, 7, pull({ readiness: { clean: false } }));
reportRepoPullMergeability(REPO, 7, pull({ readiness: { clean: false } }));
reportRepoPullMergeability({ ...REPO, isPrivate: true }, 8, pull());
reportRepoPullMergeability(REPO, 9, pull({ readiness: { available: false } }));
const reports = world.posts.slice();
world.serverSent = ["tests"];
world.blobs = { ".forkmesh/review-replies.json": { content: '{"replies":[{"id":"tests","label":"Tests","body":"Add tests"}]}' }, "docs/CONTRIBUTING.md": { content: "x" } };
const parsed = pull({ config: undefined });
delete parsed.pullQuickReplies;
await hydrateRepoPullAttention(REPO, 10, parsed);
console.log(JSON.stringify({ reports, config: parsed.pullQuickReplies, blobRequest: world.blobRequest, sent: parsed.pullQuickReplyUi.sent, last: world.posts[world.posts.length - 1] }));
""")
    reports = out["reports"]
    assert len(reports) == 1
    assert reports[0]["url"] == "/api/repo/alice/widgets/pulls/7/attention"
    body = json.loads(reports[0]["init"]["body"])
    assert body == {"baseOid": BASE, "headOid": HEAD, "clean": False, "base": "main",
                    "head": "bob/leaderboard-api", "derive": "branch"}
    assert reports[0]["init"]["headers"]["authorization"] == "Bearer token"
    assert out["config"]["source"] == "repository"
    assert out["config"]["contributingPath"] == "docs/CONTRIBUTING.md"
    assert out["config"]["replies"][0]["id"] == "tests"
    assert out["blobRequest"] == {"paths": [".forkmesh/review-replies.json", "CONTRIBUTING.md", ".github/CONTRIBUTING.md", "docs/CONTRIBUTING.md"], "options": {"ref": BASE}}
    assert out["sent"] == {HEAD: ["tests"]}
    assert out["last"]["url"] == "/api/repo/alice/widgets/pulls/10/attention?head=" + HEAD


def test_the_page_wires_the_controls_and_the_bundle_is_current():
    for marker in ("[data-repo-pull-quick-reply-edit]", "[data-repo-pull-quick-reply-cancel]",
                   "[data-repo-pull-quick-reply]", "[data-repo-pull-quick-reply-form]",
                   "[data-repo-pull-quick-reply-draft]", "consumeRepoPullQuickReplyLongPress(replyId)",
                   'document.addEventListener("pointerdown"', 'document.addEventListener("contextmenu"',
                   'event.key === "Escape"', "requestSubmit()"):
        assert marker in NETWORK, marker
    assert "delivery.relayOnly ? { relayOnly: true } : {}" in IO
    assert "...(delivery.extra || {})" in IO
    assert "options.relayOnly ? [] : directRepoRequestCandidates" in CONTENT
    assert "${model.open ? renderRepoPullQuickReplies(repo, number, parsed) : \"\"}" in CONTENT
    assert '<div data-repo-pull-conflict-slot class="fm-conflict-slot">' in CONTENT
    assert CONTENT.count("void hydrateRepoPullAttention(repo, number, parsed);") == 2
    assert 'if (kind === "pulls") void hydrateRepoPullAttention(repo, number, parsed);' in CONTENT
    for selector in (".fm-merge-quick {", ".fm-quick-reply {", ".fm-quick-reply-edit {",
                     ".fm-quick-reply button:focus-visible", ".fm-conflict-slot:empty { display: none; }",
                     ".fm-conflict-commands {", "min-height: 32px", "-webkit-touch-callout: none"):
        assert selector in SHELL, selector
    for name in ("function renderRepoPullQuickReplies(", "function renderRepoPullConflictBanner(",
                 "function sendRepoPullQuickReply(", "function hydrateRepoPullAttention("):
        assert name in BUNDLE, name
    assert "\u2028" not in CONTENT and "\u2029" not in CONTENT


def test_the_doc_describes_the_file_the_defaults_and_the_conflict_pings():
    for text in (".forkmesh/review-replies.json", "changes_requested", "checks_failing",
                 "ai_missing", "contributing", "quick_reply_sent", "pull_merge_watch",
                 "pull_attention", "pull-attention", "/attention"):
        assert text in DOCS, text

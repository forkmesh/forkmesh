import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js
from test_event_repo_binding import EV, _run, _Signer


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
PULLS_CPP = (REPO / "desktop" / "src" / "MainWindowPulls.cpp").read_text(encoding="utf-8")
PULL_STORE = (REPO / "desktop" / "src" / "PullStore.cpp").read_text(encoding="utf-8")
KEYS = ("alice/project",)


def _revision(signer, number, ts=1790000500, patch="diff --git a/x b/x\n"):
    rev = {"title": "Fix", "base": "main", "head": "carol:fix-2", "patch": patch,
           "commits": "", "author": signer.pub, "ts": ts}
    content = "\x00".join([rev["title"], rev["base"], rev["head"], rev["patch"], rev["commits"]])
    body = "%s\n%d\n%s" % (signer.pub, ts, hashlib.sha256(content.encode()).hexdigest())
    rev["sig"] = signer.sign("forkmesh-pull-event-v2\nalice/project\n" + body)
    rev["revisionSig"] = signer.sign(
        "forkmesh-pull-revision-v2\nalice/project\n"
        + EV["pull_revision_binding_body"](number, rev))
    return rev


def test_a_revision_verifies_only_for_the_number_it_binds():
    signer = _Signer()
    verify = EV["verify_pull_revision"]
    rev = _revision(signer, 12)
    assert _run(verify(12, rev, KEYS, False)) is True
    assert _run(verify(13, rev, KEYS, False)) is False
    assert _run(verify(12, rev, ("alice/other",), False)) is False


def test_a_revision_needs_both_signatures_intact():
    signer = _Signer()
    verify = EV["verify_pull_revision"]
    rev = _revision(signer, 12)
    assert _run(verify(12, dict(rev, patch="tampered"), KEYS, False)) is False
    assert _run(verify(12, dict(rev, revisionSig=""), KEYS, False)) is False
    other = _revision(signer, 12, ts=1790000600)
    assert _run(verify(12, dict(rev, revisionSig=other["revisionSig"]), KEYS, False)) is False


def test_the_binding_names_number_author_ts_and_pull_signature():
    rev = {"author": "A", "ts": 7, "sig": "S"}
    assert EV["pull_revision_binding_body"](3, rev) == "3\nA\n7\nS"


def test_relay_only_accepts_a_newer_revision_from_the_author_of_an_open_patch_pull():
    block = ENTRY.split("async def _accept_pull_revision", 1)[1].split("\nasync def ", 1)[0]
    for needle in ("verify_pull_revision(", "_pull_lifecycle_record(",
                   '"pull_not_open"', '"pull_follows_branch"', '"not_authorized"',
                   '"revision_stale"', "_bind_session_author(", "MAX_PULL_BYTES",
                   '"pull-revision:"'):
        assert needle in block, needle
    assert '"type": "revision"' in block
    assert 'revision = data.get("revision")' in ENTRY


def test_web_signs_the_same_binding_and_offers_the_update_only_to_the_author():
    assert 'const binding = `${Number(number)}\\n${revision.author}\\n${revision.ts}\\n${revision.sig}`;' in DASHBOARD_JS
    assert 'webEventSigningPrefix("pull-revision", repo)' in DASHBOARD_JS
    assert "webSessionAuthored({ author: values.author, authorName: values.authorName })" in DASHBOARD_JS
    assert '${renderPullRevisionPanel(number, parsed)}' in DASHBOARD_JS
    assert "const descriptionChanged = typeof description === \"string\";" in DASHBOARD_JS
    assert "&& !descriptionChanged) throw new Error(\"no_new_commits\");" in DASHBOARD_JS
    assert 'data-repo-pull-description-edit' in DASHBOARD_JS
    assert 'data-repo-pull-description-form' in DASHBOARD_JS


def test_desktop_drains_a_revision_before_treating_the_item_as_an_event():
    drain = PULLS_CPP.split("MainWindow::applyPullsInboxEntries", 1)[1]
    assert drain.index('obj.value("revision").isObject()') < drain.index('obj.contains("event")')
    assert "store.applyRemoteRevision(" in drain
    apply = PULL_STORE.split("bool PullStore::applyRemoteRevision", 1)[1].split("\nQString PullStore::", 1)[0]
    for needle in ("revision.ts <= current.ts", "revision.author != current.author",
                   "/revisions/%1", "next.headOid.clear();", "materializePullRef(next"):
        assert needle in apply, needle


def _js_function(name):
    match = re.search(r"\n  (?:async )?function " + name + r"\(", DASHBOARD_JS)
    start = match.start()
    cursor = DASHBOARD_JS.index("{", DASHBOARD_JS.index(")", start))
    depth = 0
    while True:
        if DASHBOARD_JS[cursor] == "{":
            depth += 1
        elif DASHBOARD_JS[cursor] == "}":
            depth -= 1
            if depth == 0:
                return DASHBOARD_JS[start:cursor + 1]
        cursor += 1


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_browser_revision_verifies_on_the_relay():
    script = _revision_harness("""
(async () => {
  const repo = { owner: "alice", name: "project" };
  await submitWebPullRevision(repo, 12, { head: "carol:fix", base: "main", title: "Fix", creationHeadOid: "c".repeat(40) }, "fix-2");
  process.stdout.write(JSON.stringify(sent));
})();
""")
    result = subprocess.run(["node"], input=script, capture_output=True, text=True, check=True)
    sent = json.loads(result.stdout)
    assert sent["number"] == 12 and sent["revision"]["head"] == "carol:fix-2"
    assert sent["revision"]["creationHeadOid"] == "b" * 40
    assert _run(EV["verify_pull_revision"](12, sent["revision"], KEYS, False)) is True
    assert _run(EV["verify_pull_revision"](11, sent["revision"], KEYS, False)) is False


def _revision_harness(body):
    names = ("bytesToB64url", "sha256HexLower", "webEventSigningPrefix", "getWebIssueKey",
             "webIssueKeyPublic", "webSessionAuthored", "signWebPullContent",
             "pullRevisionSource", "pullParentBaseOid", "fetchPullComparison",
             "submitWebPullRevision")
    return """
const ISSUE_TEXT_ENCODER = new TextEncoder();
const WEB_ISSUE_KEY_STORAGE = "k";
const store = {};
const localStorage = { getItem: (k) => store[k] || null, setItem: (k, v) => { store[k] = v; } };
const state = { session: { nodeName: "carol" } };
const repoLiveUrl = (repo, op, params) => ({ repo, op, params });
let sent = null;
async function fetchRepoJson(url) {
  return { patch: "diff --git a/x b/x\\n", commits: "", baseOid: "a".repeat(40), headOid: "b".repeat(40) };
}
async function fetchRepoMutation(repo, kind, payload) { sent = payload; return { ok: true }; }
%s
%s
""" % ("\n".join(_js_function(name) for name in names), body)


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_same_head_on_a_new_base_is_a_new_revision():
    script = _revision_harness("""
const repo = { owner: "alice", name: "project" };
const values = { head: "carol:fix", base: "main", title: "Fix", creationHeadOid: "b".repeat(40) };
(async () => {
  const outcomes = [];
  for (const creationBaseOid of ["a".repeat(40), "e".repeat(40)]) {
    sent = null;
    try {
      await submitWebPullRevision(repo, 12, { ...values, creationBaseOid }, "fix");
      outcomes.push(sent ? "sent" : "none");
    } catch (error) {
      outcomes.push(error.message);
    }
  }
  process.stdout.write(JSON.stringify(outcomes));
})();
""")
    result = subprocess.run(["node"], input=script, capture_output=True, text=True, check=True)
    assert json.loads(result.stdout) == ["no_new_commits", "sent"]


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_a_description_edit_is_a_signed_revision_without_new_commits():
    script = _revision_harness("""
const repo = { owner: "alice", name: "project" };
(async () => {
  await submitWebPullRevision(repo, 12, {
    head: "carol:fix", base: "main", title: "Fix",
    creationBaseOid: "a".repeat(40), creationHeadOid: "b".repeat(40),
  }, "fix", "Clarify the change.");
  process.stdout.write(JSON.stringify(sent));
})();
""")
    result = subprocess.run(["node"], input=script, capture_output=True, text=True, check=True)
    sent = json.loads(result.stdout)
    assert sent["revision"]["description"] == "Clarify the change."
    assert _run(EV["verify_pull_revision"](12, sent["revision"], KEYS, False)) is True


def _fork_revision_compare(branches_js, compare_js):
    names = ("pullParentBaseOid", "fetchPullComparison")
    script = """
const repoLiveUrl = (repo, op, params) => ({ repo, op, params });
const asked = [];
%s
%s
%s
(async () => {
  const comparison = await fetchPullComparison(
    { owner: "carol", name: "project" }, "main", "fix-2", { owner: "alice", name: "project" });
  process.stdout.write(JSON.stringify({ asked, comparison }));
})();
""" % (branches_js, compare_js, "\n".join(_js_function(name) for name in names))
    result = subprocess.run(["node"], input=script, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_a_fork_pull_compares_against_the_parent_base_tip():
    tip = "d" * 40
    out = _fork_revision_compare(
        """async function fetchRepoBranches(repo, options) {
  asked.push(["branches", repo.owner, Boolean(options && options.refresh)]);
  return [{ name: "dev", commit: "e".repeat(40) }, { name: "main", commit: "%s" }];
}""" % tip.upper(),
        """async function fetchRepoJson(url) {
  asked.push(["compare", url.repo.owner, url.params.base, url.params.head]);
  return { baseOid: url.params.base };
}""")
    assert out["asked"] == [["branches", "alice", True], ["compare", "carol", tip, "fix-2"]]
    assert out["comparison"]["baseOid"] == tip


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_a_fork_pull_falls_back_to_its_own_base_when_the_parent_tip_is_not_there():
    out = _fork_revision_compare(
        """async function fetchRepoBranches(repo) { return [{ name: "main", commit: "d".repeat(40) }]; }""",
        """async function fetchRepoJson(url) {
  asked.push(["compare", url.params.base]);
  if (url.params.base !== "main") throw new Error("not_found");
  return { baseOid: "a".repeat(40) };
}""")
    assert out["asked"] == [["compare", "d" * 40], ["compare", "main"]]
    assert out["comparison"]["baseOid"] == "a" * 40


def test_opening_a_fork_pull_compares_against_the_parent():
    assert 'signWebPull(repo, targetRepo, title, body, base, head, signedHead, "", parent ? targetRepo : null)' in DASHBOARD_JS
    assert "fetchPullComparison(sourceRepo, base, branch, source.owner ? repo : null)" in DASHBOARD_JS

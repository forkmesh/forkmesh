import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js
from test_event_repo_binding import EV, _run, _Signer


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
PULL_STORE = (REPO / "desktop" / "src" / "PullStore.cpp").read_text(encoding="utf-8")
MAIN_WINDOW_PULLS = (REPO / "desktop" / "src" / "MainWindowPulls.cpp").read_text(encoding="utf-8")
INTAKE_GO = (REPO / "server" / "intake.go").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()

sys.path.insert(0, str(REPO / "tools"))
import mirror_gateway  # noqa: E402
import ssh_gateway  # noqa: E402


def _signed(signer, number, event, repo="alice/project"):
    ev = dict(event, author=signer.pub, ts=1790000000)
    content = hashlib.sha256(EV["pull_comment_content"](ev).encode()).hexdigest()
    body = "%s\n%d\n%s\n%d\n%s" % (ev["type"], number, signer.pub, ev["ts"], content)
    ev["sig"] = signer.sign("forkmesh-pull-comment-v2\n" + repo + "\n" + body)
    return ev


def test_comment_edit_signs_target_and_body():
    signer = _Signer()
    verify = EV["verify_pull_comment_event"]
    ev = _signed(signer, 7, {"type": "comment-edit", "target": "c1", "body": "fixed"})
    assert EV["pull_comment_content"](ev) == "c1\x00fixed"
    assert _run(verify(7, ev, ("alice/project",), False)) is True
    assert _run(verify(7, dict(ev, target="c2"), ("alice/project",), False)) is False
    assert _run(verify(7, dict(ev, body="other"), ("alice/project",), False)) is False


def test_comment_delete_signs_its_target_and_requires_one():
    signer = _Signer()
    verify = EV["verify_pull_comment_event"]
    ev = _signed(signer, 7, {"type": "comment-delete", "target": "c1"})
    assert EV["pull_comment_content"](ev) == "c1"
    assert _run(verify(7, ev, ("alice/project",), False)) is True
    assert _run(verify(7, dict(ev, target="c9"), ("alice/project",), False)) is False
    blank = _signed(signer, 7, {"type": "comment-delete", "target": ""})
    assert _run(verify(7, blank, ("alice/project",), False)) is False


def test_gateways_hash_the_same_bytes_as_the_relay():
    for event in (
        {"type": "comment-edit", "target": "c1", "body": "fixed"},
        {"type": "comment-delete", "target": "c1", "body": ""},
    ):
        relay = EV["pull_comment_content"](event)
        assert ssh_gateway._pull_comment_content(event) == relay
        assert mirror_gateway._dashboard_pull_event_content(event) == relay
    assert "comment-edit" in ssh_gateway.PULL_COMMENT_EVENT_TYPES
    assert "comment-delete" in ssh_gateway.PULL_COMMENT_EVENT_TYPES
    assert mirror_gateway._dashboard_pull_event_content(
        {"type": "comment-delete", "target": ""}) is None


def test_desktop_signs_the_same_content_and_writes_the_target():
    assert 'if (ev.type == "comment-edit")\n        return ev.target + nul + ev.body;' in PULL_STORE
    assert 'if (ev.type == "comment-delete")\n        return ev.target;' in PULL_STORE
    assert 'lines << "target: " + ev.target;' in PULL_STORE
    assert 'ev.target = fm.get("target");' in PULL_STORE
    assert "PullStore::foldCommentChanges(pr.events, &editedComments)" in MAIN_WINDOW_PULLS


def test_go_intake_keeps_the_target_in_the_event_file():
    keys = re.search(r'for _, key := range \[\]string\{([^}]*)\}', INTAKE_GO.split(
        "func pullEventMarkdown", 1)[1]).group(1)
    assert '"target"' in keys


def test_relay_sends_no_notifications_for_comment_changes():
    block = ENTRY.split('if event.get("type") in ("comment-edit", "comment-delete"):', 1)[1]
    head = block.split("await _record_public_contributor(", 1)[0]
    assert "notify_repo_host" in head and "return json_response" in head
    assert "notify_mentions" not in head and "notify_subscribers" not in head


def test_relay_lets_an_issue_comment_author_edit_their_own_comment():
    assert "_issue_comment_owned_by_submitter(\n                env, owner, repo, number, event)" in ENTRY
    helper = ENTRY.split("async def _issue_comment_owned_by_submitter", 1)[1].split("\nasync def ", 1)[0]
    assert 'target == "self"' in helper
    assert 'event.get("authorVerified")' in helper


def _js_function(name):
    match = re.search(r"\n  function " + name + r"\(.*?\n  }\n", DASHBOARD_JS, re.S)
    assert match, name
    return match.group(0)


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_web_fold_applies_only_the_authors_edits_and_deletes():
    constant = re.search(r"\n  const PULL_EDITABLE_COMMENT_TYPES = new Set\(\[.*?\]\);\n",
                         DASHBOARD_JS, re.S).group(0)
    script = constant + "".join(_js_function(name) for name in (
        "pullTimestamp", "pullCommentChangeApplies", "foldPullCommentChanges")) + """
const events = [
  { id: "c1", type: "comment", author: "A", authorName: "ann", ts: 1, body: "first" },
  { id: "c2", type: "comment", author: "B", authorName: "bob", ts: 2, body: "second" },
  { id: "r1", type: "review", author: "B", authorName: "bob", ts: 3, state: "approved", body: "ok" },
  { id: "e1", type: "comment-edit", author: "A", authorName: "ann", ts: 4, target: "c1", body: "first, fixed" },
  { id: "e2", type: "comment-edit", author: "M", authorName: "mallory", ts: 5, target: "c2", body: "hijacked" },
  { id: "d1", type: "comment-delete", author: "X", authorName: "BOB", ts: 6, target: "c2" },
  { id: "d2", type: "comment-delete", author: "B", authorName: "bob", ts: 7, target: "r1" },
];
process.stdout.write(JSON.stringify(foldPullCommentChanges(events)));
"""
    result = subprocess.run(["node"], input=script, capture_output=True, text=True, check=True)
    folded = json.loads(result.stdout)
    assert [ev["id"] for ev in folded] == ["c1", "r1"]
    assert folded[0]["body"] == "first, fixed" and folded[0]["_edited"] is True
    assert folded[1]["body"] == "ok"


def test_web_pr_comments_offer_edit_and_delete_on_own_comments():
    assert "data-repo-pull-comment-edit=" in DASHBOARD_JS
    assert "data-repo-pull-comment-delete=" in DASHBOARD_JS
    assert 'handleWebPullCommentAction("edit"' in DASHBOARD_JS
    assert 'handleWebPullCommentAction("delete"' in DASHBOARD_JS
    assert 'content = [fields.target, cleanBody].join(NUL);' in DASHBOARD_JS
    assert "Boolean(options.canManage) || webSessionAuthored(rawEvent)" in DASHBOARD_JS

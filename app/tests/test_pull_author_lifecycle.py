#!/usr/bin/env python3

import ast
import asyncio
import hashlib
import importlib.util
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT.parent
_spec = importlib.util.spec_from_file_location(
    "event_repo_binding_harness", HERE / "test_event_repo_binding.py")
HARNESS = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(HARNESS)
EV = HARNESS.EV
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
ISSUE_IO = (ROOT / "public/dashboard/js/03-issues-profile-io.js").read_text()
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
sys.path.insert(0, str(REPO / "tools"))
import mirror_gateway as gateway  # noqa: E402

AUTHOR_KEY = "A" * 43
STRANGER_KEY = "B" * 43


def _signed(signer, ev, number, repo="alice/project"):
    ev = dict(ev, author=signer.pub, ts=1790000000000)
    content = hashlib.sha256(EV["pull_comment_content"](ev).encode()).hexdigest()
    body = "%s\n%d\n%s\n%d\n%s" % (ev["type"], number, signer.pub, ev["ts"], content)
    ev["sig"] = signer.sign("forkmesh-pull-comment-v2\n" + repo + "\n" + body)
    return ev


def test_lifecycle_events_sign_their_state_like_every_client():
    content = EV["pull_comment_content"]
    assert content({"type": "status", "state": "closed", "body": ""}) == "closed\x00"
    assert content({"type": "delete", "body": "bye"}) == "bye"
    assert gateway._dashboard_pull_event_content(
        {"type": "status", "state": "closed", "body": ""}) == "closed\x00"
    assert gateway._dashboard_pull_event_content({"type": "delete", "body": "bye"}) == "bye"
    assert gateway._dashboard_pull_event_content(
        {"type": "status", "state": "open", "body": ""}) is None
    assert 'type === "status"' in ISSUE_IO and 'type === "delete"' in ISSUE_IO
    assert "submitWebPullClose" in ISSUE_IO and "submitWebPullDelete" in ISSUE_IO


def test_relay_verifies_close_and_delete_but_never_a_reopen():
    signer = HARNESS._Signer()
    verify = EV["verify_pull_comment_event"]
    home = ("alice/project",)
    close = _signed(signer, {"type": "status", "state": "closed", "body": ""}, 4)
    delete = _signed(signer, {"type": "delete", "body": ""}, 4)
    reopen = _signed(signer, {"type": "status", "state": "open", "body": ""}, 4)
    assert HARNESS._run(verify(4, close, home)) is True
    assert HARNESS._run(verify(4, delete, home)) is True
    assert HARNESS._run(verify(5, close, home)) is False
    assert HARNESS._run(verify(4, reopen, home)) is False


def _entry_function(name, extra_globals):
    tree = ast.parse(ENTRY, filename="entry.py")
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name
    ]
    assert len(selected) == 1, name
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, "entry.py", "exec"), namespace)
    return namespace[name]


def _gate(session_name, record, manager=False, key_accounts=None):
    key_accounts = key_accounts or {}

    async def pull_record(_env, _owner, _repo, _number):
        return dict(record)

    async def session(_env, _request, _data):
        return ("bi", {"name": session_name}) if session_name else ("", None)

    async def issue_manager(_env, _owner, _data, _request):
        return (True, None) if manager else (False, ("refused",))

    async def push_account(_env, key):
        return key_accounts.get(key, "")

    return _entry_function("_authorize_pull_lifecycle", {
        "_pull_lifecycle_record": pull_record,
        "_account_session_record": session,
        "_authorize_issue_manager": issue_manager,
        "_git_push_account_for_pubkey": push_account,
        "valid_node_pubkey": lambda value: len(str(value or "")) == 43,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 64,
        "PULL_LIFECYCLE_FINAL_STATUSES": frozenset(
            {"merged", "closed-merged", "deleted"}),
        "json_response": lambda body, status=200: (status, body),
    })


def _decide(gate, event):
    allowed, refusal, actor = asyncio.run(
        gate(None, "alice", "project", 4, {}, None, event))
    return allowed, refusal, actor


OPEN_PULL = {"number": "4", "status": "open", "author": AUTHOR_KEY,
             "authorName": "bob"}


def test_the_pull_author_may_close_and_delete_their_own_pull():
    for event in ({"type": "status", "state": "closed"}, {"type": "delete"}):
        allowed, refusal, actor = _decide(_gate("bob", OPEN_PULL), event)
        assert allowed is True and refusal is None and actor == "bob"
    allowed, _, _ = _decide(
        _gate("", OPEN_PULL), {"type": "delete", "author": AUTHOR_KEY})
    assert allowed is True
    renamed = dict(OPEN_PULL, authorName="")
    allowed, _, _ = _decide(
        _gate("bob", renamed, key_accounts={AUTHOR_KEY: "Bob"}),
        {"type": "status", "state": "closed"})
    assert allowed is True


def test_maintainers_keep_their_rights():
    allowed, _, actor = _decide(
        _gate("alice", OPEN_PULL, manager=True), {"type": "delete"})
    assert allowed is True and actor == "alice"


def test_strangers_are_refused():
    for session, event in (
        ("mallory", {"type": "delete", "author": STRANGER_KEY}),
        ("mallory", {"type": "status", "state": "closed"}),
        ("", {"type": "delete", "author": STRANGER_KEY}),
    ):
        allowed, refusal, _ = _decide(
            _gate(session, OPEN_PULL, key_accounts={STRANGER_KEY: "mallory"}),
            event)
        assert allowed is False
        assert refusal == (403, {"error": "not_authorized"})


def test_merged_or_unreadable_pulls_are_never_closed_or_deleted():
    merged = dict(OPEN_PULL, status="merged")
    allowed, refusal, _ = _decide(_gate("bob", merged, manager=True), {"type": "delete"})
    assert allowed is False and refusal == (409, {"error": "pull_final"})
    allowed, refusal, _ = _decide(_gate("bob", {}), {"type": "delete"})
    assert allowed is False and refusal == (503, {"error": "pull_unavailable"})


def test_pulls_route_runs_the_lifecycle_gate_before_queueing():
    handler = ENTRY[ENTRY.index("async def pulls_handler"):]
    handler = handler[:handler.index("\nasync def ")]
    numbered = handler[handler.index("verify_pull_comment_event("):]
    gate = numbered.index("_authorize_pull_lifecycle(")
    assert gate < numbered.index("INSERT INTO pull_inbox")
    assert 'event["authorName"] = lifecycle_actor' in numbered


class _Repository:
    def __init__(self, record):
        self.record = record

    def blob(self, query):
        if self.record is None or query["ref"] != "refs/heads/forkmesh/pulls":
            raise gateway.GatewayError("repository path was not found")
        lines = ["---"] + ["%s: %s" % item for item in self.record.items()] + ["---", ""]
        return {"ok": True, "encoding": "utf8", "content": "\n".join(lines)}


def test_mirror_intake_applies_the_same_rule():
    repository = _Repository(OPEN_PULL)
    refuse = gateway._pull_lifecycle_refusal
    assert refuse(repository, 4, {"type": "delete"}, "alice", "bob", True) is None
    assert refuse(repository, 4, {"type": "delete"}, "alice", "alice", True) is None
    assert refuse(repository, 4, {"type": "delete", "author": AUTHOR_KEY},
                  "alice", "", False) is None
    assert refuse(repository, 4, {"type": "delete"}, "alice", "mallory", True) == (
        "not_authorized", 403)
    assert refuse(repository, 4, {"type": "delete"}, "alice", "bob", False) == (
        "not_authorized", 403)
    assert refuse(_Repository(dict(OPEN_PULL, status="merged")), 4,
                  {"type": "delete"}, "alice", "alice", True) == ("pull_final", 409)
    assert refuse(_Repository(None), 4, {"type": "delete"}, "alice", "bob", True) == (
        "pull_unavailable", 503)


def test_web_page_offers_close_and_delete_to_the_author_and_maintainers():
    gate = REPO_UI[REPO_UI.index("function pullLifecycleAllowed("):]
    gate = gate[:gate.index("\n  }\n")]
    assert "values.authorName" in gate and "pullMergeAuthorized" in gate
    assert '"open" && status !== "closed"' in gate
    assert 'data-repo-pull-review-action="close"' in REPO_UI
    assert 'data-repo-pull-review-action="delete"' in REPO_UI
    assert 'action === "close" || action === "delete"' in REPO_UI
    buttons = REPO_UI[REPO_UI.index("function renderPullLifecycleButtons("):]
    buttons = buttons[:buttons.index("\n  }\n")]
    assert "text-destructive" in buttons and "bg-cta" not in buttons

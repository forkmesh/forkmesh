#!/usr/bin/env python3

import hashlib
import importlib.util
from pathlib import Path


HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "event_repo_binding_harness", HERE / "test_event_repo_binding.py")
HARNESS = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(HARNESS)
EV = HARNESS.EV
ENTRY = HARNESS.ENTRY


def _thread_comment():
    return {
        "type": "thread-comment", "id": "thread-comment-ios-1",
        "threadId": "thread-comment-ios-1", "path": "src/app.txt",
        "side": "new", "lineStart": 3, "lineEnd": 5, "body": "Rename these",
        "suggestionPatch": "", "patchset": "",
        "anchorLines": "alpha\nbeta\ngamma",
    }


def _signed(signer, ev, branch, prefix="forkmesh-branch-comment-v2\nalice/project\n"):
    ev = dict(ev, author=signer.pub, ts=1790000000000)
    content = hashlib.sha256(EV["pull_comment_content"](ev).encode()).hexdigest()
    body = "%s\n%s\n%s\n%d\n%s" % (ev["type"], branch, signer.pub, ev["ts"], content)
    ev["sig"] = signer.sign(prefix + body)
    return ev


def test_branch_comment_content_matches_the_shared_vectors():
    content = EV["pull_comment_content"]
    assert hashlib.sha256(content(_thread_comment()).encode()).hexdigest() == (
        "02d13720c8a1e971d05805561c7378eba454f51998fd5c8f447bf6569efb64ce")
    reply = {"type": "thread-reply", "threadId": "thread-comment-ios-1",
             "parentId": "thread-comment-ios-1", "body": "Done"}
    assert hashlib.sha256(content(reply).encode()).hexdigest() == (
        "d9c05bb661d5f22bce769db007e4fa6831fb49282b9e041ead75f52ef517468a")
    state = {"type": "thread-state", "threadId": "thread-comment-ios-1",
             "state": "resolved", "body": ""}
    assert hashlib.sha256(content(state).encode()).hexdigest() == (
        "ce3bf1a2fd60719b415f48680b213edf965f71ecc6154233b02dc3c82c6c2c5f")


def test_branch_comment_binds_the_repository_and_the_branch():
    signer = HARNESS._Signer()
    verify = EV["verify_branch_comment_event"]
    ev = _signed(signer, _thread_comment(), "feature/review")
    home = ("alice/project",)
    assert HARNESS._run(verify("feature/review", ev, home)) is True
    assert HARNESS._run(verify("feature/other", ev, home)) is False
    assert HARNESS._run(verify("feature/review", ev, ("mallory/other",))) is False
    assert HARNESS._run(verify("feature/review", ev, ())) is False


def test_branch_comment_refuses_v1_signatures_and_pull_only_types():
    signer = HARNESS._Signer()
    verify = EV["verify_branch_comment_event"]
    home = ("alice/project",)
    v1 = _signed(signer, _thread_comment(), "feature/review",
                 prefix="forkmesh-branch-comment-v1\n")
    assert HARNESS._run(verify("feature/review", v1, home)) is False
    comment = _signed(signer, {"type": "comment", "body": "hi"}, "feature/review")
    assert HARNESS._run(verify("feature/review", comment, home)) is False
    pull_signed = HARNESS._pull_comment(signer, 3, 2)
    assert HARNESS._run(verify("3", pull_signed, home)) is False


def test_branch_names_that_git_or_paths_would_misread_are_refused():
    valid = EV["valid_branch_name"]
    assert valid("feature/review") is True
    assert valid("1319-diff-view-allow-click") is True
    for bad in ("", "-rf", "/abs", "a/", "a..b", "a//b", "x.lock", "a@{1}",
                "a b", "a\nb", "a:b", "a\x00b", "x" * 256, None, 7):
        assert valid(bad) is False, repr(bad)


def test_pulls_route_accepts_signed_branch_events_into_the_pull_inbox():
    entry = ENTRY.read_text(encoding="utf-8")
    handler = entry[entry.index("async def pulls_handler"):]
    handler = handler[:handler.index("\nasync def ")]
    branch = handler[handler.index('branch = data.get("branch")'):]
    branch = branch[:branch.index("if isinstance(event, dict):\n")]
    assert "verify_branch_comment_event(" in branch
    assert "_event_signing_repo_keys(request, owner, repo)" in branch
    assert '"branch": branch' in branch
    assert "INSERT INTO pull_inbox" in branch
    assert 'status=201' in branch
    assert 'verify_branch_comment_event = _events.export(' in entry

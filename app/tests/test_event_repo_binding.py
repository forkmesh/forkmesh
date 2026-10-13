#!/usr/bin/env python3

import ast
import asyncio
import base64
import hashlib
import time
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


ROOT = Path(__file__).resolve().parents[1]
EVENTS = ROOT / "src" / "events.py"
ENTRY = ROOT / "src" / "entry.py"
JS_BOUND = {"to_js", "_to_js_bytes", "ed25519_verify", "sha256_hex",
            "verify_release_manifest"}


def _b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _b64url_decode(value):
    value = value + "=" * (-len(value) % 4)
    return base64.urlsafe_b64decode(value.encode())


async def _ed25519_verify(pub, sig, data):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PublicKey,
    )
    try:
        Ed25519PublicKey.from_public_bytes(_b64url_decode(pub)).verify(
            _b64url_decode(sig), data)
        return True
    except Exception:
        return False


async def _sha256_hex(text):
    return hashlib.sha256(text.encode()).hexdigest()


def _load_events():
    tree = ast.parse(EVENTS.read_text(encoding="utf-8"), filename=str(EVENTS))
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name not in JS_BOUND)
        or isinstance(node, ast.Assign)
    ]
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {
        "time": time,
        "ed25519_verify": _ed25519_verify,
        "sha256_hex": _sha256_hex,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    exec(compile(module, str(EVENTS), "exec"), namespace)
    return namespace


EV = _load_events()


class _Signer:
    def __init__(self):
        self.key = Ed25519PrivateKey.generate()
        self.pub = _b64url(self.key.public_key().public_bytes(
            Encoding.Raw, PublicFormat.Raw))

    def sign(self, message):
        return _b64url(self.key.sign(message.encode()))


def _issue_comment(signer, number, version, repo="alice/project"):
    ev = {"type": "comment", "author": signer.pub, "ts": 1790000000000,
          "body": "looks good", "attachments": []}
    content = hashlib.sha256(EV["issue_event_content"](ev).encode()).hexdigest()
    body = "comment\n%d\n%s\n%d\n%s" % (number, signer.pub, ev["ts"], content)
    if version == 2:
        message = "forkmesh-issue-event-v2\n" + repo + "\n" + body
    else:
        message = "forkmesh-issue-event-v1\n" + body
    ev["sig"] = signer.sign(message)
    return ev


def _pull_comment(signer, number, version, repo="alice/project"):
    ev = {"type": "comment", "author": signer.pub, "ts": 1790000000000,
          "body": "please rebase"}
    content = hashlib.sha256(EV["pull_comment_content"](ev).encode()).hexdigest()
    body = "comment\n%d\n%s\n%d\n%s" % (number, signer.pub, ev["ts"], content)
    if version == 2:
        message = "forkmesh-pull-comment-v2\n" + repo + "\n" + body
    else:
        message = "forkmesh-pull-comment-v1\n" + body
    ev["sig"] = signer.sign(message)
    return ev


def _discussion_comment(signer, number, version, repo="alice/project"):
    ev = {"type": "comment", "author": signer.pub, "ts": 1790000000000,
          "body": "agreed"}
    content = hashlib.sha256(
        EV["discussion_event_content"](ev).encode()).hexdigest()
    body = "comment\n%d\n%s\n%d\n%s" % (number, signer.pub, ev["ts"], content)
    if version == 2:
        message = "forkmesh-discussion-event-v2\n" + repo + "\n" + body
    else:
        message = "forkmesh-discussion-event-v1\n" + body
    ev["sig"] = signer.sign(message)
    return ev


def _pull(signer, version, repo="alice/project"):
    pr = {"title": "Fix", "base": "main", "head": "fix", "patch": "diff",
          "commits": "mbox", "author": signer.pub, "ts": 1790000000000}
    content = hashlib.sha256("\x00".join(
        [pr["title"], pr["base"], pr["head"], pr["patch"], pr["commits"]]
    ).encode()).hexdigest()
    body = "%s\n%d\n%s" % (signer.pub, pr["ts"], content)
    if version == 2:
        message = "forkmesh-pull-event-v2\n" + repo + "\n" + body
    else:
        message = "forkmesh-pull-event-v1\n" + body
    pr["sig"] = signer.sign(message)
    return pr


def _run(coro):
    return asyncio.run(coro)


def test_repo_key_is_lowercase_owner_slash_name_and_rejects_separators():
    assert EV["event_signing_repo_key"]("Alice", "Project") == "alice/project"
    assert EV["event_signing_repo_key"]("a/b", "c") == ""
    assert EV["event_signing_repo_key"]("a", "b\nc") == ""
    assert EV["event_signing_repo_key"]("", "c") == ""
    assert EV["event_signing_repo_keys"](
        ("Acme", "Tool"), ("jett", "tool"), ("acme", "tool"), ("", "x"),
    ) == ("acme/tool", "jett/tool")


def test_v1_intake_window_closes_at_the_cutoff():
    cutoff = EV["EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS"]
    assert EV["v1_event_intake_open"](cutoff - 1) is True
    assert EV["v1_event_intake_open"](cutoff) is False


def test_v2_issue_event_is_accepted_only_for_the_repository_it_names():
    signer = _Signer()
    ev = _issue_comment(signer, 5, 2)
    verify = EV["verify_issue_event"]
    assert _run(verify(5, ev, ("alice/project",), False)) is True
    assert _run(verify(5, ev, ("mallory/other",), False)) is False
    assert _run(verify(5, ev, ("mallory/other",), True)) is False
    assert _run(verify(5, ev, (), True)) is False


def test_cross_repo_replay_is_refused_for_every_event_kind_in_v2():
    signer = _Signer()
    other = ("mallory/other",)
    home = ("alice/project",)
    cases = [
        (EV["verify_issue_event"], (5, _issue_comment(signer, 5, 2))),
        (EV["verify_pull_comment_event"], (3, _pull_comment(signer, 3, 2))),
        (EV["verify_discussion_event"], (2, _discussion_comment(signer, 2, 2))),
        (EV["verify_pull_event"], (_pull(signer, 2),)),
    ]
    for verify, args in cases:
        assert _run(verify(*args, home, False)) is True, verify.__name__
        assert _run(verify(*args, other, True)) is False, verify.__name__


def test_v1_events_are_accepted_only_while_the_intake_window_is_open():
    signer = _Signer()
    other = ("mallory/other",)
    cases = [
        (EV["verify_issue_event"], (5, _issue_comment(signer, 5, 1))),
        (EV["verify_pull_comment_event"], (3, _pull_comment(signer, 3, 1))),
        (EV["verify_discussion_event"], (2, _discussion_comment(signer, 2, 1))),
        (EV["verify_pull_event"], (_pull(signer, 1),)),
    ]
    for verify, args in cases:
        assert _run(verify(*args, other, True)) is True, verify.__name__
        assert _run(verify(*args, other, False)) is False, verify.__name__


def test_v1_default_follows_the_clock():
    signer = _Signer()
    ev = _issue_comment(signer, 5, 1)
    original = EV["EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS"]
    try:
        EV["EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS"] = 0
        assert _run(EV["verify_issue_event"](5, ev, ("alice/project",))) is False
        EV["EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS"] = int(time.time() * 1000) + 60000
        assert _run(EV["verify_issue_event"](5, ev, ("alice/project",))) is True
    finally:
        EV["EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS"] = original


def test_v2_signature_does_not_verify_under_another_number_or_as_v1():
    signer = _Signer()
    ev = _issue_comment(signer, 5, 2)
    assert _run(EV["verify_issue_event"](6, ev, ("alice/project",), True)) is False


def test_worker_inbox_routes_pass_the_route_repository_to_every_verifier():
    entry = ENTRY.read_text(encoding="utf-8")
    helper = entry[entry.index("def _event_signing_repo_keys"):]
    helper = helper[:helper.index("\nasync def ")]
    assert "REPO_API_PREFIX_RE.match" in helper
    assert "event_signing_repo_keys((public_owner, repo), (owner, repo))" in helper
    for call in ("verify_issue_event(", "verify_pull_comment_event(",
                 "verify_pull_event(", "verify_discussion_event("):
        start = entry.index("if not await " + call)
        assert "_event_signing_repo_keys(request, owner, repo)" in entry[start:start + 200]
    microjobs = (ROOT / "src" / "microjobs_runtime.py").read_text(encoding="utf-8")
    start = microjobs.index("if not await verify_issue_event(")
    assert "_event_signing_repo_keys(self.request, owner, repo)" in microjobs[start:start + 200]

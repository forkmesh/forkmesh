#!/usr/bin/env python3
"""Key-signed owner cloud agent sessions from the desktop (adhoc #259).

The desktop Agents list shows the cloud runs its owner queued on a mirror, but
a desktop that launched normally authenticated silently: it holds the account's
Ed25519 key and no session token. These checks pin how narrow the signed form
of /api/repo/{owner}/{repo}/agent-sessions is:

  * one proof prefix per operation, and no proof stands in for another —
    including the account-wide list, which reads every repository at once;
  * the canonical binds the exact path, so a proof for one repository or
    session never works against a different one;
  * a follow-up binds the digest of its prompt and may carry nothing else;
  * queueing new work binds the digest of its provider, model, pull request
    choice, sending device and prompt, and may carry nothing else;
  * the signer travels as `user`, and a triple sent as `node` is not read;
  * once a triple is on the URL it is the only credential considered.

AST-extraction harness in the style of test_org_admin_signed_writes.py.

Run: python3 -m pytest app/tests/test_owner_agent_signed_sessions.py
"""

import ast
import asyncio
import hashlib
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ROUTES = ROOT / "src" / "route_handlers.py"
URLS = ROOT / "src" / "urls.py"
CATALOG = ROOT / "src" / "catalog.py"
ROUTES_TEXT = ROUTES.read_text(encoding="utf-8")
SOURCE_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + "\n"
    + ROUTES_TEXT
    + "\n"
    + CATALOG.read_text(encoding="utf-8")
    + "\n"
    + URLS.read_text(encoding="utf-8")
)

FUNCS = {
    "_owner_agent_signed_triple",
    "_owner_agent_signed_session",
    "_signed_account_session",
    "_account_email_unverified",
    "_owner_agent_queue_text",
    "_owner_agent_branch_text",
    "clean_string",
}
CONSTANTS = {
    "OWNER_AGENT_LIST_PROOF",
    "OWNER_AGENT_ACCOUNT_LIST_PROOF",
    "OWNER_AGENT_READ_PROOF",
    "OWNER_AGENT_PROMPT_PROOF",
    "OWNER_AGENT_DELETE_PROOF",
    "OWNER_AGENT_STOP_PROOF",
    "OWNER_AGENT_QUEUE_PROOF",
    "OWNER_AGENT_QUEUE_KEYS",
    "OWNER_AGENT_BRANCH_PROOF",
    "OWNER_AGENT_BRANCH_ACTIONS",
    "OWNER_AGENT_BRANCH_KEYS",
    "REPO_OWNER_AGENT_SESSION_DIFF_RE",
    "REPO_OWNER_AGENT_SESSIONS_RE",
    "REPO_OWNER_AGENT_SESSION_RE",
    "ACCOUNT_AGENT_SESSIONS_RE",
    "ORG_AGENT_BOTS_RE",
    "ORG_AGENT_BOT_RE",
    "REPO_OWNER_AGENT_SESSION_STREAM_RE",
    "ORG_AGENT_BOT_STREAM_RE",
    "REPO_OWNER_AGENT_SESSION_ATTACHMENT_RE",
    "ORG_AGENT_BOT_ATTACHMENT_RE",
    "UNVERIFIED_SAFE_METHODS",
    "UNVERIFIED_EMAIL_ERROR",
}

LIST_PATH = "/api/repo/alice/project/agent-sessions"
ITEM_PATH = LIST_PATH + "/s1"
ACCOUNT_PATH = "/api/account/agent-sessions"
ORG_LIST_PATH = "/api/orgs/acme/repos/project/agent-bots"
ORG_ITEM_PATH = ORG_LIST_PATH + "/s1"
IMAGE = "/attachments/" + "a" * 32


def _load(extra):
    tree = ast.parse(SOURCE_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCS
        ):
            selected.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in CONSTANTS
            for target in node.targets
        ):
            selected.append(node)
    found = set()
    for node in selected:
        found.add(node.name if hasattr(node, "name") else node.targets[0].id)
    assert found == FUNCS | CONSTANTS, \
        "missing: %s" % sorted((FUNCS | CONSTANTS) - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Request:
    def __init__(self, method, url):
        self.method = method
        self.url = url


def _fake_sig(pubkey, canonical):
    return "sig-" + pubkey + "-" + hashlib.sha256(canonical).hexdigest()[:32]


def _harness(accounts, ts_ok=True):
    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        record = accounts.get(key)
        if record is None:
            return "bi:" + key, None
        record = dict(record)
        record.setdefault("name", key)
        return "bi:" + key, record

    async def _account_signing_pubkeys(_env, _account_bi, record):
        return [record["pubkey"]] + list(record.get("devices") or [])

    async def ed25519_verify(pubkey, sig, canonical):
        return sig == _fake_sig(pubkey, canonical)

    return _load({
        "re": re,
        "hashlib": hashlib,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "MAX_NODE_NAME": 63,
        "method_name": lambda request: request.method,
        "_ts_ok": lambda _ts: ts_ok,
        "_account_row": _account_row,
        "_account_kind": lambda record: record.get("kind", "user"),
        "_account_signing_pubkeys": _account_signing_pubkeys,
        "ed25519_verify": ed25519_verify,
    })


ACCOUNTS = {
    "alice": {"pubkey": "pk-alice", "status": "active", "devices": ["pk-lap"]},
    "pending": {
        "pubkey": "pk-pending", "status": "active",
        "email": "p@example.com", "email_verified": False,
    },
}


def _canonical(ns, proof, user, path, ts="1700", prompt=None):
    text = ns[proof] + "\n" + user + "\n" + path + "\n"
    if prompt is not None:
        text += hashlib.sha256(prompt.encode("utf-8")).hexdigest() + "\n"
    return (text + ts).encode()


def _url(path, user, sig, ts="1700", name="user"):
    return "https://app.forkmesh.com%s?%s=%s&ts=%s&sig=%s" % (
        path, name, user, ts, sig)


def _resolve(ns, method, url, data=None):
    return asyncio.run(ns["_owner_agent_signed_session"](
        object(), _Request(method, url), data))


def test_list_read_and_delete_resolve_only_for_their_own_proof_and_path():
    ns = _harness(ACCOUNTS)
    cases = (
        ("GET", LIST_PATH, "OWNER_AGENT_LIST_PROOF"),
        ("GET", ITEM_PATH, "OWNER_AGENT_READ_PROOF"),
        ("DELETE", ITEM_PATH, "OWNER_AGENT_DELETE_PROOF"),
    )
    for method, path, proof in cases:
        sig = _fake_sig("pk-alice", _canonical(ns, proof, "alice", path))
        account_bi, record = _resolve(ns, method, _url(path, "alice", sig))
        assert account_bi == "bi:alice" and record["name"] == "alice"
        # The same signature never authorizes a different verb or resource.
        for other_method, other_path, _ in cases:
            if (other_method, other_path) == (method, path):
                continue
            assert _resolve(
                ns, other_method, _url(other_path, "alice", sig)) == ("", None)
        assert _resolve(ns, method, _url(
            path.replace("project", "other"), "alice", sig)) == ("", None)


def test_the_account_wide_list_has_its_own_proof():
    # The account list reads every repository the account owns in one request,
    # so a repository list proof must not answer for it and it must not answer
    # for a repository (adhoc #404).
    ns = _harness(ACCOUNTS)
    sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_ACCOUNT_LIST_PROOF", "alice", ACCOUNT_PATH))
    assert _resolve(ns, "GET", _url(ACCOUNT_PATH, "alice", sig))[0] == "bi:alice"
    assert _resolve(ns, "GET", _url(LIST_PATH, "alice", sig)) == ("", None)
    repo_sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_LIST_PROOF", "alice", ACCOUNT_PATH))
    assert _resolve(ns, "GET", _url(ACCOUNT_PATH, "alice", repo_sig)) == ("", None)
    # Reading is all it is: no verb but GET resolves on that path.
    for method in ("POST", "DELETE"):
        assert _resolve(ns, method, _url(ACCOUNT_PATH, "alice", sig),
                        {"prompt": "x"}) == ("", None)


def test_an_organization_session_takes_the_same_proofs_on_its_own_path():
    ns = _harness(ACCOUNTS)
    cases = (
        ("GET", ORG_LIST_PATH, "OWNER_AGENT_LIST_PROOF", None),
        ("GET", ORG_ITEM_PATH, "OWNER_AGENT_READ_PROOF", None),
        ("DELETE", ORG_ITEM_PATH, "OWNER_AGENT_DELETE_PROOF", None),
        ("POST", ORG_ITEM_PATH, "OWNER_AGENT_PROMPT_PROOF", "carry on"),
    )
    for method, path, proof, prompt in cases:
        sig = _fake_sig("pk-alice", _canonical(
            ns, proof, "alice", path, prompt=prompt))
        data = {"prompt": prompt} if prompt is not None else None
        assert _resolve(
            ns, method, _url(path, "alice", sig), data)[0] == "bi:alice"
        owner_path = path.replace(
            "/api/orgs/acme/repos/project/agent-bots",
            "/api/repo/acme/project/agent-sessions")
        assert _resolve(
            ns, method, _url(owner_path, "alice", sig), data) == ("", None)
    sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_QUEUE_PROOF", "alice", ORG_LIST_PATH))
    assert _resolve(ns, "POST", _url(ORG_LIST_PATH, "alice", sig),
                    {"prompt": "x"}) == ("", None)


def test_the_live_stream_reads_with_the_read_proof_on_its_own_path():
    ns = _harness(ACCOUNTS)
    for item in (ITEM_PATH, ORG_ITEM_PATH):
        path = item + "/stream"
        sig = _fake_sig("pk-alice", _canonical(
            ns, "OWNER_AGENT_READ_PROOF", "alice", path))
        assert _resolve(ns, "GET", _url(path, "alice", sig))[0] == "bi:alice"
        assert _resolve(ns, "GET", _url(item, "alice", sig)) == ("", None)
        assert _resolve(ns, "POST", _url(path, "alice", sig),
                        {"prompt": "x"}) == ("", None)
        item_sig = _fake_sig("pk-alice", _canonical(
            ns, "OWNER_AGENT_READ_PROOF", "alice", item))
        assert _resolve(ns, "GET", _url(path, "alice", item_sig)) == ("", None)
        for proof in ("OWNER_AGENT_LIST_PROOF", "OWNER_AGENT_DELETE_PROOF"):
            other = _fake_sig("pk-alice", _canonical(ns, proof, "alice", path))
            assert _resolve(ns, "GET", _url(path, "alice", other)) == ("", None)


def test_a_prompt_image_is_read_with_a_read_proof_for_its_own_path():
    ns = _harness(ACCOUNTS)
    for item in (ITEM_PATH, ORG_ITEM_PATH):
        path = item + IMAGE
        sig = _fake_sig("pk-alice", _canonical(
            ns, "OWNER_AGENT_READ_PROOF", "alice", path))
        assert _resolve(ns, "GET", _url(path, "alice", sig))[0] == "bi:alice"
        for method in ("POST", "DELETE"):
            assert _resolve(ns, method, _url(path, "alice", sig),
                            {"prompt": "x"}) == ("", None)
        assert _resolve(ns, "GET", _url(item, "alice", sig)) == ("", None)
        session_sig = _fake_sig("pk-alice", _canonical(
            ns, "OWNER_AGENT_READ_PROOF", "alice", item))
        assert _resolve(
            ns, "GET", _url(path, "alice", session_sig)) == ("", None)


def test_a_registered_device_key_signs_as_the_account():
    ns = _harness(ACCOUNTS)
    sig = _fake_sig("pk-lap", _canonical(
        ns, "OWNER_AGENT_LIST_PROOF", "alice", LIST_PATH))
    assert _resolve(ns, "GET", _url(LIST_PATH, "alice", sig))[0] == "bi:alice"


def test_follow_up_binds_the_prompt_and_nothing_else_may_ride_along():
    ns = _harness(ACCOUNTS)
    prompt = "also fix the tests\nand rebase"
    sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_PROMPT_PROOF", "alice", ITEM_PATH, prompt=prompt))
    url = _url(ITEM_PATH, "alice", sig)
    assert _resolve(ns, "POST", url, {"prompt": prompt})[0] == "bi:alice"
    assert _resolve(ns, "POST", url, {"prompt": "rm -rf"}) == ("", None)
    assert _resolve(
        ns, "POST", url, {"prompt": prompt, "sessionToken": "x"}) == ("", None)
    assert _resolve(ns, "POST", url, {"prompt": 7}) == ("", None)
    assert _resolve(ns, "POST", url, None) == ("", None)


def test_stop_has_its_own_proof_on_an_owner_session_only():
    ns = _harness(ACCOUNTS)
    stop = {"action": "stop"}
    sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_STOP_PROOF", "alice", ITEM_PATH))
    url = _url(ITEM_PATH, "alice", sig)
    assert _resolve(ns, "POST", url, stop)[0] == "bi:alice"
    for body in (
        {"action": "merge"}, {"action": "cleanup"}, {"action": "STOP"},
        {"action": "stop", "prompt": "x"}, {"action": "stop", "node": "n"},
        {"prompt": "stop"}, None,
    ):
        assert _resolve(ns, "POST", url, body) == ("", None)
    assert _resolve(ns, "DELETE", url) == ("", None)
    assert _resolve(ns, "GET", url) == ("", None)
    assert _resolve(ns, "POST", _url(
        ITEM_PATH.replace("s1", "s2"), "alice", sig), stop) == ("", None)
    for proof in ("OWNER_AGENT_PROMPT_PROOF", "OWNER_AGENT_DELETE_PROOF",
                  "OWNER_AGENT_READ_PROOF"):
        other = _fake_sig("pk-alice", _canonical(ns, proof, "alice", ITEM_PATH))
        assert _resolve(ns, "POST", _url(ITEM_PATH, "alice", other),
                        stop) == ("", None)
    prompt_sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_PROMPT_PROOF", "alice", ITEM_PATH, prompt="stop"))
    assert _resolve(ns, "POST", _url(ITEM_PATH, "alice", prompt_sig),
                    stop) == ("", None)
    org_sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_STOP_PROOF", "alice", ORG_ITEM_PATH))
    assert _resolve(ns, "POST", _url(ORG_ITEM_PATH, "alice", org_sig),
                    stop) == ("", None)
    for path in (LIST_PATH, ITEM_PATH + IMAGE):
        other = _fake_sig("pk-alice", _canonical(
            ns, "OWNER_AGENT_STOP_PROOF", "alice", path))
        assert _resolve(ns, "POST", _url(path, "alice", other),
                        stop) == ("", None)


def _branch_text(body):
    def flag(key):
        return "" if key not in body else "1" if body[key] else "0"
    return "\n".join((
        body["action"], flag("cleanup"), flag("discard"),
        body.get("requestId", ""),
        str(body["number"]) if "number" in body else ""))


def _branch_sig(ns, path, body, user="alice", pubkey="pk-alice"):
    text = (ns["OWNER_AGENT_BRANCH_PROOF"] + "\n" + user + "\n" + path
            + "\n" + hashlib.sha256(
                _branch_text(body).encode("utf-8")).hexdigest() + "\n1700")
    return _fake_sig(pubkey, text.encode())


def test_merge_cleanup_and_pull_link_bind_their_whole_body():
    ns = _harness(ACCOUNTS)
    bodies = (
        {"action": "merge", "cleanup": True, "requestId": "ab-123456789012"},
        {"action": "merge", "cleanup": False},
        {"action": "cleanup", "requestId": "ab-123456789012"},
        {"action": "cleanup", "discard": True, "requestId": "r" * 20},
        {"action": "link-pull", "number": 42},
    )
    for body in bodies:
        url = _url(ITEM_PATH, "alice", _branch_sig(ns, ITEM_PATH, body))
        assert _resolve(ns, "POST", url, body)[0] == "bi:alice"
        for changed in (
            dict(body, action="cleanup" if body["action"] != "cleanup"
                 else "merge"),
            dict(body, requestId="ab-999999999999"),
            dict(body, number=43),
            dict(body, discard=not body.get("discard", False)),
            dict(body, prompt="x"),
        ):
            if changed != body:
                assert _resolve(ns, "POST", url, changed) == ("", None)
    merge = bodies[0]
    sig = _branch_sig(ns, ITEM_PATH, merge)
    assert _resolve(ns, "POST", _url(
        ITEM_PATH.replace("s1", "s2"), "alice", sig), merge) == ("", None)
    org_sig = _branch_sig(ns, ORG_ITEM_PATH, merge)
    assert _resolve(ns, "POST", _url(ORG_ITEM_PATH, "alice", org_sig),
                    merge) == ("", None)
    stop_sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_STOP_PROOF", "alice", ITEM_PATH))
    assert _resolve(ns, "POST", _url(ITEM_PATH, "alice", stop_sig),
                    merge) == ("", None)
    for body in (
        {"action": "delete"}, {"action": "link-pull", "number": "42"},
        {"action": "link-pull", "number": True},
        {"action": "merge", "cleanup": "yes"},
        {"action": "merge", "requestId": 5},
    ):
        bad = _url(ITEM_PATH, "alice", _branch_sig(ns, ITEM_PATH, {
            "action": "merge"}))
        assert _resolve(ns, "POST", bad, body) == ("", None)


def test_the_branch_diff_is_read_with_a_read_proof_for_its_own_path():
    ns = _harness(ACCOUNTS)
    diff = ITEM_PATH + "/diff"
    sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_READ_PROOF", "alice", diff))
    url = _url(diff, "alice", sig) + "&page=1&pageSize=25"
    assert _resolve(ns, "GET", url)[0] == "bi:alice"
    assert _resolve(ns, "POST", url, {}) == ("", None)
    item_sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_READ_PROOF", "alice", ITEM_PATH))
    assert _resolve(ns, "GET", _url(diff, "alice", item_sig)) == ("", None)


def test_an_unconfirmed_address_cannot_merge():
    ns = _harness(ACCOUNTS)
    item = "/api/repo/pending/project/agent-sessions/s1"
    body = {"action": "merge", "cleanup": True}
    sig = _branch_sig(ns, item, body, "pending", "pk-pending")
    with pytest.raises(RuntimeError):
        _resolve(ns, "POST", _url(item, "pending", sig), body)


def test_an_unconfirmed_address_cannot_stop_a_run():
    ns = _harness(ACCOUNTS)
    item = "/api/repo/pending/project/agent-sessions/s1"
    sig = _fake_sig("pk-pending", _canonical(
        ns, "OWNER_AGENT_STOP_PROOF", "pending", item))
    with pytest.raises(RuntimeError):
        _resolve(ns, "POST", _url(item, "pending", sig), {"action": "stop"})


def _queue_text(body):
    return "\n".join((
        body["provider"], body["model"], "1" if body["createPr"] else "0",
        body["fromDevice"], body["prompt"]))


def test_queueing_new_work_binds_the_whole_request():
    ns = _harness(ACCOUNTS)
    body = {
        "prompt": "fix the flaky test",
        "provider": "claude-code",
        "model": "claude-opus-5-5",
        "createPr": False,
        "fromDevice": "alice-laptop",
    }
    sig = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_QUEUE_PROOF", "alice", LIST_PATH,
        prompt=_queue_text(body)))
    url = _url(LIST_PATH, "alice", sig)
    assert _resolve(ns, "POST", url, body)[0] == "bi:alice"
    for key, value in (
        ("prompt", "rm -rf"), ("provider", "codex"), ("model", ""),
        ("createPr", True), ("fromDevice", "other"),
    ):
        assert _resolve(ns, "POST", url, {**body, key: value}) == ("", None)
    assert _resolve(ns, "POST", url, {**body, "targetNode": "x"}) == ("", None)
    assert _resolve(ns, "POST", url, {**body, "createPr": "no"}) == ("", None)
    missing = dict(body)
    missing.pop("fromDevice")
    assert _resolve(ns, "POST", url, missing) == ("", None)
    assert _resolve(ns, "POST", _url(
        LIST_PATH.replace("project", "other"), "alice", sig), body) == ("", None)
    for proof in ("OWNER_AGENT_LIST_PROOF", "OWNER_AGENT_PROMPT_PROOF"):
        other = _fake_sig("pk-alice", _canonical(
            ns, proof, "alice", LIST_PATH, prompt=_queue_text(body)))
        assert _resolve(ns, "POST", _url(LIST_PATH, "alice", other),
                        body) == ("", None)
    queue_on_item = _fake_sig("pk-alice", _canonical(
        ns, "OWNER_AGENT_QUEUE_PROOF", "alice", ITEM_PATH,
        prompt=_queue_text(body)))
    assert _resolve(ns, "POST", _url(ITEM_PATH, "alice", queue_on_item),
                    body) == ("", None)


def test_a_signed_queue_only_targets_a_cloud_node():
    body = ROUTES_TEXT[ROUTES_TEXT.index(
        "async def _owner_agent_cloud_node("):]
    body = body[:body.index("async def org_agent_bots_handler(")]
    assert '("headless", "runner-pool")' in body
    assert "node != leaving" in body
    handler = ROUTES_TEXT[ROUTES_TEXT.index(
        "async def org_agent_bots_handler("):]
    handler = handler[:handler.index("target_decision = await")]
    assert handler.index("if signed:\n        preferred_node = await "
                         "_owner_agent_cloud_node(") > 0


def test_the_signer_is_named_user_and_stale_or_foreign_proofs_fail():
    ns = _harness(ACCOUNTS)
    canonical = _canonical(ns, "OWNER_AGENT_LIST_PROOF", "alice", LIST_PATH)
    sig = _fake_sig("pk-alice", canonical)
    node_url = _url(LIST_PATH, "alice", sig, name="node")
    assert _resolve(ns, "GET", node_url) == ("", None)
    assert not ns["_owner_agent_signed_triple"](_Request("GET", node_url))
    assert ns["_owner_agent_signed_triple"](
        _Request("GET", _url(LIST_PATH, "alice", sig)))
    stranger = _fake_sig("pk-mallory", canonical)
    assert _resolve(ns, "GET", _url(LIST_PATH, "alice", stranger)) == ("", None)
    stale = _harness(ACCOUNTS, ts_ok=False)
    assert _resolve(stale, "GET", _url(LIST_PATH, "alice", sig)) == ("", None)


def test_an_unconfirmed_address_may_read_but_not_write():
    ns = _harness(ACCOUNTS)
    path = "/api/repo/pending/project/agent-sessions"
    sig = _fake_sig("pk-pending", _canonical(
        ns, "OWNER_AGENT_LIST_PROOF", "pending", path))
    assert _resolve(ns, "GET", _url(path, "pending", sig))[0] == "bi:pending"
    item = path + "/s1"
    sig = _fake_sig("pk-pending", _canonical(
        ns, "OWNER_AGENT_DELETE_PROOF", "pending", item))
    with pytest.raises(RuntimeError):
        _resolve(ns, "DELETE", _url(item, "pending", sig))


def test_a_triple_is_the_only_credential_once_it_is_on_the_url():
    # The handlers waive the browser-origin check for a signed request, which
    # is only safe while the member context never falls back to the cookie.
    assert ROUTES_TEXT.count(
        "signed = _owner_agent_signed_triple(request)") == 2
    assert 'if method in ("POST", "DELETE") and not signed:' in ROUTES_TEXT
    assert ("if not signed and not _request_origin_allowed(env, request):"
            in ROUTES_TEXT)
    assert ROUTES_TEXT.count("signed_account=signed_account") == 4
    stream = ROUTES_TEXT[ROUTES_TEXT.index(
        "async def org_agent_bot_stream_handler("):]
    stream = stream[:stream.index("\nasync def ", 1)]
    assert 'if method_name(request) != "GET":' in stream
    assert "if _owner_agent_signed_triple(request):" in stream
    entry = ENTRY.read_text(encoding="utf-8")
    body = entry[entry.index("async def _org_agent_member_context("):]
    split = body.index("org_bi, org_row = await _org_row(env, org)")
    for part in (body[:split], body[split:body.index("\nasync def ", 1)]):
        assert "if signed_account is not None:" in part
        assert part.index("if signed_account is not None:") < part.index(
            "_account_session_record(")

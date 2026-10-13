#!/usr/bin/env python3
"""Key-signed alert-inbox requests from the desktop (adhoc #59).

The desktop Alerts page mirrors the website's notification bell. A desktop that
authenticated silently owns its account's Ed25519 key and holds no session
token, so it can only reach /api/notifications by signing a proof onto the URL.
These checks pin how narrow that credential is: four operations, each with its
own proof (and the delete proof additionally bound to the row being removed),
the collection path only, and no authority for an account that is not an active
user.

AST-extraction harness in the style of test_org_task_signed_writes.py.
"""

import ast
import asyncio
import hashlib
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
# clean_string lives in catalog.py; parse both so it can be extracted too.
SOURCE_TEXT = ENTRY_TEXT + "\n" + CATALOG.read_text(encoding="utf-8")

FUNCS = {
    "_account_alert_signed_session",
    "_alert_inbox_account_name",
    "clean_string",
}
CONSTANTS = {
    "ACCOUNT_ALERT_LIST_PROOF",
    "ACCOUNT_ALERT_READ_PROOF",
    "ACCOUNT_ALERT_DELETE_PROOF",
    "ACCOUNT_ALERT_CLEAR_PROOF",
    "ACCOUNT_ALERT_COLLECTION_RE",
}


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
    found = {
        node.name if hasattr(node, "name") else node.targets[0].id
        for node in selected
    }
    assert found == FUNCS | CONSTANTS, \
        "missing: %s" % sorted((FUNCS | CONSTANTS) - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="GET", url="", headers=None):
        self.method = method
        self.url = url
        self.headers = _Headers(headers)


def _fake_sig(pubkey, canonical):
    digest = hashlib.sha256(canonical).hexdigest()[:32]
    return "sig-" + pubkey + "-" + digest


def _harness(accounts, ts_ok=True, session_name=""):
    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        record = accounts.get(key)
        if record is None:
            return "bi:" + key, None
        record = dict(record)
        record.setdefault("name", key)
        return "bi:" + key, record

    async def _owner_pubkey(_env, owner):
        _bi, record = await _account_row(_env, owner)
        return record.get("pubkey", "") if record else ""

    async def ed25519_verify(pubkey, sig, canonical):
        # A node "signs" by presenting a digest over the exact canonical string
        # it signed, keyed by its public key. Digested rather than embedded so
        # the proof's newlines never have to survive a URL round trip.
        return bool(pubkey) and sig == _fake_sig(pubkey, canonical)

    async def _authed_account_name(_env, _request, _data=None):
        return session_name

    namespace = _load({
        "re": re,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "method_name": lambda request: getattr(request, "method", "GET"),
        "_ts_ok": lambda _ts: ts_ok,
        "_owner_pubkey": _owner_pubkey,
        "_account_row": _account_row,
        "_account_kind": lambda record: record.get("kind", "user"),
        "_authed_account_name": _authed_account_name,
        "ed25519_verify": ed25519_verify,
        "MAX_NODE_NAME": 63,
    })
    return namespace, object()


def _signed_url(namespace, path, proof_name, node="alice", pubkey="PK-alice",
                ts="1700000000000"):
    canonical = namespace[proof_name] + "\n" + node + "\n" + ts
    sig = _fake_sig(pubkey, canonical.encode())
    return (
        "https://forkmesh.test" + path
        + "?node=" + node + "&ts=" + ts + "&sig=" + sig
    )


def _signed_delete_url(namespace, item_id, node="alice", pubkey="PK-alice",
                       ts="1700000000000"):
    # The delete proof binds the row id, so the canonical string carries it.
    canonical = (namespace["ACCOUNT_ALERT_DELETE_PROOF"] + "\n" + node + "\n"
                 + item_id + "\n" + ts)
    sig = _fake_sig(pubkey, canonical.encode())
    return (
        "https://forkmesh.test/api/notifications"
        "?node=" + node + "&ts=" + ts + "&sig=" + sig
    )


def _signed_clear_url(namespace, node="alice", pubkey="PK-alice",
                      ts="1700000000000"):
    canonical = (namespace["ACCOUNT_ALERT_CLEAR_PROOF"] + "\n" + node
                 + "\nall\n" + ts)
    sig = _fake_sig(pubkey, canonical.encode())
    return (
        "https://forkmesh.test/api/notifications"
        "?node=" + node + "&ts=" + ts + "&sig=" + sig
    )


def _user(**overrides):
    record = {"pubkey": "PK-alice", "status": "active", "kind": "user"}
    record.update(overrides)
    return record


def test_list_and_read_proofs_authorize_their_own_operation():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_account_alert_signed_session"]

    assert asyncio.run(resolve(env, _Request(
        method="GET",
        url=_signed_url(namespace, "/api/notifications",
                        "ACCOUNT_ALERT_LIST_PROOF")))) == "alice"
    assert asyncio.run(resolve(env, _Request(
        method="POST",
        url=_signed_url(namespace, "/api/notifications",
                        "ACCOUNT_ALERT_READ_PROOF")))) == "alice"
    # The trailing-slash spelling of the same collection is the same route.
    assert asyncio.run(resolve(env, _Request(
        method="GET",
        url=_signed_url(namespace, "/api/notifications/",
                        "ACCOUNT_ALERT_LIST_PROOF")))) == "alice"


def test_a_read_proof_is_never_replayable_as_a_write():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_account_alert_signed_session"]

    assert asyncio.run(resolve(env, _Request(
        method="POST",
        url=_signed_url(namespace, "/api/notifications",
                        "ACCOUNT_ALERT_LIST_PROOF")))) == ""
    assert asyncio.run(resolve(env, _Request(
        method="GET",
        url=_signed_url(namespace, "/api/notifications",
                        "ACCOUNT_ALERT_READ_PROOF")))) == ""


def test_deleting_an_alert_needs_its_own_proof_bound_to_the_row():
    """The desktop can delete, but only the exact row it signed (adhoc #77)."""

    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_account_alert_signed_session"]
    row = "a" * 64
    other = "b" * 64

    assert asyncio.run(resolve(env, _Request(
        method="DELETE",
        url=_signed_delete_url(namespace, row)), row)) == "alice"
    # The same signature presented for a different row proves nothing.
    assert asyncio.run(resolve(env, _Request(
        method="DELETE",
        url=_signed_delete_url(namespace, row)), other)) == ""
    # …and neither does a delete with no row named at all.
    assert asyncio.run(resolve(env, _Request(
        method="DELETE",
        url=_signed_delete_url(namespace, row)), "")) == ""


def test_clearing_an_inbox_needs_a_separate_all_rows_proof():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_account_alert_signed_session"]
    row = "a" * 64

    assert asyncio.run(resolve(env, _Request(
        method="DELETE", url=_signed_clear_url(namespace)), "all")) == "alice"
    # A clear credential must not authorize a single row, and a row credential
    # must not become a bulk-delete credential.
    assert asyncio.run(resolve(env, _Request(
        method="DELETE", url=_signed_clear_url(namespace)), row)) == ""
    assert asyncio.run(resolve(env, _Request(
        method="DELETE", url=_signed_delete_url(namespace, row)), "all")) == ""


def test_a_read_or_list_proof_is_never_replayable_as_a_delete():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_account_alert_signed_session"]
    row = "a" * 64
    for proof in ("ACCOUNT_ALERT_LIST_PROOF", "ACCOUNT_ALERT_READ_PROOF"):
        assert asyncio.run(resolve(env, _Request(
            method="DELETE",
            url=_signed_url(
                namespace, "/api/notifications", proof)), row)) == "", proof
    # The delete and clear proofs are likewise inert on the read verbs.
    for method in ("GET", "POST"):
        assert asyncio.run(resolve(env, _Request(
            method=method,
            url=_signed_delete_url(namespace, row)))) == "", method
        assert asyncio.run(resolve(env, _Request(
            method=method,
            url=_signed_clear_url(namespace)))) == "", method


def test_no_other_verb_is_signature_authorized():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_account_alert_signed_session"]
    for method in ("PATCH", "PUT"):
        for proof in ("ACCOUNT_ALERT_LIST_PROOF", "ACCOUNT_ALERT_READ_PROOF",
                      "ACCOUNT_ALERT_DELETE_PROOF", "ACCOUNT_ALERT_CLEAR_PROOF"):
            assert asyncio.run(resolve(env, _Request(
                method=method,
                url=_signed_url(
                    namespace, "/api/notifications", proof)))) == "", (
                method, proof)


def test_only_the_collection_path_is_signable():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_account_alert_signed_session"]
    for path in ("/api/notifications/abc", "/api/notifications/settings",
                 "/api/poll", "/api/tasks"):
        assert asyncio.run(resolve(env, _Request(
            method="GET",
            url=_signed_url(
                namespace, path, "ACCOUNT_ALERT_LIST_PROOF")))) == "", path


def test_bad_signature_stale_stamp_and_ineligible_accounts_are_refused():
    namespace, env = _harness({
        "alice": _user(),
        "suspended": _user(status="suspended"),
        "orgaccount": _user(kind="org"),
    })
    resolve = namespace["_account_alert_signed_session"]
    path = "/api/notifications"

    forged = (
        "https://forkmesh.test/api/notifications"
        "?node=alice&ts=1700000000000&sig=not-a-real-signature"
    )
    assert asyncio.run(resolve(env, _Request(url=forged))) == ""

    # A signature that verifies for a different key is still somebody else's.
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, path, "ACCOUNT_ALERT_LIST_PROOF",
        pubkey="PK-mallory")))) == ""

    # Missing triple, unknown account, suspended account, non-user account.
    assert asyncio.run(resolve(env, _Request(
        url="https://forkmesh.test/api/notifications"))) == ""
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, path, "ACCOUNT_ALERT_LIST_PROOF",
        node="nobody", pubkey="PK-nobody")))) == ""
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, path, "ACCOUNT_ALERT_LIST_PROOF",
        node="suspended")))) == ""
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, path, "ACCOUNT_ALERT_LIST_PROOF",
        node="orgaccount")))) == ""

    # An expired stamp is refused even with an otherwise valid signature.
    stale, stale_env = _harness({"alice": _user()}, ts_ok=False)
    assert asyncio.run(stale["_account_alert_signed_session"](
        stale_env,
        _Request(url=_signed_url(
            stale, path, "ACCOUNT_ALERT_LIST_PROOF")))) == ""


def test_the_session_token_still_wins_and_the_signature_only_fills_the_gap():
    signed, env = _harness({"alice": _user()})
    resolve = signed["_alert_inbox_account_name"]

    # No session, valid signature: the desktop is recognized.
    assert asyncio.run(resolve(env, _Request(
        method="GET",
        url=_signed_url(signed, "/api/notifications",
                        "ACCOUNT_ALERT_LIST_PROOF")))) == "alice"
    # No session, no signature: nothing is authorized.
    assert asyncio.run(resolve(env, _Request(
        url="https://forkmesh.test/api/notifications?node=alice"))) == ""
    # A browser session is answered without consulting the signature at all.
    browser, browser_env = _harness({"alice": _user()}, session_name="bob")
    assert asyncio.run(browser["_alert_inbox_account_name"](
        browser_env,
        _Request(url="https://forkmesh.test/api/notifications"))) == "bob"


def test_the_inbox_handler_uses_the_shared_gate_for_every_verb():
    start = ENTRY_TEXT.index("async def notifications_handler(env, request):")
    handler = ENTRY_TEXT[start:ENTRY_TEXT.index(
        "async def mirror_requests_handler(", start)]
    assert handler.count("_alert_inbox_account_name(") == 3
    # The DELETE branch hands either the row id or explicit all-inbox resource
    # to the gate, so a single-row proof cannot be replayed as a bulk clear.
    delete_branch = handler[handler.index('if method == "DELETE":'):]
    assert 'resource=("all" if clear_all else item_id)' in delete_branch
    # Pausing pings is the one operation that NARROWS the shared gate rather
    # than relying on it: the POST proof binds only the node, so honouring a
    # pause on the signed path would let a captured mark-read be replayed as a
    # mute. It runs after the gate above and demands a session on top, so the
    # single extra call may only appear inside the POST branch.
    post_branch = handler[handler.index('if method == "POST":'):
                          handler.index('if method == "DELETE":')]
    assert handler.count("_authed_account_name(") == 1
    assert '"pauseUntil" in data' in post_branch
    assert "_authed_account_name(env, request, data) != node" in post_branch


def test_desktop_and_worker_agree_on_the_canonical_proof_strings():
    qt = (
        ENTRY.parents[2] / "desktop" / "src" / "MainWindowInternal.h"
    ).read_text(encoding="utf-8")
    for proof in ("forkmesh-account-alert-list-v1",
                  "forkmesh-account-alert-read-v1",
                  "forkmesh-account-alert-delete-v1",
                  "forkmesh-account-alert-clear-v1"):
        assert '"%s"' % proof in ENTRY_TEXT
        assert '"%s"' % proof in qt

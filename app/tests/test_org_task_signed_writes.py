#!/usr/bin/env python3
"""Key-signed organization-task requests from the desktop (adhoc #18, #52).

A desktop that authenticated silently owns its account's Ed25519 key and holds
no account session token, so the Agents composer's Task toggle and the Tasks
tab's board can only reach /api/tasks by signing a proof onto the URL. These
checks pin how narrow that credential is: six operations, each with its own
proof, the completion/deletion/priority proofs bound to the one task they name
(priority additionally to the one value it sets), and no authority for an
account that is not an active user.

AST-extraction harness in the style of test_chat_room_key.py.
"""

import ast
import asyncio
import hashlib
import json
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
# clean_string lives in catalog.py; parse both so it can be extracted too.
SOURCE_TEXT = ENTRY_TEXT + "\n" + CATALOG.read_text(encoding="utf-8")
# The Office marketing-tasks runtime moved into its own module so Cloudflare's
# Python startup validation never parses it; the authority contract below still
# covers it.
OFFICE_ROUTES_TEXT = (
    Path(__file__).resolve().parents[2] / "world" / "src"
    / "world_office_routes.py"
).read_text(encoding="utf-8")

FUNCS = {
    "_org_task_signed_session",
    "_signed_account_session",
    "_account_signing_pubkeys",
    "_owner_signing_pubkeys",
    "_verify_owner_signature",
    "clean_string",
    "org_task_update_digest",
}
CONSTANTS = {
    "ORG_TASK_OPEN_PROOF",
    "ORG_TASK_COMPLETE_PROOF",
    "ORG_TASK_UNDO_PROOF",
    "ORG_TASK_AGENT_STATUS_PROOF",
    "ORG_TASK_AGENT_STATUS_BATCH_PROOF",
    "ORG_TASK_LIST_PROOF",
    "ORG_TASK_DELETE_PROOF",
    "ORG_TASK_PRIORITY_PROOF",
    "ORG_TASK_PROGRESS_PROOF",
    "ORG_TASK_UPDATE_PROOF",
    "ORG_TASK_ATTACHMENT_PROOF",
    "ORG_TASK_ATTACHMENT_RE",
    "ORG_TASK_COMPLETE_RE",
    "ORG_TASK_UNDO_RE",
    "ORG_TASK_AGENT_STATUS_RE",
    "ORG_TASK_AGENT_STATUS_BATCH_RE",
    "ORG_TASK_COLLECTION_RE",
    "ORG_TASK_ITEM_RE",
    "ORG_TASK_RESTORE_PROOF",
    "ORG_TASK_DELETE_BATCH_PROOF",
    "ORG_TASK_RESTORE_RE",
    "ORG_TASK_DELETE_BATCH_RE",
    "ORG_TASK_REMIND_PROOF",
    "ORG_TASK_CHECKIN_PROOF",
    "ORG_TASK_REMIND_RE",
    "ORG_TASK_CHECKIN_RE",
    "ORG_TASK_REPLY_PROOF",
    "ORG_TASK_HISTORY_PROOF",
    "ORG_TASK_REPLY_RE",
    "ORG_TASK_HISTORY_RE",
}

TASK_ID = "a" * 32
OTHER_TASK_ID = "b" * 32


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
    namespace = {"json": json, "hashlib": hashlib, **dict(extra)}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="POST", url="", headers=None):
        self.method = method
        self.url = url
        self.headers = _Headers(headers)


def _fake_sig(pubkey, canonical):
    digest = hashlib.sha256(canonical).hexdigest()[:32]
    return "sig-" + pubkey + "-" + digest


def _harness(accounts, ts_ok=True, devices=None):
    devices = devices or {}

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

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def _account_devices_list(_env, account_bi):
        return [dict(device) for device in devices.get(account_bi, [])]

    namespace = _load({
        "re": re,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "method_name": lambda request: getattr(request, "method", "GET"),
        "_ts_ok": lambda _ts: ts_ok,
        "_owner_pubkey": _owner_pubkey,
        "_account_row": _account_row,
        "_account_devices_list": _account_devices_list,
        "blind_index": blind_index,
        "_account_kind": lambda record: record.get("kind", "user"),
        "ed25519_verify": ed25519_verify,
        "MAX_NODE_NAME": 63,
    })
    return namespace, object()


def _device(pubkey, capabilities=("owner_sign",), enabled=True):
    return {
        "pubkey": pubkey,
        "capabilities": list(capabilities),
        "enabled": enabled,
    }


def _signed_url(namespace, path, proof_name, resource="", node="alice",
                pubkey="PK-alice", ts="1700000000000"):
    canonical = namespace[proof_name] + "\n" + node + "\n"
    if resource:
        canonical += resource + "\n"
    canonical += ts
    sig = _fake_sig(pubkey, canonical.encode())
    return (
        "https://forkmesh.test" + path
        + "?node=" + node + "&ts=" + ts + "&sig=" + sig
    )


def _user(**overrides):
    record = {"pubkey": "PK-alice", "status": "active", "kind": "user"}
    record.update(overrides)
    return record


def test_open_and_complete_proofs_authorize_their_own_operation():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]

    account_bi, record = asyncio.run(resolve(env, _Request(
        url=_signed_url(namespace, "/api/tasks", "ORG_TASK_OPEN_PROOF"))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"

    account_bi, record = asyncio.run(resolve(env, _Request(
        url=_signed_url(
            namespace, "/api/tasks/%s/agent-status" % TASK_ID,
            "ORG_TASK_AGENT_STATUS_PROOF", resource=TASK_ID))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"

    account_bi, record = asyncio.run(resolve(env, _Request(
        url=_signed_url(
            namespace, "/api/tasks/%s/complete" % TASK_ID,
            "ORG_TASK_COMPLETE_PROOF", resource=TASK_ID))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"


def test_agent_status_batch_proof_authorizes_only_the_fleet_write():
    """adhoc #1618: one signed write reports every run, and nothing else."""

    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    batch_path = "/api/tasks/agent-status"

    account_bi, record = asyncio.run(resolve(env, _Request(
        url=_signed_url(
            namespace, batch_path, "ORG_TASK_AGENT_STATUS_BATCH_PROOF"))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"

    # The batch proof opens no task, closes none, deletes none, and reads
    # nothing — and no other proof stands in for it.
    for path, proof in (
        ("/api/tasks", "ORG_TASK_AGENT_STATUS_BATCH_PROOF"),
        ("/api/tasks/%s/complete" % TASK_ID, "ORG_TASK_AGENT_STATUS_BATCH_PROOF"),
        (batch_path, "ORG_TASK_OPEN_PROOF"),
        (batch_path, "ORG_TASK_AGENT_STATUS_PROOF"),
        (batch_path, "ORG_TASK_LIST_PROOF"),
    ):
        assert asyncio.run(resolve(env, _Request(
            url=_signed_url(namespace, path, proof)))) == ("", None), (path, proof)

    # Nor is the batch path readable or deletable with any signature.
    for method in ("GET", "DELETE"):
        assert asyncio.run(resolve(env, _Request(
            method=method,
            url=_signed_url(
                namespace, batch_path,
                "ORG_TASK_AGENT_STATUS_BATCH_PROOF")))) == ("", None), method


def test_completion_proof_is_bound_to_the_task_it_names():
    namespace, env = _harness({"alice": _user()})
    # A proof signed for one task replayed against another must not resolve.
    replayed = _signed_url(
        namespace, "/api/tasks/%s/complete" % OTHER_TASK_ID,
        "ORG_TASK_COMPLETE_PROOF", resource=TASK_ID)
    account_bi, record = asyncio.run(
        namespace["_org_task_signed_session"](env, _Request(url=replayed)))
    assert (account_bi, record) == ("", None)

    # Nor may an open proof stand in for a completion.
    crossed = _signed_url(
        namespace, "/api/tasks/%s/complete" % TASK_ID,
        "ORG_TASK_OPEN_PROOF", resource=TASK_ID)
    account_bi, record = asyncio.run(
        namespace["_org_task_signed_session"](env, _Request(url=crossed)))
    assert (account_bi, record) == ("", None)


def test_list_proof_reads_the_board_and_nothing_else():
    """The Tasks tab's GET is signature-authorized; its edits are not (#52)."""

    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]

    account_bi, record = asyncio.run(resolve(env, _Request(
        method="GET",
        url=_signed_url(namespace, "/api/tasks", "ORG_TASK_LIST_PROOF"))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"

    # Reading one task by id, or any sub-resource, still needs a session.
    for path in ("/api/tasks/%s" % TASK_ID, "/api/tasks/%s/qa" % TASK_ID):
        assert asyncio.run(resolve(env, _Request(
            method="GET",
            url=_signed_url(
                namespace, path, "ORG_TASK_LIST_PROOF",
                resource=TASK_ID)))) == ("", None), path
        assert asyncio.run(resolve(env, _Request(
            method="GET",
            url=_signed_url(
                namespace, path, "ORG_TASK_LIST_PROOF")))) == ("", None), path

    # A read proof must never be replayable as a write, in either direction.
    assert asyncio.run(resolve(env, _Request(
        url=_signed_url(
            namespace, "/api/tasks", "ORG_TASK_LIST_PROOF")))) == ("", None)
    assert asyncio.run(resolve(env, _Request(
        method="GET",
        url=_signed_url(
            namespace, "/api/tasks", "ORG_TASK_OPEN_PROOF")))) == ("", None)


def test_registered_device_key_reads_the_board_after_a_restart():
    """adhoc #63: the board went "invalid session" on every restarted app.

    A desktop that logged in with a password gets its key stored in
    account_devices; the account's primary pubkey keeps naming the install that
    created the account. The session token is memory-only, so the next launch
    signs the list proof with the device key — which must authorize the read,
    exactly as it authorizes GET /api/sync.
    """
    namespace, env = _harness(
        {"alice": _user()},
        devices={"bi:alice": [
            _device("PK-alice-laptop"),
            _device("PK-alice-retired", enabled=False),
            _device("PK-alice-readonly", capabilities=()),
        ]},
    )
    resolve = namespace["_org_task_signed_session"]

    account_bi, record = asyncio.run(resolve(env, _Request(
        method="GET",
        url=_signed_url(namespace, "/api/tasks", "ORG_TASK_LIST_PROOF",
                        pubkey="PK-alice-laptop"))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"

    # Opening a task from that same machine is signed the same way.
    assert asyncio.run(resolve(env, _Request(
        url=_signed_url(namespace, "/api/tasks", "ORG_TASK_OPEN_PROOF",
                        pubkey="PK-alice-laptop"))))[0] == "bi:alice"

    # A revoked/disabled device, and one without owner_sign, stay locked out.
    for pubkey in ("PK-alice-retired", "PK-alice-readonly"):
        assert asyncio.run(resolve(env, _Request(
            method="GET",
            url=_signed_url(
                namespace, "/api/tasks", "ORG_TASK_LIST_PROOF",
                pubkey=pubkey)))) == ("", None), pubkey


def test_delete_proof_authorizes_the_one_task_it_names():
    """Delete is key-signed too (adhoc #1426), bound to that task id.

    A desktop that authenticated silently could read and write the board but not
    remove a row, so the Tasks tab told an operator who was signed in with their
    account key to go type a password. Manage permission is still enforced by
    _delete, exactly as it is for a session-token caller.
    """
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    path = "/api/tasks/%s" % TASK_ID

    account_bi, record = asyncio.run(resolve(env, _Request(
        method="DELETE",
        url=_signed_url(
            namespace, path, "ORG_TASK_DELETE_PROOF", resource=TASK_ID))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"

    # Signed for one task, replayed against another: refused.
    assert asyncio.run(resolve(env, _Request(
        method="DELETE",
        url=_signed_url(
            namespace, "/api/tasks/%s" % OTHER_TASK_ID,
            "ORG_TASK_DELETE_PROOF", resource=TASK_ID)))) == ("", None)

    # No other proof stands in for a deletion, and the delete proof authorizes
    # nothing but a DELETE of the task it names.
    for proof in ("ORG_TASK_OPEN_PROOF", "ORG_TASK_LIST_PROOF",
                  "ORG_TASK_COMPLETE_PROOF"):
        assert asyncio.run(resolve(env, _Request(
            method="DELETE",
            url=_signed_url(
                namespace, path, proof, resource=TASK_ID)))) == ("", None), proof
    for method in ("GET", "POST"):
        assert asyncio.run(resolve(env, _Request(
            method=method,
            url=_signed_url(
                namespace, path, "ORG_TASK_DELETE_PROOF",
                resource=TASK_ID)))) == ("", None), method

    # A bare DELETE of the collection, or of a sub-resource, is not a deletion
    # the relay signs for.
    for target in ("/api/tasks", "/api/tasks/%s/qa" % TASK_ID):
        assert asyncio.run(resolve(env, _Request(
            method="DELETE",
            url=_signed_url(
                namespace, target, "ORG_TASK_DELETE_PROOF",
                resource=TASK_ID)))) == ("", None), target


def test_signature_authorizes_only_the_six_named_operations():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]

    # Edits are never signature-authorized (the priority-only POST below is
    # the one exception, and it carries its own value-bound proof), whichever
    # proof signs.
    for method in ("PATCH", "PUT"):
        for proof in ("ORG_TASK_OPEN_PROOF", "ORG_TASK_LIST_PROOF",
                      "ORG_TASK_DELETE_PROOF"):
            account_bi, record = asyncio.run(resolve(env, _Request(
                method=method,
                url=_signed_url(namespace, "/api/tasks", proof))))
            assert (account_bi, record) == ("", None), (method, proof)

    # Neither are the other task sub-actions, whichever proof is presented.
    for action in ("start", "stop", "checkin", "qa"):
        path = "/api/tasks/%s/%s" % (TASK_ID, action)
        for proof in ("ORG_TASK_OPEN_PROOF", "ORG_TASK_COMPLETE_PROOF"):
            account_bi, record = asyncio.run(resolve(env, _Request(
                url=_signed_url(namespace, path, proof, resource=TASK_ID))))
            assert (account_bi, record) == ("", None), (action, proof)

    # The live-status proof is bound to its exact task and exact sub-action.
    status_path = "/api/tasks/%s/agent-status" % TASK_ID
    crossed = _signed_url(
        namespace, status_path, "ORG_TASK_COMPLETE_PROOF", resource=TASK_ID)
    assert asyncio.run(resolve(env, _Request(url=crossed))) == ("", None)
    replayed = _signed_url(
        namespace,
        "/api/tasks/%s/agent-status" % OTHER_TASK_ID,
        "ORG_TASK_AGENT_STATUS_PROOF",
        resource=TASK_ID,
    )
    assert asyncio.run(resolve(env, _Request(url=replayed))) == ("", None)


def test_priority_proof_sets_exactly_the_task_and_value_it_names():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    path = "/api/tasks/%s" % TASK_ID
    url = _signed_url(namespace, path, "ORG_TASK_PRIORITY_PROOF",
                      resource=TASK_ID + "\n7")

    # The exact task, the exact value, and a priority-only PATCH body (the
    # task API's update verb): authorized.
    account_bi, record = asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), {"priority": 7}))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"
    # A stringy but integral body value signs and applies identically.
    account_bi, _ = asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), {"priority": "7"}))
    assert account_bi == "bi:alice"

    # A body value other than the signed one is a different edit: refused.
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), {"priority": 8})) == ("", None)
    # Any field beyond the priority makes it a general edit: refused.
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url),
        {"priority": 7, "title": "x"})) == ("", None)
    # No body at all is not a priority edit: refused.
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url))) == ("", None)
    # Out-of-range and non-integral priorities never qualify.
    for bad in (-1, 100, "high", None):
        bad_url = _signed_url(namespace, path, "ORG_TASK_PRIORITY_PROOF",
                              resource=TASK_ID + "\n" + str(bad))
        assert asyncio.run(resolve(
            env, _Request(method="PATCH", url=bad_url),
            {"priority": bad})) == ("", None)
    # The proof is bound to the task it names: replay on another task fails.
    replayed = _signed_url(
        namespace, "/api/tasks/%s" % OTHER_TASK_ID,
        "ORG_TASK_PRIORITY_PROOF", resource=TASK_ID + "\n7")
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=replayed),
        {"priority": 7})) == ("", None)
    # And to its method: GET and POST with the same proof authorize nothing.
    for method in ("GET", "POST"):
        assert asyncio.run(resolve(
            env, _Request(method=method, url=url),
            {"priority": 7})) == ("", None)
    # PATCH cannot borrow any POST-shaped proof either.
    for proof, borrowed in (
            ("ORG_TASK_OPEN_PROOF", "/api/tasks"),
            ("ORG_TASK_COMPLETE_PROOF", "/api/tasks/%s/complete" % TASK_ID)):
        crossed = _signed_url(
            namespace, borrowed, proof,
            resource=TASK_ID if "complete" in borrowed else "")
        assert asyncio.run(resolve(
            env, _Request(method="PATCH", url=crossed),
            {"priority": 7})) == ("", None)


def test_progress_proof_sets_exactly_the_task_and_value_it_names():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    path = "/api/tasks/%s" % TASK_ID
    url = _signed_url(namespace, path, "ORG_TASK_PROGRESS_PROOF",
                      resource=TASK_ID + "\n40")

    account_bi, record = asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), {"progress": 40}))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"
    account_bi, _ = asyncio.run(resolve(
        env, _Request(method="PATCH", url=url),
        {"progress": "40", "sessionToken": ""}))
    assert account_bi == "bi:alice"

    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), {"progress": 41})) == ("", None)
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url),
        {"progress": 40, "priority": 7})) == ("", None)
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url))) == ("", None)
    for edge in (0, 100):
        edge_url = _signed_url(namespace, path, "ORG_TASK_PROGRESS_PROOF",
                               resource=TASK_ID + "\n" + str(edge))
        assert asyncio.run(resolve(
            env, _Request(method="PATCH", url=edge_url),
            {"progress": edge}))[0] == "bi:alice"
    for bad in (-1, 101, "half", None):
        bad_url = _signed_url(namespace, path, "ORG_TASK_PROGRESS_PROOF",
                              resource=TASK_ID + "\n" + str(bad))
        assert asyncio.run(resolve(
            env, _Request(method="PATCH", url=bad_url),
            {"progress": bad})) == ("", None)
    replayed = _signed_url(
        namespace, "/api/tasks/%s" % OTHER_TASK_ID,
        "ORG_TASK_PROGRESS_PROOF", resource=TASK_ID + "\n40")
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=replayed),
        {"progress": 40})) == ("", None)
    priority_signed = _signed_url(namespace, path, "ORG_TASK_PRIORITY_PROOF",
                                  resource=TASK_ID + "\n40")
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=priority_signed),
        {"progress": 40})) == ("", None)
    progress_signed_as_priority = _signed_url(
        namespace, path, "ORG_TASK_PROGRESS_PROOF", resource=TASK_ID + "\n7")
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=progress_signed_as_priority),
        {"priority": 7})) == ("", None)
    for method in ("GET", "POST"):
        assert asyncio.run(resolve(
            env, _Request(method=method, url=url),
            {"progress": 40})) == ("", None)


def test_bad_signature_stale_stamp_and_ineligible_accounts_are_refused():
    namespace, env = _harness({
        "alice": _user(),
        "suspended": _user(status="suspended"),
        "orgaccount": _user(kind="org"),
    })
    resolve = namespace["_org_task_signed_session"]
    open_path = "/api/tasks"

    forged = (
        "https://forkmesh.test/api/tasks"
        "?node=alice&ts=1700000000000&sig=not-a-real-signature"
    )
    assert asyncio.run(resolve(env, _Request(url=forged))) == ("", None)

    # A signature that verifies for a different key is still somebody else's.
    wrong_key = _signed_url(
        namespace, open_path, "ORG_TASK_OPEN_PROOF", pubkey="PK-mallory")
    assert asyncio.run(resolve(env, _Request(url=wrong_key))) == ("", None)

    # Missing triple, unknown account, suspended account, non-user account.
    assert asyncio.run(resolve(env, _Request(
        url="https://forkmesh.test/api/tasks"))) == ("", None)
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, open_path, "ORG_TASK_OPEN_PROOF",
        node="nobody", pubkey="PK-nobody")))) == ("", None)
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, open_path, "ORG_TASK_OPEN_PROOF",
        node="suspended")))) == ("", None)
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, open_path, "ORG_TASK_OPEN_PROOF",
        node="orgaccount")))) == ("", None)

    # An expired stamp is refused even with an otherwise valid signature.
    stale, stale_env = _harness({"alice": _user()}, ts_ok=False)
    assert asyncio.run(stale["_org_task_signed_session"](
        stale_env,
        _Request(url=_signed_url(
            stale, open_path, "ORG_TASK_OPEN_PROOF")))) == ("", None)


def test_signed_request_never_falls_back_to_cookie_authority():
    """A URL signature must switch cookie auth off, not merely add to it.

    same_origin() relaxes for a signed write, so if session() could still fall
    through to the cookie a cross-site POST could append a junk triple and
    borrow the victim's browser session.
    """
    start = OFFICE_ROUTES_TEXT.index("class _OfficeMarketingTasksRuntime:")
    runtime = OFFICE_ROUTES_TEXT[start:start + 6000]
    session = runtime[runtime.index("    async def session(self, data=None):"):]
    signed = session.index("if self._signed_query():")
    fallback = session.index("_account_session_record")
    assert signed < fallback, "cookie fallback must sit behind the signed check"
    assert "return await _org_task_signed_session(" in session
    # The relaxed origin check and the signed session must read the same triple.
    assert "self._signed_query()" in runtime[:runtime.index("def query(")]


def test_desktop_and_worker_agree_on_the_canonical_proof_strings():
    qt = (
        ENTRY.parents[2] / "desktop" / "src" / "MainWindowInternal.h"
    ).read_text(encoding="utf-8")
    for proof in ("forkmesh-org-task-open-v1", "forkmesh-org-task-complete-v1",
                  "forkmesh-org-task-undo-v1",
                  "forkmesh-org-task-list-v1", "forkmesh-org-task-delete-v1",
                  "forkmesh-org-task-agent-status-v1",
                  "forkmesh-org-task-agent-status-batch-v1",
                  "forkmesh-org-task-remind-v1",
                  "forkmesh-org-task-checkin-v1",
                  "forkmesh-org-task-reply-v1",
                  "forkmesh-org-task-history-v1"):
        assert '"%s"' % proof in ENTRY_TEXT
        assert '"%s"' % proof in qt


def test_desktop_tasks_tab_signs_when_it_holds_no_session_token():
    """The tab must not go back to bailing out on an empty token (#52).

    Every ordinary launch authenticates silently, so a token-only Tasks tab
    shows an empty board to an operator who is signed in.
    """
    tab = (
        ENTRY.parents[2] / "desktop" / "src" / "MainWindowTasks.cpp"
    ).read_text(encoding="utf-8")
    request = tab[tab.index("void MainWindow::requestOrganizationTasks("):]
    request = request[:request.index("\nvoid MainWindow::")]
    assert "authenticateOrgTaskRequest(" in request
    assert "kOrgTaskListProof" in tab
    # And Delete must map to its own proof rather than demanding a password
    # sign-in from an install that holds the account key (adhoc #1426).
    proof = tab[tab.index("QString organizationTaskProof("):]
    proof = proof[:proof.index("\n}\n")]
    assert 'QByteArrayLiteral("DELETE")' in proof
    assert "kOrgTaskDeleteProof" in proof
    delete = proof[proof.index('QByteArrayLiteral("DELETE")'):]
    delete = delete[:delete.index("kOrgTaskDeleteProof")]
    assert "updateDigest(body)" in delete
    assert "kOrgTaskAgentStatusProof" in proof
    assert "kOrgTaskRemindProof" in proof
    assert "kOrgTaskCheckinProof" in proof
    assert "kOrgTaskReplyProof" in proof
    assert "kOrgTaskHistoryProof" in proof


def test_remind_and_checkin_proofs_bind_the_task_and_the_exact_body():
    """A desktop signed in with its account key can remind and check in."""

    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    digest = namespace["org_task_update_digest"]
    for action, proof, body in (
        ("remind", "ORG_TASK_REMIND_PROOF", {"note": "any update?"}),
        ("checkin", "ORG_TASK_CHECKIN_PROOF",
         {"state": "on_track", "note": "halfway"}),
    ):
        path = "/api/tasks/%s/%s" % (TASK_ID, action)
        url = _signed_url(namespace, path, proof,
                          resource=TASK_ID + "\n" + digest(body))
        assert asyncio.run(resolve(
            env, _Request(url=url), dict(body)))[0] == "bi:alice", action
        assert asyncio.run(resolve(
            env, _Request(url=url), {**body, "note": "other"})) == (
                "", None), action
        assert asyncio.run(resolve(
            env, _Request(method="PATCH", url=url), body)) == (
                "", None), action
        replayed = _signed_url(
            namespace, "/api/tasks/%s/%s" % (OTHER_TASK_ID, action), proof,
            resource=TASK_ID + "\n" + digest(body))
        assert asyncio.run(resolve(
            env, _Request(url=replayed), body)) == ("", None), action
    bare = _signed_url(namespace, "/api/tasks/%s/remind" % TASK_ID,
                       "ORG_TASK_REMIND_PROOF", resource=TASK_ID)
    assert asyncio.run(resolve(env, _Request(url=bare), {}))[0] == "bi:alice"
    crossed = _signed_url(namespace, "/api/tasks/%s/checkin" % TASK_ID,
                          "ORG_TASK_REMIND_PROOF", resource=TASK_ID)
    assert asyncio.run(resolve(env, _Request(url=crossed), {})) == ("", None)


def test_desktop_agent_delete_is_not_stranded_by_the_busy_guard():
    launch = (
        ENTRY.parents[2] / "desktop" / "src" / "MainWindowAgentLaunch.cpp"
    ).read_text(encoding="utf-8")
    flow = launch[launch.index("void MainWindow::deleteAgentSessionOrgTask("):]
    flow = flow[:flow.index("\nvoid MainWindow::")]
    assert '"task_delete_blocked"' in flow
    assert '"force_requires_owner"' in flow
    assert "deleteAgentSessionOrgTask(sessionId, taskId, true)" in flow
    assert '{QStringLiteral("force"), true}' in flow
    kept = flow[flow.index("Kept organization task"):]
    assert "deleteAgentSessionAndCleanupAfterTask(sessionId)" in kept
    assert '"/agent-status"' in flow
    assert "AgentStatus::Stopped" in flow


def test_update_proof_is_bound_to_the_task_and_the_exact_body():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    path = "/api/tasks/%s" % TASK_ID
    body = {"title": "Moved", "parentTaskId": OTHER_TASK_ID, "team": "security"}
    digest = namespace["org_task_update_digest"](body)
    url = _signed_url(namespace, path, "ORG_TASK_UPDATE_PROOF",
                      resource=TASK_ID + "\n" + digest)
    account_bi, _ = asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), dict(reversed(body.items()))))
    assert account_bi == "bi:alice"
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url),
        {**body, "title": "Other"})) == ("", None)
    replayed = _signed_url(namespace, "/api/tasks/%s" % OTHER_TASK_ID,
                           "ORG_TASK_UPDATE_PROOF",
                           resource=TASK_ID + "\n" + digest)
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=replayed), body)) == ("", None)
    for method in ("GET", "POST", "DELETE"):
        assert asyncio.run(resolve(
            env, _Request(method=method, url=url), body)) == ("", None)
    assert namespace["org_task_update_digest"]({"sessionToken": "x"}) == ""


def test_trash_proofs_bind_the_task_and_the_exact_body():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    digest = namespace["org_task_update_digest"]
    path = "/api/tasks/%s" % TASK_ID

    forced = {"force": True, "reason": "duplicate"}
    url = _signed_url(namespace, path, "ORG_TASK_DELETE_PROOF",
                      resource=TASK_ID + "\n" + digest(forced))
    assert asyncio.run(resolve(
        env, _Request(method="DELETE", url=url), forced))[0] == "bi:alice"
    assert asyncio.run(resolve(
        env, _Request(method="DELETE", url=url),
        {"force": False, "reason": "duplicate"})) == ("", None)
    bare = _signed_url(namespace, path, "ORG_TASK_DELETE_PROOF",
                       resource=TASK_ID)
    assert asyncio.run(resolve(
        env, _Request(method="DELETE", url=bare), forced)) == ("", None)
    assert asyncio.run(resolve(
        env, _Request(method="DELETE", url=bare), {}))[0] == "bi:alice"

    restore_path = "/api/tasks/%s/restore" % TASK_ID
    restore = _signed_url(namespace, restore_path, "ORG_TASK_RESTORE_PROOF",
                          resource=TASK_ID)
    assert asyncio.run(resolve(
        env, _Request(method="POST", url=restore), {}))[0] == "bi:alice"
    assert asyncio.run(resolve(env, _Request(
        method="POST",
        url=_signed_url(namespace, "/api/tasks/%s/restore" % OTHER_TASK_ID,
                        "ORG_TASK_RESTORE_PROOF", resource=TASK_ID)),
        {})) == ("", None)
    assert asyncio.run(resolve(env, _Request(
        method="POST",
        url=_signed_url(namespace, restore_path, "ORG_TASK_COMPLETE_PROOF",
                        resource=TASK_ID)), {})) == ("", None)

    batch = {"ids": [TASK_ID, OTHER_TASK_ID]}
    batch_url = _signed_url(namespace, "/api/tasks/delete",
                            "ORG_TASK_DELETE_BATCH_PROOF",
                            resource=digest(batch))
    assert asyncio.run(resolve(
        env, _Request(method="POST", url=batch_url), batch))[0] == "bi:alice"
    assert asyncio.run(resolve(
        env, _Request(method="POST", url=batch_url),
        {"ids": [TASK_ID, OTHER_TASK_ID], "force": True})) == ("", None)
    assert asyncio.run(resolve(
        env, _Request(method="POST", url=batch_url), {})) == ("", None)
    assert asyncio.run(resolve(
        env, _Request(method="DELETE", url=batch_url), batch)) == ("", None)


WORK_ITEMS_DIGEST_VECTOR = (
    {
        "workItems": [{
            "repository": "forkmesh/src",
            "branch": "feat/task-work-items",
            "worktree": "/home/tester/src/.worktrees/012-café \"q\"\\\\x\t",
            "node": "desk-1",
        }],
        "priority": 7,
        "flag": True,
        "none": None,
    },
    "ec09f1e510ca68958c21322518b3956925c79edba3a8c782e586f4f325ad7ad7",
)


def test_work_items_update_signs_with_the_update_proof_and_body_digest():
    namespace, env = _harness({"alice": _user()})
    body, expected = WORK_ITEMS_DIGEST_VECTOR
    digest = namespace["org_task_update_digest"](
        {**body, "sessionToken": "ignored"})
    assert digest == expected
    work_items = {"workItems": body["workItems"]}
    path = "/api/tasks/%s" % TASK_ID
    url = _signed_url(
        namespace, path, "ORG_TASK_UPDATE_PROOF",
        resource=TASK_ID + "\n" + namespace["org_task_update_digest"](
            work_items))
    resolve = namespace["_org_task_signed_session"]
    account_bi, _ = asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), work_items))
    assert account_bi == "bi:alice"
    assert asyncio.run(resolve(
        env, _Request(method="PATCH", url=url), {"workItems": []})) == (
            "", None)
    qt = (
        ENTRY.parents[2] / "desktop" / "tests" / "test_task_work_items.cpp"
    ).read_text(encoding="utf-8")
    assert expected in qt
    header = (
        ENTRY.parents[2] / "desktop" / "src" / "MainWindowInternal.h"
    ).read_text(encoding="utf-8")
    assert '"forkmesh-org-task-update-v1"' in header


def test_desktop_attach_to_deleted_task_is_expected():
    src = ENTRY.parents[2] / "desktop" / "src"
    tab = (src / "MainWindowTasks.cpp").read_text(encoding="utf-8")
    request = tab[tab.index("void MainWindow::requestOrganizationTasks("):]
    request = request[:request.index("\nvoid MainWindow::")]
    assert "forkmesh::markExpectedHttpStatus(request, expected)" in request
    assert "!ok && !expectedHttpStatuses.contains(status)" in request
    items = (src / "MainWindowTaskWorkItems.cpp").read_text(encoding="utf-8")
    attach = items[items.index("void MainWindow::sendOrganizationTaskWorkItemAttach("):]
    attach = attach[:attach.index("\nvoid MainWindow::")]
    gone = attach[attach.index('QLatin1String("task_not_found")'):]
    gone = gone[:gone.index("return;")]
    assert "m_taskWorkItemAttachQueue.remove(taskId)" in gone
    assert "fail" not in gone.lower()
    assert "{404});" in attach
    delete = tab[tab.index("void MainWindow::sendOrganizationTaskDelete("):]
    delete = delete[:delete.index("\nvoid MainWindow::")]
    assert "{404});" in delete
    launch = (src / "MainWindowAgentLaunch.cpp").read_text(encoding="utf-8")
    flow = launch[launch.index("void MainWindow::deleteAgentSessionOrgTask("):]
    flow = flow[:flow.index("\nvoid MainWindow::")]
    assert "{404});" in flow


def test_comment_and_history_proofs_bind_the_task_and_the_comment():
    """A desktop signed in with its account key can comment and read history."""

    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_org_task_signed_session"]
    digest = namespace["org_task_update_digest"]
    body = {"body": "looks good @bob"}
    path = "/api/tasks/%s/replies" % TASK_ID
    url = _signed_url(namespace, path, "ORG_TASK_REPLY_PROOF",
                      resource=TASK_ID + "\n" + digest(body))
    assert asyncio.run(resolve(env, _Request(url=url), dict(body)))[0] == (
        "bi:alice")
    assert asyncio.run(resolve(
        env, _Request(url=url), {"body": "something else"})) == ("", None)
    assert asyncio.run(resolve(env, _Request(url=url), {})) == ("", None)
    replayed = _signed_url(
        namespace, "/api/tasks/%s/replies" % OTHER_TASK_ID,
        "ORG_TASK_REPLY_PROOF", resource=TASK_ID + "\n" + digest(body))
    assert asyncio.run(resolve(
        env, _Request(url=replayed), dict(body))) == ("", None)

    history = _signed_url(
        namespace, "/api/tasks/%s/history" % TASK_ID,
        "ORG_TASK_HISTORY_PROOF", resource=TASK_ID)
    assert asyncio.run(resolve(
        env, _Request(method="GET", url=history)))[0] == "bi:alice"
    assert asyncio.run(resolve(
        env, _Request(method="POST", url=history), {})) == ("", None)
    other = _signed_url(
        namespace, "/api/tasks/%s/history" % OTHER_TASK_ID,
        "ORG_TASK_HISTORY_PROOF", resource=TASK_ID)
    assert asyncio.run(resolve(
        env, _Request(method="GET", url=other))) == ("", None)

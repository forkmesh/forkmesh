#!/usr/bin/env python3
"""Identity key rotation (issue #368): the account rebinds to a successor key
only when the *currently bound* key signs the rotation."""

import ast
import asyncio
from pathlib import Path

from worker_test_helpers import json_from_request_double


ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"


def _load_rotate(extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name == "_account_rotate"
    ]
    assert selected, "missing _account_rotate"
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_account_rotate"]


class _Request:
    def __init__(self, body):
        self._body = body

    async def json(self):
        return self._body


def _json_response(data, status=200, **_kwargs):
    return {"status": status, "data": data}


def _harness(rec, pubkey_lookup=None, expected_canonical=None,
             revoked_keys=(), device_accounts=(), proven_users=()):
    saved = []
    verified = []
    revoked = []
    device_writes = []

    async def _account_row(_env, name):
        if rec is None:
            return "bi:" + name, None
        return "bi:" + name, dict(rec)

    async def _account_row_by_pubkey(_env, pubkey):
        if pubkey_lookup and pubkey in pubkey_lookup:
            return pubkey_lookup[pubkey]
        return None, None

    async def _save_account(_env, name_bi, updated_rec, **_kwargs):
        saved.append((name_bi, dict(updated_rec)))

    async def _account_revoke_sessions(_env, name_bi):
        revoked.append(name_bi)

    async def d1_run(_env, sql, *args):
        device_writes.append((sql, args))

    async def _account_key_revoked(_env, account_bi, pubkey):
        return (account_bi, pubkey) in set(revoked_keys)

    async def d1_all(_env, sql, *args):
        assert sql == "SELECT account_bi FROM account_devices WHERE pubkey=?"
        assert args == ("new-pubkey",)
        return [{"account_bi": bi} for bi in device_accounts]

    async def _device_proven_user_bis(_env, pubkey):
        assert pubkey == "new-pubkey"
        return set(proven_users)

    async def blind_index(_env, value):
        return "bi:" + value

    async def ed25519_verify(pubkey, sig, canonical):
        # Only the currently-bound old key with the sentinel signature verifies.
        verified.append((pubkey, sig, canonical))
        if pubkey == "new-pubkey":
            return sig == "goodnewsig" and canonical == (
                b"forkmesh-rotate-accept-v1\nalice-node\nold-pubkey\n"
                b"new-pubkey\n1783000000000")
        if sig != "goodsig" or pubkey != "old-pubkey":
            return False
        if expected_canonical is not None:
            return canonical == expected_canonical
        return True

    class _Date:
        @staticmethod
        def now():
            return 1783000000000

    handler = _load_rotate(
        {
            "clean_string": lambda v, n=240: str(v or "")[:n],
            "MAX_NODE_NAME": 32,
            "valid_node_name": lambda v: bool(v),
            "valid_node_pubkey": lambda v: bool(v) and v != "invalid",
            "_ts_ok": lambda ts: ts != "stale",
            "_account_row": _account_row,
            "_account_row_by_pubkey": _account_row_by_pubkey,
            "_save_account": _save_account,
            "_account_revoke_sessions": _account_revoke_sessions,
            "d1_run": d1_run,
            "d1_all": d1_all,
            "_device_proven_user_bis": _device_proven_user_bis,
            "blind_index": blind_index,
            "_account_key_revoked": _account_key_revoked,
            "ed25519_verify": ed25519_verify,
            "json_response": _json_response,
            "Date": _Date,
        }
    )
    handler.revoked_sessions = revoked
    handler.device_writes = device_writes
    return handler, saved, verified


def _bound_account(**overrides):
    rec = {"name": "alice-node", "status": "active", "pubkey": "old-pubkey"}
    rec.update(overrides)
    return rec


def _run(handler, body):
    return asyncio.run(handler(object(), _Request(body)))


def test_rotate_rebinds_to_successor_when_old_key_signs():
    handler, saved, _verified = _harness(_bound_account())
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 200
    assert resp["data"] == {"ok": True, "nodeName": "alice-node", "pubkey": "new-pubkey"}
    assert len(saved) == 1
    _, rec = saved[0]
    assert rec["pubkey"] == "new-pubkey"
    assert rec["prev_pubkeys"] == ["old-pubkey"]
    assert rec["rotated_at"] == 1783000000000
    assert handler.revoked_sessions == ["bi:alice-node"]
    assert handler.device_writes == [
        (
            "UPDATE account_devices SET revoked_at=?, enabled=0 "
            "WHERE account_bi=? AND pubkey=? AND revoked_at=0",
            (1783000000000, "bi:alice-node", "old-pubkey"),
        )
    ]


def test_rotate_accepts_desktop_rotation_record_signature_field():
    canonical = b"forkmesh-rotate-v1\nold-pubkey\nnew-pubkey\n1783000000000"
    handler, saved, _verified = _harness(_bound_account(), expected_canonical=canonical)
    resp = _run(handler, {
        "kind": "forkmesh.rotate",
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "signature": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 200
    assert resp["data"] == {"ok": True, "nodeName": "alice-node", "pubkey": "new-pubkey"}
    assert len(saved) == 1
    _, rec = saved[0]
    assert rec["pubkey"] == "new-pubkey"
    assert handler.revoked_sessions == ["bi:alice-node"]


def test_rotate_accepts_desktop_rotation_record_without_node_name():
    canonical = b"forkmesh-rotate-v1\nold-pubkey\nnew-pubkey\n1783000000000"
    handler, saved, _verified = _harness(
        _bound_account(),
        {"old-pubkey": ("bi:alice-node", _bound_account())},
        expected_canonical=canonical,
    )
    resp = _run(handler, {
        "kind": "forkmesh.rotate",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "signature": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 200
    assert resp["data"] == {"ok": True, "nodeName": "alice-node", "pubkey": "new-pubkey"}
    assert saved == [
        (
            "bi:alice-node",
            {
                "name": "alice-node",
                "status": "active",
                "pubkey": "new-pubkey",
                "prev_pubkeys": ["old-pubkey"],
                "rotated_at": 1783000000000,
            },
        )
    ]
    assert handler.revoked_sessions == ["bi:alice-node"]


def test_rotate_retries_desktop_record_after_nameless_rotation():
    canonical = b"forkmesh-rotate-v1\nold-pubkey\nnew-pubkey\n1783000000000"
    rotated = _bound_account(pubkey="new-pubkey", prev_pubkeys=["old-pubkey"])
    handler, saved, verified = _harness(
        None,
        {"new-pubkey": ("bi:alice-node", rotated)},
        expected_canonical=canonical,
    )
    resp = _run(handler, {
        "kind": "forkmesh.rotate",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "signature": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 200
    assert resp["data"] == {"ok": True, "nodeName": "alice-node", "pubkey": "new-pubkey"}
    assert saved == []
    assert verified == [("old-pubkey", "goodsig", canonical)]
    assert handler.revoked_sessions == []


def test_rotate_rejects_successor_key_bound_to_another_account():
    handler, saved, _verified = _harness(
        _bound_account(),
        {
            "new-pubkey": (
                "bi:bob-node",
                _bound_account(name="bob-node", pubkey="new-pubkey"),
            )
        },
    )
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 409
    assert resp["data"]["error"] == "pubkey_taken"
    assert saved == []


def test_rotate_rejects_signature_from_a_non_bound_key():
    handler, saved, _verified = _harness(_bound_account())
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "attacker-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 403
    assert resp["data"]["error"] == "not_bound"
    assert saved == []


def test_rotate_rejects_bad_signature_from_the_bound_key():
    handler, saved, _verified = _harness(_bound_account())
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "forged",
    })
    assert resp["status"] == 401
    assert resp["data"]["error"] == "bad_signature"
    assert saved == []


def test_rotate_is_idempotent_when_already_bound_to_successor():
    handler, saved, _verified = _harness(_bound_account(pubkey="new-pubkey"))
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "forged",
    })
    assert resp["status"] == 200
    assert resp["data"] == {"ok": True, "nodeName": "alice-node", "pubkey": "new-pubkey"}
    assert saved == []


def test_rotate_rejects_stale_request():
    handler, saved, _verified = _harness(_bound_account())
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "stale",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 401
    assert resp["data"]["error"] == "stale_request"
    assert saved == []


def test_rotate_requires_an_existing_account():
    handler, saved, _verified = _harness(None)
    resp = _run(handler, {
        "nodeName": "ghost-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 404
    assert resp["data"]["error"] == "no_account"
    assert saved == []


def test_rotate_refuses_a_bound_key_that_was_revoked_as_a_device():
    handler, saved, _verified = _harness(
        _bound_account(), revoked_keys=[("bi:alice-node", "old-pubkey")])
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 401
    assert resp["data"]["error"] == "device_revoked"
    assert saved == []
    assert handler.revoked_sessions == []


def test_rotate_refuses_to_bind_a_revoked_successor_key():
    handler, saved, _verified = _harness(
        _bound_account(), revoked_keys=[("bi:alice-node", "new-pubkey")])
    resp = _run(handler, {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    })
    assert resp["status"] == 401
    assert resp["data"]["error"] == "device_revoked"
    assert saved == []


def _rotation_body(**overrides):
    body = {
        "nodeName": "alice-node",
        "oldPubkey": "old-pubkey",
        "newPubkey": "new-pubkey",
        "ts": "1783000000000",
        "sig": "goodsig",
        "newSig": "goodnewsig",
    }
    body.update(overrides)
    return {key: value for key, value in body.items() if value is not None}


def test_rotate_requires_the_new_key_to_sign_the_acceptance():
    handler, saved, _verified = _harness(_bound_account())
    resp = _run(handler, _rotation_body(newSig=None))
    assert resp["status"] == 426
    assert resp["data"]["error"] == "new_key_proof_required"
    assert saved == []
    assert handler.revoked_sessions == []


def test_rotate_rejects_an_acceptance_not_signed_by_the_new_key():
    handler, saved, verified = _harness(_bound_account())
    resp = _run(handler, _rotation_body(newSig="goodsig"))
    assert resp["status"] == 401
    assert resp["data"]["error"] == "bad_new_key_signature"
    assert verified[-1] == (
        "new-pubkey", "goodsig",
        b"forkmesh-rotate-accept-v1\nalice-node\nold-pubkey\nnew-pubkey\n"
        b"1783000000000")
    assert saved == []


def test_rotate_refuses_a_key_registered_as_another_accounts_device():
    handler, saved, _verified = _harness(
        _bound_account(), device_accounts=["bi:alice-node", "bi:bob"])
    resp = _run(handler, _rotation_body())
    assert resp["status"] == 409
    assert resp["data"]["error"] == "device_key_conflict"
    assert saved == []


def test_rotate_refuses_a_key_proven_by_another_user():
    handler, saved, _verified = _harness(
        _bound_account(), proven_users=["bi:bob"])
    resp = _run(handler, _rotation_body())
    assert resp["status"] == 409
    assert resp["data"]["error"] == "device_key_conflict"
    assert saved == []


def test_rotate_allows_the_nodes_own_owner_device_key():
    handler, saved, _verified = _harness(
        _bound_account(owner="alice"),
        device_accounts=["bi:alice"], proven_users=["bi:alice"])
    resp = _run(handler, _rotation_body())
    assert resp["status"] == 200
    assert saved[0][1]["pubkey"] == "new-pubkey"

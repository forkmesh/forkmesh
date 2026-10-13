#!/usr/bin/env python3
"""Administrator override for an email the recipient never confirmed.

Contract under test, extracted straight from entry.py:
- POST /api/accounts/admin-verify-email marks the address verified only for a
  caller who proved the administrator signature the canonical string binds;
- the override is recorded as ``admin_override`` and names the administrator,
  so it is never mistaken for the recipient having pressed the emailed link;
- verifying drains the account's row from the manual confirmation queue, which
  is what the "Email confirmations" dialog lists;
- an account with nothing to verify (no address, not active) is refused, and a
  second press on an already-verified account is a no-op that still clears a
  stale queue row.
"""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")

NOW = 2_000_000_000_000


def _load(extra):
    tree = ast.parse(TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == "_admin_verify_email"
    ]
    assert len(selected) == 1
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _harness(rec=None, authorized=True):
    """One admin-verify runtime over a single target account record."""
    state = {
        "saved": {},
        "audits": [],
        "queries": [],
        "proofs": [],
        "finalized": [],
    }

    class _DateStub:
        @staticmethod
        def now():
            return NOW

    def json_response(body, status=200, **_kwargs):
        return {"status": status, "body": body}

    async def bounded_json_request(request):
        return dict(request.payload)

    async def _admin_authorized(_env, node, ts, sig, canonical):
        state["proofs"].append((node, ts, sig, canonical))
        return {"name": node} if authorized else None

    async def _account_row(_env, name):
        return ("bi:" + name, dict(rec)) if rec is not None else ("", None)

    async def _save_account(_env, account_bi, record):
        state["saved"][account_bi] = dict(record)

    async def _finalize_referral_signup(_env, account_bi):
        state["finalized"].append(account_bi)
        return "pending"

    async def d1_run(_env, sql, *args):
        state["queries"].append((sql, args))

    async def _audit_sensitive_action(
            _env, actor, action, target_type="", target="",
            outcome="success", details=None):
        state["audits"].append({
            "actor": actor, "action": action, "target": target,
            "outcome": outcome, "details": dict(details or {}),
        })

    namespace = _load({
        "Date": _DateStub,
        "MAX_NODE_NAME": 64,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "_account_kind": lambda record: record.get("kind", "user"),
        "json_response": json_response,
        "bounded_json_request": bounded_json_request,
        "_admin_authorized": _admin_authorized,
        "_account_row": _account_row,
        "_save_account": _save_account,
        "_finalize_referral_signup": _finalize_referral_signup,
        "d1_run": d1_run,
        "_audit_sensitive_action": _audit_sensitive_action,
    })
    return namespace, state


def _call(namespace, target="carol", node="alice"):
    request = SimpleNamespace(payload={
        "node": node, "target": target, "ts": str(NOW), "sig": "signature"})
    return asyncio.run(namespace["_admin_verify_email"](object(), request))


def _unverified(**overrides):
    rec = {"name": "carol", "kind": "user", "status": "active",
           "email": "carol@example.test", "email_verified": False}
    rec.update(overrides)
    return rec


def test_admin_marks_the_address_verified_and_records_the_override():
    namespace, state = _harness(_unverified())

    response = _call(namespace)

    assert response["status"] == 200
    assert response["body"] == {
        "ok": True, "target": "carol", "emailVerified": True,
        "alreadyVerified": False}
    saved = state["saved"]["bi:carol"]
    assert saved["email_verified"] is True
    assert saved["email_verified_at"] == NOW
    # The provenance is the whole point: an administrator vouched, nobody
    # proved control of the mailbox.
    assert saved["email_verification_method"] == "admin_override"
    assert saved["email_verified_by"] == "alice"
    assert state["finalized"] == ["bi:carol"]
    audit = state["audits"][-1]
    assert audit["actor"] == "alice"
    assert audit["action"] == "admin.email_verify"
    assert audit["outcome"] == "success"
    assert audit["details"] == {
        "method": "admin_override", "alreadyVerified": False}


def test_verifying_drains_the_manual_confirmation_queue():
    namespace, state = _harness(_unverified())

    _call(namespace)

    assert state["queries"] == [
        ("DELETE FROM pending_verifications WHERE name_bi=?", ("bi:carol",))]


def test_the_signature_binds_the_administrator_and_the_target():
    namespace, state = _harness(_unverified())

    _call(namespace)

    node, ts, sig, canonical = state["proofs"][-1]
    assert (node, ts, sig) == ("alice", str(NOW), "signature")
    assert canonical == (
        b"forkmesh-admin-verify-email-v1\nalice\ncarol\n" + str(NOW).encode())


def test_an_unproven_caller_changes_nothing():
    namespace, state = _harness(_unverified(), authorized=False)

    response = _call(namespace)

    assert response == {"status": 401, "body": {"error": "unauthorized"}}
    assert not state["saved"]
    assert not state["queries"]
    assert not state["audits"]


def test_an_account_with_no_address_or_no_record_is_refused():
    namespace, state = _harness(_unverified(email=""))
    response = _call(namespace)
    assert response["status"] == 409
    assert response["body"] == {"error": "account_has_no_email"}
    assert state["audits"][-1]["details"] == {"reason": "account_has_no_email"}
    assert not state["saved"]

    namespace, state = _harness(None)
    response = _call(namespace)
    assert response["status"] == 404
    assert response["body"] == {"error": "no_such_user"}
    assert state["audits"][-1]["details"] == {"reason": "no_such_user"}
    assert not state["saved"]

    namespace, state = _harness(_unverified())
    response = _call(namespace, target="not a name")
    assert response == {"status": 400, "body": {"error": "invalid_account"}}
    assert not state["saved"]

    namespace, state = _harness(_unverified(kind="node"))
    response = _call(namespace)
    assert response == {"status": 404, "body": {"error": "no_such_user"}}
    assert not state["saved"]


def test_a_second_press_rewrites_nothing_but_still_clears_the_queue():
    namespace, state = _harness(_unverified(email_verified=True))

    response = _call(namespace)

    assert response["status"] == 200
    assert response["body"]["alreadyVerified"] is True
    # Re-saving would restamp a genuine recipient confirmation as an override.
    assert not state["saved"]
    assert not state["finalized"]
    assert state["queries"] == [
        ("DELETE FROM pending_verifications WHERE name_bi=?", ("bi:carol",))]
    assert state["audits"][-1]["details"] == {
        "method": "admin_override", "alreadyVerified": True}

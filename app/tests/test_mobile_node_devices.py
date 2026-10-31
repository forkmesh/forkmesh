#!/usr/bin/env python3
"""Mobile node binding and the owner-facing node registry (Worker side).

Contract: www/docs/mobile/mobile-node-contract.md sections 2-5. A phone binds
its Ed25519 key on a normal password login with ``deviceKind: mobile_node``;
the key lands in account_devices with the reduced MOBILE_NODE_CAPABILITIES,
never becomes the account's primary key, signs as the owner while enabled,
and can be revoked (with every session it opened) from the web registry.
"""

import ast
import asyncio
import base64
import hashlib
import hmac
import json
import re
import sqlite3
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
SCHEMA = ROOT / "src" / "schema.py"
TEXT = ENTRY.read_text(encoding="utf-8")
TREE = ast.parse(TEXT, filename=str(ENTRY))

DESKTOP_CAPS = (
    "browse,comment,submit_issue,submit_pr,host_repo,mirror_repo,"
    "publish_repo,owner_sign")
MOBILE_CAPS = "browse,comment,submit_issue,submit_pr,publish_repo,owner_sign"
CLIENT_CAPS = "browse,comment,submit_issue,submit_pr"

PHONE_KEY = base64.urlsafe_b64encode(b"\x21" * 32).decode().rstrip("=")
DESKTOP_KEY = base64.urlsafe_b64encode(b"\x22" * 32).decode().rstrip("=")
DEVICE_TS = "1783000000000"
DEVICE_SIG = "device-proof-signature"
NOW = int(DEVICE_TS)


def _nodes(names):
    selected = [
        node for node in TREE.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in names
    ]
    missing = set(names) - {node.name for node in selected}
    assert not missing, missing
    return selected


def _literal_assignment(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name
                for t in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(name)


def _load(names, extra_globals):
    module = ast.fix_missing_locations(
        ast.Module(body=_nodes(names), type_ignores=[]))
    namespace = {"json": json, "re": re, "hmac": hmac, "urlparse": urlparse,
                 **extra_globals}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _clean_string(value, max_length=240):
    return str(value or "")[:max_length]


def _json_response(data, status=200, extra_headers=None, **_kwargs):
    return {"status": status, "data": data, "headers": extra_headers or {}}


class _Date:
    @staticmethod
    def now():
        return NOW


# ---------------------------------------------------------------------------
# Constants and registration
# ---------------------------------------------------------------------------


def test_mobile_node_capabilities_constant_matches_contract():
    constants = _literal_assignment_all()
    assert constants["MOBILE_NODE_CAPABILITIES"] == MOBILE_CAPS
    assert constants["DESKTOP_NODE_CAPABILITIES"] == DESKTOP_CAPS
    assert constants["CLIENT_CAPABILITIES"] == CLIENT_CAPS
    assert "host_repo" not in MOBILE_CAPS.split(",")
    assert "mirror_repo" not in MOBILE_CAPS.split(",")


def _literal_assignment_all():
    out = {}
    for name in ("MOBILE_NODE_CAPABILITIES", "DESKTOP_NODE_CAPABILITIES",
                 "CLIENT_CAPABILITIES"):
        out[name] = _literal_assignment(ENTRY, name)
    return out


def _register_harness():
    rows = []

    async def blind_index(_env, value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    async def d1_run(_env, sql, *args):
        assert "INSERT INTO account_devices" in sql
        rows.append({
            "device_bi": args[0], "account_bi": args[1], "pubkey": args[2],
            "kind": args[3], "label": args[4], "capabilities": args[5],
        })

    ns = _load({"_register_account_device"}, {
        "blind_index": blind_index,
        "d1_run": d1_run,
        "Date": _Date,
        "DESKTOP_NODE_CAPABILITIES": DESKTOP_CAPS,
        "MOBILE_NODE_CAPABILITIES": MOBILE_CAPS,
        "CLIENT_CAPABILITIES": CLIENT_CAPS,
        "CHAT_NODE_DEVICE_KIND": "chat_node",
        "CHAT_NODE_CAPABILITIES": "chat_node",
        "CONTRIBUTION_KEY_PROOF_CAPABILITY": "contribution_key_proof",
    })
    return ns["_register_account_device"], rows


@pytest.mark.parametrize("kind,expected", [
    ("desktop_node", DESKTOP_CAPS),
    ("mobile_node", MOBILE_CAPS),
    ("chat_node", "chat_node"),
    ("web_or_mobile", CLIENT_CAPS),
    ("anything-else", CLIENT_CAPS),
])
def test_register_account_device_maps_kind_to_capabilities(kind, expected):
    register, rows = _register_harness()
    result = asyncio.run(register(
        object(), "account-bi", PHONE_KEY, kind, "label",
        proof_verified=True))
    assert rows[0]["kind"] == kind
    assert rows[0]["capabilities"] == expected + ",contribution_key_proof"
    assert result["capabilities"] == (
        expected + ",contribution_key_proof").split(",")
    assert result["id"] == rows[0]["device_bi"][:16]


# ---------------------------------------------------------------------------
# Login (bind)
# ---------------------------------------------------------------------------


class _Request:
    def __init__(self, body, url="https://forkmesh.test/api/accounts/login",
                 method="POST", headers=None):
        self._body = body
        self.url = url
        self.method = method
        self.headers = headers or {}

    async def json(self):
        return self._body


def _active_account(**overrides):
    rec = {
        "name": "alice-node",
        "email": "alice@example.com",
        "status": "active",
        "pass_salt": "salt",
        "pass_hash": "hash",
        "pubkey": "",
        "email_verified": True,
    }
    rec.update(overrides)
    return rec


LOGIN_FUNCTIONS = {
    "_account_login",
    "_account_admin_totp_enrollment",
    "_account_login_mobile_node_response",
    "_ensure_local_demo_account",
    "_is_local_demo_request",
    "_register_account_device",
    "_account_device_for_pubkey",
    "_account_devices_list",
    "_account_device_public",
    "_with_session_capabilities",
    "_device_bind_canonical",
    "_device_proven_user_bis",
    "_device_retire_unproven_claims",
    "_account_key_revoked",
}


def _login_harness(rec, *, initial_devices=None):
    saved = []
    device_rows = [dict(row) for row in (initial_devices or [])]
    audits = []
    notifications = []
    payload_calls = []
    touched = []

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def bounded_json_request(request):
        return await request.json()

    async def d1_first(_env, sql, *args):
        if "FROM users WHERE email_bi" in sql:
            return ({"data": "encrypted"}
                    if args == ("bi:alice@example.com",) else None)
        if "FROM account_devices" in sql:
            for row in device_rows:
                if args == (row["account_bi"], row["pubkey"]):
                    return dict(row)
        return None

    async def d1_all(_env, sql, *args):
        if "proven_device_users" in sql:
            resolved = {
                row["account_bi"] for row in device_rows
                if row.get("pubkey") == args[0]
                and "contribution_key_proof" in
                row.get("capabilities", "").split(",")
            }
            return [{"user_bi": value} for value in sorted(resolved)]
        if "FROM account_devices" in sql:
            return [dict(row) for row in device_rows
                    if args == (row["account_bi"],)]
        return []

    async def d1_run(_env, sql, *args):
        if "INSERT INTO account_devices" in sql:
            row = {
                "device_bi": args[0],
                "account_bi": args[1],
                "pubkey": args[2],
                "kind": args[3],
                "label": args[4],
                "capabilities": args[5],
                "enabled": 1,
                "created_at": args[6],
                "last_seen": args[7],
                "revoked_at": 0,
            }
            existing = next((item for item in device_rows
                             if item["device_bi"] == row["device_bi"]), None)
            if existing:
                # Mirrors the ON CONFLICT clause: kind/created_at are kept,
                # a revoked row never comes back.
                existing["last_seen"] = row["last_seen"]
                existing["capabilities"] = row["capabilities"]
                if not existing.get("revoked_at"):
                    existing["enabled"] = 1
            else:
                device_rows.append(row)
        elif "DELETE FROM account_devices" in sql and args:
            pubkey = args[0]
            account_bi = args[1] if len(args) > 1 else None
            device_rows[:] = [
                row for row in device_rows
                if not (row.get("pubkey") == pubkey
                        and row.get("account_bi") != account_bi
                        and "contribution_key_proof" not in
                        row.get("capabilities", "").split(","))
            ]

    async def decrypt_row(_env, _data):
        return dict(rec)

    async def _account_row(_env, name):
        return "bi:" + name, dict(rec)

    async def verify_password(password, _salt, _hash):
        return password == "correct horse battery staple"

    async def totp_verify(_secret, _totp):
        return True

    async def ed25519_verify(pubkey, signature, canonical):
        expected = (
            "forkmesh-device-bind-v1\n"
            "alice-node\n" + pubkey + "\n" + DEVICE_TS
        ).encode()
        return signature == DEVICE_SIG and canonical == expected

    async def _login_attempt_keys(env, _request, identifier):
        return [await blind_index(env, identifier)]

    async def _login_failure_response(_env, _attempt_keys, error):
        return _json_response({"error": error}, status=401)

    async def _login_clear(_env, _id_bi):
        return None

    async def _save_account(_env, name_bi, updated_rec, **_kwargs):
        saved.append((name_bi, dict(updated_rec)))

    async def touch_registered_node(_env, name_bi, _rec=None):
        touched.append(name_bi)

    async def _account_public_payload(_env, account, _session_token=None,
                                      **kwargs):
        payload_calls.append(dict(kwargs))
        return {
            "ok": True,
            "nodeName": account.get("name", ""),
            "pubkey": account.get("pubkey", ""),
            "sessionToken": "v2.test-session.test-secret",
        }

    async def _audit_sensitive_action(_env, actor, action, target_type="",
                                      target="", outcome="success",
                                      details=None):
        audits.append((actor, action, target_type, target, outcome,
                       dict(details or {})))

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        notifications.append((recipient, kind, title, dict(kwargs)))
        return True

    async def _account_session_response(
            _env, _request, payload, status=200):
        return _json_response(
            payload, status=status,
            extra_headers={"Set-Cookie": "cookie:" + payload["sessionToken"]})

    async def _is_admin_flag(_env, _name):
        return False

    ns = _load(LOGIN_FUNCTIONS, {
        "_is_admin_flag": _is_admin_flag,
        "blind_index": blind_index,
        "bounded_json_request": bounded_json_request,
        "clean_string": _clean_string,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "_account_row": _account_row,
        "verify_password": verify_password,
        "valid_node_name": lambda value: bool(value),
        "totp_verify": totp_verify,
        "_account_totp_ok": lambda _env, rec, code: totp_verify(
            rec.get("totp_secret", ""), code),
        "ed25519_verify": ed25519_verify,
        "_ts_ok": lambda value: value == DEVICE_TS,
        "valid_node_pubkey": lambda value: (
            isinstance(value, str) and len(value) == 43),
        "_login_attempt_keys": _login_attempt_keys,
        "_login_failure_response": _login_failure_response,
        "_login_clear": _login_clear,
        "_save_account": _save_account,
        "touch_registered_node": touch_registered_node,
        "_account_public_payload": _account_public_payload,
        "_account_session_device_label": (
            lambda _request, desktop=False:
            "Desktop node" if desktop else "Web browser"),
        "_account_session_client_ip": lambda _request: "203.0.113.9",
        "_account_session_cookie": lambda token: "cookie:" + token,
        "_account_session_response": _account_session_response,
        "_audit_sensitive_action": _audit_sensitive_action,
        "enqueue_notification": enqueue_notification,
        "json_response": _json_response,
        "Date": _Date,
        "DESKTOP_NODE_CAPABILITIES": DESKTOP_CAPS,
        "MOBILE_NODE_CAPABILITIES": MOBILE_CAPS,
        "CLIENT_CAPABILITIES": CLIENT_CAPS,
        "CONTRIBUTION_KEY_PROOF_CAPABILITY": "contribution_key_proof",
        "MAX_NODE_NAME": 64,
    })
    return SimpleNamespace(
        handler=ns["_account_login"], saved=saved, devices=device_rows,
        audits=audits, notifications=notifications,
        payload_calls=payload_calls, touched=touched)


def _login(harness, **body):
    payload = {
        "email": "alice@example.com",
        "password": "correct horse battery staple",
    }
    payload.update(body)
    return asyncio.run(harness.handler(object(), _Request(payload)))


def _mobile_body(label="iPhone 15 Pro"):
    return {
        "pubkey": PHONE_KEY,
        "deviceTs": DEVICE_TS,
        "deviceSig": DEVICE_SIG,
        "deviceKind": "mobile_node",
        "deviceLabel": label,
    }


def test_mobile_node_login_registers_reduced_capabilities_without_primary_key():
    harness = _login_harness(_active_account())
    response = _login(harness, **_mobile_body())

    assert response["status"] == 200, response
    data = response["data"]
    # The device row.
    assert len(harness.devices) == 1
    device = harness.devices[0]
    assert device["kind"] == "mobile_node"
    assert device["pubkey"] == PHONE_KEY
    assert device["label"] == "iPhone 15 Pro"
    assert device["capabilities"] == MOBILE_CAPS + ",contribution_key_proof"
    # Never promoted to the account's primary key, never a hosting node.
    assert harness.saved == []
    assert harness.touched == []
    assert data["pubkey"] == ""
    # Session shape for a mobile bind.
    assert data["sessionKind"] == "mobile_node"
    assert data["deviceKind"] == "mobile_node"
    assert data["deviceKeyMatched"] is True
    assert data["desktopCapable"] is False
    assert data["capabilities"] == (
        MOBILE_CAPS + ",contribution_key_proof").split(",")
    assert data["canHost"] is False
    assert data["canPublish"] is True
    assert data["canOwnerSign"] is True
    assert data["deviceId"] == device["device_bi"][:16]
    assert len(data["deviceId"]) == 16
    assert data["devices"] == [{
        "id": device["device_bi"][:16],
        "kind": "mobile_node",
        "label": "iPhone 15 Pro",
        "capabilities": (MOBILE_CAPS + ",contribution_key_proof").split(","),
        "enabled": True,
        "lastSeen": NOW,
        "createdAt": NOW,
        "revokedAt": 0,
        "current": True,
    }]
    assert "pubkey" not in data["devices"][0]
    assert response["headers"]["Set-Cookie"] == (
        "cookie:v2.test-session.test-secret")
    # The minted session is tied to the device row.
    assert harness.payload_calls[-1]["session_device_bi"] == (
        device["device_bi"])
    assert harness.payload_calls[-1]["session_device_label"] == "Mobile node"
    # First bind of a new row: audit (no pubkey) + in-app ping.
    assert harness.audits == [(
        "alice-node", "account.device_bind", "account_device",
        device["device_bi"][:16], "success",
        {"kind": "mobile_node", "label": "iPhone 15 Pro"},
    )]
    assert PHONE_KEY not in json.dumps(harness.audits)
    assert len(harness.notifications) == 1
    recipient, kind, title, extra = harness.notifications[0]
    assert (recipient, kind) == ("alice-node", "device_added")
    assert "iPhone 15 Pro" in extra["body"]
    assert extra["dedupe"] == "device_added:" + device["device_bi"][:16]


def test_mobile_node_rebind_of_known_key_is_quiet():
    harness = _login_harness(_active_account())
    assert _login(harness, **_mobile_body())["status"] == 200
    response = _login(harness, **_mobile_body())
    assert response["status"] == 200
    assert len(harness.devices) == 1
    # Only the first creation of the row is audited/pinged.
    assert len(harness.audits) == 1
    assert len(harness.notifications) == 1


def test_mobile_bind_notification_failure_never_fails_login():
    harness = _login_harness(_active_account())

    async def boom(*_args, **_kwargs):
        raise RuntimeError("notifications down")

    harness.handler.__globals__["enqueue_notification"] = boom
    harness.handler.__globals__["_audit_sensitive_action"] = boom
    response = _login(harness, **_mobile_body())
    assert response["status"] == 200
    assert response["data"]["sessionKind"] == "mobile_node"


def test_desktop_login_after_mobile_bind_still_becomes_primary_and_capable():
    harness = _login_harness(_active_account())
    assert _login(harness, **_mobile_body())["status"] == 200

    response = _login(
        harness, pubkey=DESKTOP_KEY, deviceTs=DEVICE_TS,
        deviceSig=DEVICE_SIG, deviceLabel="Studio Mac")
    assert response["status"] == 200, response
    data = response["data"]
    # The first desktop key is still promoted to the primary key.
    assert harness.saved and harness.saved[-1][1]["pubkey"] == DESKTOP_KEY
    assert harness.touched == ["bi:alice-node"]
    assert data["sessionKind"] == "desktop_node"
    assert data["deviceKind"] == "desktop_node"
    assert data["desktopCapable"] is True
    assert data["canHost"] is True
    assert "deviceId" not in data
    desktop = next(d for d in harness.devices if d["pubkey"] == DESKTOP_KEY)
    assert desktop["kind"] == "desktop_node"
    assert desktop["capabilities"] == DESKTOP_CAPS + ",contribution_key_proof"
    # Desktop sessions are bound to their device row too.
    assert harness.payload_calls[-1]["session_device_bi"] == (
        desktop["device_bi"])
    # The phone row is untouched by the desktop bind.
    phone = next(d for d in harness.devices if d["pubkey"] == PHONE_KEY)
    assert phone["kind"] == "mobile_node"
    # Desktop binds are not audited/pinged by this change (unchanged path).
    assert len(harness.audits) == 1
    assert len(harness.notifications) == 1


def test_explicit_desktop_node_kind_matches_legacy_desktop_path():
    harness = _login_harness(_active_account())
    response = _login(
        harness, pubkey=DESKTOP_KEY, deviceTs=DEVICE_TS,
        deviceSig=DEVICE_SIG, deviceKind="desktop_node")
    assert response["status"] == 200
    assert response["data"]["sessionKind"] == "desktop_node"
    assert response["data"]["desktopCapable"] is True
    assert harness.devices[0]["kind"] == "desktop_node"


@pytest.mark.parametrize("kind", ["phone", "web_or_mobile", "MOBILE_NODE",
                                  "mobile_node ", "desktop"])
def test_unknown_device_kind_is_rejected(kind):
    harness = _login_harness(_active_account())
    response = _login(harness, **{**_mobile_body(), "deviceKind": kind})
    assert response == {
        "status": 400, "data": {"error": "invalid_device_kind"},
        "headers": {}}
    assert harness.devices == []


def test_mobile_login_still_requires_device_possession_proof():
    harness = _login_harness(_active_account())
    bad = _mobile_body()
    bad["deviceSig"] = "forged"
    response = _login(harness, **bad)
    assert response["status"] == 401
    assert response["data"] == {"error": "device_proof_required"}
    assert harness.devices == []


def test_mobile_login_with_revoked_key_is_refused_as_device_revoked():
    harness = _login_harness(_active_account())
    assert _login(harness, **_mobile_body())["status"] == 200
    harness.devices[0]["revoked_at"] = NOW - 1
    harness.devices[0]["enabled"] = 0
    response = _login(harness, **_mobile_body())
    assert response["status"] == 401
    assert response["data"] == {"error": "device_revoked"}
    # No session was minted and the row stays revoked.
    assert len(harness.payload_calls) == 1
    assert harness.devices[0]["enabled"] == 0


def test_plain_web_login_is_unchanged():
    harness = _login_harness(_active_account())
    response = _login(harness)
    assert response["status"] == 200
    assert response["data"]["sessionKind"] == "account"
    assert response["data"]["deviceKind"] == "web_or_mobile"
    assert "deviceId" not in response["data"]
    assert harness.payload_calls[-1].get("session_device_bi", "") == ""


# ---------------------------------------------------------------------------
# Registry routes, session binding and revocation on a real SQLite schema
# ---------------------------------------------------------------------------


REGISTRY_FUNCTIONS = {
    "_account_devices",
    "_account_device_check",
    "_account_device_check_signed",
    "_signed_account_session",
    "_ts_ok",
    "_account_sudo_reauth",
    "_account_devices_list",
    "_account_device_public",
    "_account_session_lookup",
    "_account_session_invalid_error",
    "_session_device_revoked",
    "_account_session_device_bi",
    "_account_revoke_device_sessions",
    "_account_revoke_sessions",
    "_account_session_token",
    "_account_session_token_digest",
    "_account_session_secret",
    "_register_account_device",
    "_owner_signing_pubkeys",
    "_account_signing_pubkeys",
    "_account_sessions",
    "_account_last_seen",
    "_account_email_activity",
    "_account_browser_control_bind",
    "_account_browser_control_pubkeys",
    "_account_desktops",
    "_account_desktops_handler",
    "_account_device_for_pubkey",
    "_account_key_revoked",
    "_account_drop_revoked_primary",
}


def _device_check_sig(pub, canonical):
    return "signed-by-" + pub + ":" + canonical.decode()


class _Registry:
    """Real handler code over an in-memory SQLite copy of the D1 tables."""

    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in _literal_assignment(SCHEMA, "SCHEMA_STATEMENTS"):
            if "account_devices" in statement or "account_sessions" in statement:
                self.db.execute(statement)
        self.db.execute(
            "CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT)")
        # The atomic device-proof claim joins the hosting-node table.
        self.db.execute(
            "CREATE TABLE nodes (node_bi TEXT PRIMARY KEY, user_bi TEXT, "
            "pubkey TEXT, data TEXT, name TEXT, last_seen INTEGER)")
        self.env = SimpleNamespace(ADMIN_PASS="admin", DATA_KEY="data")
        self.audits = []
        self.fails = []
        self.accounts = {}
        self.now = NOW
        registry = self

        async def blind_index(_env, value):
            return hashlib.sha256(str(value).encode()).hexdigest()

        async def d1_first(_env, sql, *args):
            row = registry.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_all(_env, sql, *args):
            return [dict(r) for r in registry.db.execute(sql, args)]

        async def d1_run(_env, sql, *args):
            registry.db.execute(sql, args)
            registry.db.commit()

        async def decrypt_row(_env, data):
            return json.loads(data) if data else None

        async def ensure_schema(_env):
            return None

        async def verify_password(password, _salt, _hash):
            return password == "correct horse battery staple"

        async def totp_verify(_secret, totp):
            return totp == "123456"

        async def _login_attempt_keys(_env, _request, identifier):
            return ["attempt:" + identifier]

        async def _login_failure_response(_env, attempt_keys, error):
            registry.fails.append((tuple(attempt_keys), error))
            return _json_response({"error": error}, status=401)

        async def _audit_sensitive_action(_env, actor, action, target_type="",
                                          target="", outcome="success",
                                          details=None):
            registry.audits.append(
                (actor, action, target_type, target, outcome,
                 dict(details or {})))

        async def _account_row(env, owner):
            rec = registry.accounts.get(owner)
            return (await blind_index(env, owner),
                    dict(rec) if rec else None)

        async def _save_account(_env, account_bi, rec, **_kwargs):
            assert account_bi == registry.bi(rec["name"])
            registry.accounts[rec["name"]] = dict(rec)

        async def bounded_json_request(request):
            return await request.json()

        def _request_account_session_token(request, payload=None):
            payload = payload if isinstance(payload, dict) else {}
            token = payload.get("sessionToken") or ""
            if not token:
                auth = request.headers.get("authorization") or ""
                if auth.lower().startswith("bearer "):
                    token = auth[7:]
            if token:
                return token, False
            return request.headers.get("cookie-token", ""), True

        counter = {"n": 0}

        def _random_bytes(n):
            counter["n"] += 1
            return (counter["n"].to_bytes(2, "big") * 64)[:n]

        async def _append_portable_session_cookie(
                _env, _request, response, _token, _rec,
                reuse_current=False):
            return response

        async def ed25519_verify(pub, sig, canonical):
            return sig == _device_check_sig(pub, canonical)

        self.ns = _load(REGISTRY_FUNCTIONS, {
            "ed25519_verify": ed25519_verify,
            "LOGIN_MAX_SKEW_MS": 5 * 60 * 1000,
            "ACCOUNT_DEVICE_CHECK_PROOF": _literal_assignment(
                ENTRY, "ACCOUNT_DEVICE_CHECK_PROOF"),
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_first_primary": d1_first,
            "d1_all": d1_all,
            "d1_run": d1_run,
            "decrypt_row": decrypt_row,
            "ensure_schema": ensure_schema,
            "verify_password": verify_password,
            "totp_verify": totp_verify,
            "_account_totp_ok": lambda _env, rec, code: totp_verify(
                rec.get("totp_secret", ""), code),
            "_login_attempt_keys": _login_attempt_keys,
            "_login_failure_response": _login_failure_response,
            "_audit_sensitive_action": _audit_sensitive_action,
            "_account_row": _account_row,
            "_save_account": _save_account,
            "bounded_json_request": bounded_json_request,
            "_request_account_session_token": _request_account_session_token,
            "_request_origin_allowed": lambda _env, _request: True,
            "_clear_account_session_cookie": lambda: "cleared-cookie",
            "_append_portable_session_cookie": (
                _append_portable_session_cookie),
            "_clear_all_account_cookies": (
                lambda _env, _request, response: response),
            "_account_kind": lambda rec: rec.get("kind", "user"),
            "_b64url_encode": lambda raw: base64.urlsafe_b64encode(
                raw).decode().rstrip("="),
            "_random_bytes": _random_bytes,
            "clean_string": _clean_string,
            "valid_node_name": lambda value: bool(value),
            "valid_node_pubkey": lambda value: (
                isinstance(value, str) and len(value) == 43),
            "method_name": lambda request: request.method,
            "json_response": _json_response,
            "Date": SimpleNamespace(now=lambda: registry.now),
            "ACCOUNT_SESSION_TOKEN_RE": re.compile(
                r"^v2\.([A-Za-z0-9_-]{24,64})\.([A-Za-z0-9_-]{32,96})$"),
            "ACCOUNT_SESSION_TOUCH_MS": 300_000,
            "ACCOUNT_SESSION_MAX_ACTIVE": 20,
            "ACCOUNT_SESSION_TTL_MS": 1826 * 24 * 60 * 60 * 1000,
            "ADMIN_SESSION_TTL_MS": 12 * 60 * 60 * 1000,
            "ACCOUNT_SESSION_COOKIE": "forkmesh_account",
            "ACCOUNT_DEVICE_KINDS": (
                "desktop_node", "mobile_node", "browser_control"),
            "BROWSER_CONTROL_CAPABILITIES": "desktop_control",
            "CHAT_NODE_DEVICE_KIND": "chat_node",
            "CHAT_NODE_CAPABILITIES": "chat_node",
            "BROWSER_CONTROL_BIND_PROOF": _literal_assignment(
                ENTRY, "BROWSER_CONTROL_BIND_PROOF"),
            "BROWSER_CONTROL_MAX_DEVICES": 3,
            "ACCOUNT_DEVICE_ID_RE": re.compile(r"[0-9a-fA-F]{8,64}"),
            "ACCOUNT_EMAIL_STATUS_DELIVERED": "delivered",
            "ACCOUNT_EMAIL_STATUS_FAILED": "failed",
            "DESKTOP_NODE_CAPABILITIES": DESKTOP_CAPS,
            "MOBILE_NODE_CAPABILITIES": MOBILE_CAPS,
            "CLIENT_CAPABILITIES": CLIENT_CAPS,
            "CONTRIBUTION_KEY_PROOF_CAPABILITY": "contribution_key_proof",
            "MAX_NODE_NAME": 64,
        })

    def _run(self, coro):
        return asyncio.run(coro)

    def account(self, name, **overrides):
        rec = {"name": name, "status": "active", "kind": "user",
               "pass_salt": "salt", "pass_hash": "hash", "pubkey": ""}
        rec.update(overrides)
        self.accounts[name] = rec
        account_bi = self.bi(name)
        self.db.execute(
            "INSERT OR REPLACE INTO users (user_bi, data) VALUES (?, ?)",
            (account_bi, json.dumps(rec)))
        self.db.commit()
        return account_bi

    def bi(self, value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    def bind(self, name, pubkey, kind, label=""):
        """Register a device and mint the session that bind would mint."""
        account_bi = self.bi(name)
        result = self._run(self.ns["_register_account_device"](
            self.env, account_bi, pubkey, kind, label, proof_verified=True))
        device_bi = self.bi(account_bi + ":" + pubkey)
        assert result["id"] == device_bi[:16]
        token = self._run(self.ns["_account_session_token"](
            self.env, name, "Mobile node" if kind == "mobile_node"
            else "Desktop node", "203.0.113.9", device_bi))
        return device_bi, token

    def web_session(self, name):
        return self._run(self.ns["_account_session_token"](
            self.env, name, "Web browser", "203.0.113.9"))

    def lookup(self, token):
        return self._run(self.ns["_account_session_lookup"](
            self.env, token, touch=False))

    def invalid_error(self, token):
        return self._run(self.ns["_account_session_invalid_error"](
            self.env, token))

    def devices(self, token, method="GET", target="", action="", body=None,
                cookie=False):
        headers = ({"cookie-token": token} if cookie
                   else {"authorization": "Bearer " + token})
        request = _Request(
            body or {}, url="https://forkmesh.test/api/accounts/devices",
            method=method, headers=headers)
        return self._run(self.ns["_account_devices"](
            self.env, request, target, action))

    def check_device(self, token, pubkey, cookie=False):
        headers = ({"cookie-token": token} if cookie
                   else {"authorization": "Bearer " + token})
        request = _Request(
            {"pubkey": pubkey},
            url="https://forkmesh.test/api/accounts/devices/check",
            method="POST", headers=headers)
        return self._run(self.ns["_account_device_check"](self.env, request))

    def check_device_signed(self, node, signer, pubkey, ts=None,
                            signed_pubkey=None):
        ts = str(self.now if ts is None else ts)
        canonical = ("forkmesh-device-check-v1\n" + node + "\n" +
                     (signed_pubkey or pubkey) + "\n" + ts).encode()
        request = _Request(
            {"pubkey": pubkey, "node": node, "ts": ts,
             "sig": _device_check_sig(signer, canonical)},
            url="https://forkmesh.test/api/accounts/devices/check",
            method="POST", headers={})
        return self._run(self.ns["_account_device_check"](self.env, request))

    def bind_browser(self, token, pubkey, name="alice", ts=None, sig=None,
                     cookie=False):
        ts = str(self.now if ts is None else ts)
        canonical = ("forkmesh-browser-control-v1\n" + name + "\n" +
                     pubkey + "\n" + ts).encode()
        headers = ({"cookie-token": token} if cookie
                   else {"authorization": "Bearer " + token})
        request = _Request(
            {"pubkey": pubkey, "ts": ts, "label": "Firefox",
             "sig": sig or _device_check_sig(pubkey, canonical)},
            url="https://forkmesh.test/api/accounts/devices/browser-control",
            method="POST", headers=headers)
        return self._run(self.ns["_account_browser_control_bind"](
            self.env, request))

    def desktops(self, token):
        request = _Request(
            {}, url="https://forkmesh.test/api/accounts/desktops",
            method="GET", headers={"authorization": "Bearer " + token})
        return self._run(self.ns["_account_desktops_handler"](
            self.env, request))

    def sessions(self, token, method="GET", target=""):
        request = _Request(
            {}, url="https://forkmesh.test/api/accounts/sessions",
            method=method, headers={"authorization": "Bearer " + token})
        return self._run(self.ns["_account_sessions"](
            self.env, request, target))

    def owner_keys(self, name):
        return self._run(self.ns["_owner_signing_pubkeys"](self.env, name))

    def device_row(self, device_bi):
        row = self.db.execute(
            "SELECT * FROM account_devices WHERE device_bi=?",
            (device_bi,)).fetchone()
        return dict(row) if row else None

    def session_rows(self, name):
        return [dict(r) for r in self.db.execute(
            "SELECT session_id, device_bi, revoked_at, device_label "
            "FROM account_sessions WHERE account_bi=? ORDER BY rowid",
            (self.bi(name),))]


SUDO = {"password": "correct horse battery staple", "totp": "123456"}


def test_account_sessions_table_links_sessions_to_devices():
    registry = _Registry()
    columns = {
        row[1] for row in registry.db.execute(
            "PRAGMA table_info(account_sessions)")}
    assert "device_bi" in columns
    assert ("ALTER TABLE account_sessions ADD COLUMN device_bi TEXT NOT NULL "
            "DEFAULT ''") in TEXT
    migration = (ROOT / "migrations" / "0159_account_session_device.sql"
                 ).read_text(encoding="utf-8")
    assert "ALTER TABLE account_sessions" in migration
    assert "ADD COLUMN device_bi TEXT NOT NULL DEFAULT ''" in migration


def test_bound_login_session_records_its_device_and_web_session_does_not():
    registry = _Registry()
    registry.account("alice")
    phone_bi, phone_token = registry.bind("alice", PHONE_KEY, "mobile_node",
                                          "iPhone")
    web_token = registry.web_session("alice")
    rows = registry.session_rows("alice")
    assert [r["device_bi"] for r in rows] == [phone_bi, ""]
    assert [r["device_label"] for r in rows] == ["Mobile node", "Web browser"]
    assert registry.lookup(phone_token)[0] == registry.bi("alice")
    assert registry.lookup(web_token)[0] == registry.bi("alice")


def test_devices_listing_is_session_gated_and_marks_current():
    registry = _Registry()
    registry.account("alice")
    registry.account("mallory")
    phone_bi, phone_token = registry.bind(
        "alice", PHONE_KEY, "mobile_node", "iPhone 15 Pro")
    desktop_bi, _desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    registry.bind("mallory", "M" * 43, "mobile_node", "mallory phone")
    web_token = registry.web_session("alice")

    response = registry.devices(phone_token)
    assert response["status"] == 200, response
    body = response["data"]
    assert body["ok"] is True
    assert body["currentDeviceId"] == phone_bi[:16]
    assert [d["kind"] for d in body["devices"]] == [
        "mobile_node", "desktop_node"]
    phone, desktop = body["devices"]
    assert phone == {
        "id": phone_bi[:16],
        "kind": "mobile_node",
        "label": "iPhone 15 Pro",
        "capabilities": (MOBILE_CAPS + ",contribution_key_proof").split(","),
        "enabled": True,
        "lastSeen": NOW,
        "createdAt": NOW,
        "revokedAt": 0,
        "current": True,
    }
    assert desktop["id"] == desktop_bi[:16]
    assert desktop["current"] is False
    assert desktop["capabilities"] == (
        DESKTOP_CAPS + ",contribution_key_proof").split(",")
    # No public key and no other account's rows leak.
    encoded = json.dumps(body)
    assert PHONE_KEY not in encoded and DESKTOP_KEY not in encoded
    assert "mallory" not in encoded

    # A browser session sees the same rows with nothing marked current.
    web = registry.devices(web_token)["data"]
    assert web["currentDeviceId"] == ""
    assert all(d["current"] is False for d in web["devices"])

    # No session, no listing.
    denied = registry.devices("v2." + "x" * 32 + "." + "y" * 43)
    assert denied["status"] == 401
    assert denied["data"] == {"error": "invalid_session"}


def test_revoking_a_device_requires_sudo_and_closes_its_sessions():
    registry = _Registry()
    registry.account("alice", totp_enrolled=True, totp_secret="s")
    phone_bi, phone_token = registry.bind(
        "alice", PHONE_KEY, "mobile_node", "iPhone")
    _phone_bi_again, phone_token_2 = registry.bind(
        "alice", PHONE_KEY, "mobile_node", "iPhone")
    desktop_bi, desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    web_token = registry.web_session("alice")
    assert PHONE_KEY in registry.owner_keys("alice")

    # Missing / wrong re-auth is refused before anything changes and counts
    # against the login failure budget of the account.
    for body in ({}, {"password": "wrong", "totp": "123456"},
                 {"password": "correct horse battery staple", "totp": "000"}):
        response = registry.devices(
            web_token, method="DELETE", target=phone_bi[:16], body=body)
        assert response["status"] == 401, response
        assert response["data"] == {"error": "reauth_required"}
    assert len(registry.fails) == 3
    assert registry.fails[0][0] == ("attempt:alice",)
    assert registry.device_row(phone_bi)["revoked_at"] == 0
    assert registry.audits == []

    # Unknown id -> 404 (after sudo), malformed id -> 400.
    missing = registry.devices(
        web_token, method="DELETE", target="0" * 16, body=SUDO)
    assert missing["status"] == 404
    assert missing["data"] == {"error": "device_not_found"}
    malformed = registry.devices(
        web_token, method="DELETE", target="not-hex!", body=SUDO)
    assert malformed["status"] == 400
    assert malformed["data"] == {"error": "invalid_device_id"}

    # Sudo-backed revoke from the browser session.
    response = registry.devices(
        web_token, method="DELETE", target=phone_bi[:16], body=SUDO)
    assert response["status"] == 200, response
    assert response["data"] == {"ok": True, "currentRevoked": False}
    assert response["headers"] == {}
    row = registry.device_row(phone_bi)
    assert row["revoked_at"] == NOW and row["enabled"] == 0
    # Both sessions the phone key opened are gone; desktop and web live on.
    assert registry.lookup(phone_token) == ("", None, "")
    assert registry.lookup(phone_token_2) == ("", None, "")
    assert registry.lookup(desktop_token)[0] == registry.bi("alice")
    assert registry.lookup(web_token)[0] == registry.bi("alice")
    revoked = [r for r in registry.session_rows("alice") if r["revoked_at"]]
    assert {r["device_bi"] for r in revoked} == {phone_bi}
    assert len(revoked) == 2
    # The phone's own endpoints now say device_revoked, not invalid_session.
    assert registry.invalid_error(phone_token) == "device_revoked"
    assert registry.invalid_error(phone_token_2) == "device_revoked"
    assert registry.invalid_error("v2." + "x" * 32 + "." + "y" * 43) == (
        "invalid_session")
    phone_view = registry.devices(phone_token)
    assert phone_view["status"] == 401
    assert phone_view["data"] == {"error": "device_revoked"}
    phone_sessions = registry.sessions(phone_token)
    assert phone_sessions["status"] == 401
    assert phone_sessions["data"] == {"error": "device_revoked"}
    # The key no longer signs as the owner.
    assert PHONE_KEY not in registry.owner_keys("alice")
    assert DESKTOP_KEY in registry.owner_keys("alice")
    # Audited with metadata only.
    assert registry.audits == [(
        "alice", "account.device_revoke", "account_device", phone_bi[:16],
        "success",
        {"kind": "mobile_node", "label": "iPhone", "current": False},
    )]
    assert PHONE_KEY not in json.dumps(registry.audits)
    # Listing keeps the revoked row, disabled and stamped.
    listing = registry.devices(web_token)["data"]["devices"]
    phone_entry = next(d for d in listing if d["id"] == phone_bi[:16])
    assert phone_entry["enabled"] is False
    assert phone_entry["revokedAt"] == NOW
    # Re-registering the same key never re-enables the row.
    registry.bind("alice", PHONE_KEY, "mobile_node", "iPhone")
    row = registry.device_row(phone_bi)
    assert row["enabled"] == 0 and row["revoked_at"] == NOW
    assert PHONE_KEY not in registry.owner_keys("alice")


def test_revoking_the_calling_device_clears_the_cookie():
    registry = _Registry()
    registry.account("alice")
    desktop_bi, desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    response = registry.devices(
        desktop_token, method="DELETE", target=desktop_bi[:16], body=SUDO)
    assert response["status"] == 200
    assert response["data"] == {"ok": True, "currentRevoked": True}
    assert response["headers"] == {"Set-Cookie": "cleared-cookie"}
    assert registry.lookup(desktop_token) == ("", None, "")
    assert registry.audits[0][5]["current"] is True


def test_device_revoke_cannot_touch_another_accounts_device():
    registry = _Registry()
    registry.account("alice")
    registry.account("mallory")
    phone_bi, _ = registry.bind("alice", PHONE_KEY, "mobile_node", "iPhone")
    mallory_token = registry.web_session("mallory")
    response = registry.devices(
        mallory_token, method="DELETE", target=phone_bi[:16], body=SUDO)
    assert response["status"] == 404
    assert registry.device_row(phone_bi)["revoked_at"] == 0


def test_revoke_all_keeps_the_callers_device_and_session():
    registry = _Registry()
    registry.account("alice")
    phone_bi, phone_token = registry.bind(
        "alice", PHONE_KEY, "mobile_node", "iPhone")
    second_phone_bi, second_phone_token = registry.bind(
        "alice", "Q" * 43, "mobile_node", "iPad")
    desktop_bi, desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    web_token = registry.web_session("alice")

    # Sudo is required here too.
    denied = registry.devices(
        desktop_token, method="POST", action="revoke-all", body={})
    assert denied["status"] == 401
    assert denied["data"] == {"error": "reauth_required"}

    # Filtered: only the mobile nodes (and their sessions) go.
    response = registry.devices(
        desktop_token, method="POST", action="revoke-all",
        body={**SUDO, "kinds": ["mobile_node"]})
    assert response["status"] == 200, response
    assert response["data"] == {
        "ok": True, "revokedDevices": 2, "currentRevoked": False}
    assert registry.device_row(phone_bi)["revoked_at"] == NOW
    assert registry.device_row(second_phone_bi)["revoked_at"] == NOW
    assert registry.device_row(desktop_bi)["revoked_at"] == 0
    assert registry.lookup(phone_token) == ("", None, "")
    assert registry.lookup(second_phone_token) == ("", None, "")
    assert registry.lookup(desktop_token)[0] == registry.bi("alice")
    # A filtered sign-out leaves browser sessions alone.
    assert registry.lookup(web_token)[0] == registry.bi("alice")
    assert registry.owner_keys("alice") == [DESKTOP_KEY]
    assert registry.audits[-1][1:5] == (
        "account.device_revoke_all", "account_device", "all-other-devices",
        "success")
    assert registry.audits[-1][5] == {
        "revokedDevices": 2, "kinds": "mobile_node", "sessions": "devices"}

    # Unfiltered from the desktop: nothing else left to revoke device-wise,
    # but every other session (the browser) is closed; the caller stays.
    response = registry.devices(
        desktop_token, method="POST", action="revoke-all", body=SUDO)
    assert response["data"] == {
        "ok": True, "revokedDevices": 0, "currentRevoked": False}
    assert registry.lookup(web_token) == ("", None, "")
    assert registry.lookup(desktop_token)[0] == registry.bi("alice")
    assert registry.device_row(desktop_bi)["revoked_at"] == 0


def test_revoke_all_from_a_browser_revokes_every_device():
    registry = _Registry()
    registry.account("alice")
    phone_bi, phone_token = registry.bind(
        "alice", PHONE_KEY, "mobile_node", "iPhone")
    desktop_bi, desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    web_token = registry.web_session("alice")
    response = registry.devices(
        web_token, method="POST", action="revoke-all", body=SUDO)
    assert response["data"] == {
        "ok": True, "revokedDevices": 2, "currentRevoked": False}
    assert registry.device_row(phone_bi)["revoked_at"] == NOW
    assert registry.device_row(desktop_bi)["revoked_at"] == NOW
    assert registry.lookup(phone_token) == ("", None, "")
    assert registry.lookup(desktop_token) == ("", None, "")
    assert registry.lookup(web_token)[0] == registry.bi("alice")
    assert registry.owner_keys("alice") == []


def test_owner_signing_keys_include_enabled_mobile_node_and_drop_revoked():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    phone_bi, _ = registry.bind("alice", PHONE_KEY, "mobile_node", "iPhone")
    assert registry.owner_keys("alice") == [DESKTOP_KEY, PHONE_KEY]
    web_token = registry.web_session("alice")
    registry.devices(
        web_token, method="DELETE", target=phone_bi[:16], body=SUDO)
    assert registry.owner_keys("alice") == [DESKTOP_KEY]



def test_revoking_the_primary_desktop_takes_its_key_off_the_account():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    desktop_bi, _desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    web_token = registry.web_session("alice")
    assert registry.owner_keys("alice") == [DESKTOP_KEY]
    response = registry.devices(
        web_token, method="DELETE", target=desktop_bi[:16], body=SUDO)
    assert response["status"] == 200, response
    rec = registry.accounts["alice"]
    assert rec["pubkey"] == ""
    assert rec["prev_pubkeys"] == [DESKTOP_KEY]
    assert rec["pubkey_revoked_at"] == NOW
    assert registry.owner_keys("alice") == []


def test_revoke_all_takes_a_revoked_primary_key_off_the_account():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    registry.bind("alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    _phone_bi, phone_token = registry.bind(
        "alice", PHONE_KEY, "mobile_node", "iPhone")
    response = registry.devices(
        phone_token, method="POST", action="revoke-all", body=SUDO)
    assert response["data"] == {
        "ok": True, "revokedDevices": 1, "currentRevoked": False}
    assert registry.accounts["alice"]["pubkey"] == ""
    assert registry.owner_keys("alice") == [PHONE_KEY]


def test_revoking_another_device_keeps_the_primary_key():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    registry.bind("alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    phone_bi, _ = registry.bind("alice", PHONE_KEY, "mobile_node", "iPhone")
    web_token = registry.web_session("alice")
    registry.devices(
        web_token, method="DELETE", target=phone_bi[:16], body=SUDO)
    assert registry.accounts["alice"]["pubkey"] == DESKTOP_KEY
    assert "prev_pubkeys" not in registry.accounts["alice"]
    assert registry.owner_keys("alice") == [DESKTOP_KEY]


def test_a_primary_key_with_a_revoked_device_row_never_signs():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    desktop_bi, _ = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    registry.db.execute(
        "UPDATE account_devices SET revoked_at=?, enabled=0 "
        "WHERE device_bi=?", (NOW, desktop_bi))
    registry.db.commit()
    assert registry.accounts["alice"]["pubkey"] == DESKTOP_KEY
    assert registry.owner_keys("alice") == []
    assert registry._run(registry.ns["_account_key_revoked"](
        registry.env, registry.bi("alice"), DESKTOP_KEY)) is True
    assert registry._run(registry.ns["_account_key_revoked"](
        registry.env, registry.bi("alice"), PHONE_KEY)) is False
    check = registry.check_device_signed("alice", DESKTOP_KEY, DESKTOP_KEY)
    assert check["status"] == 401


def test_device_check_answers_only_for_the_callers_own_enabled_keys():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    registry.account("mallory")
    phone_bi, _ = registry.bind("alice", PHONE_KEY, "mobile_node", "iPhone")
    mallory_key = "M" * 43
    registry.bind("mallory", mallory_key, "mobile_node", "mallory phone")
    _desktop_bi, desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")

    owned = registry.check_device(desktop_token, PHONE_KEY)
    assert owned["status"] == 200, owned
    assert owned["data"] == {"ok": True, "owned": True, "nodeName": "alice"}
    assert PHONE_KEY not in json.dumps(owned["data"])
    assert registry.check_device(
        desktop_token, mallory_key)["data"]["owned"] is False
    assert registry.check_device(
        desktop_token, "N" * 43)["data"]["owned"] is False
    assert registry.check_device(
        desktop_token, "short")["status"] == 400

    web_token = registry.web_session("alice")
    registry.devices(
        web_token, method="DELETE", target=phone_bi[:16], body=SUDO)
    assert registry.check_device(
        desktop_token, PHONE_KEY)["data"]["owned"] is False

    denied = registry.check_device(
        "v2." + "x" * 32 + "." + "y" * 43, PHONE_KEY)
    assert denied["status"] == 401
    registry.ns["_request_origin_allowed"] = lambda _env, _request: False
    assert registry.check_device(
        web_token, DESKTOP_KEY, cookie=True)["status"] == 401


def test_device_check_accepts_an_account_key_proof_without_a_session():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    registry.account("mallory")
    registry.bind("alice", PHONE_KEY, "mobile_node", "iPhone")
    mallory_key = "M" * 43
    registry.bind("mallory", mallory_key, "mobile_node", "mallory phone")

    owned = registry.check_device_signed("alice", DESKTOP_KEY, PHONE_KEY)
    assert owned["status"] == 200, owned
    assert owned["data"] == {"ok": True, "owned": True, "nodeName": "alice"}
    assert registry.check_device_signed(
        "alice", DESKTOP_KEY, mallory_key)["data"]["owned"] is False

    assert registry.check_device_signed(
        "alice", mallory_key, PHONE_KEY)["status"] == 401
    assert registry.check_device_signed(
        "mallory", DESKTOP_KEY, PHONE_KEY)["status"] == 401
    assert registry.check_device_signed(
        "alice", DESKTOP_KEY, PHONE_KEY,
        signed_pubkey=mallory_key)["status"] == 401
    assert registry.check_device_signed(
        "alice", DESKTOP_KEY, PHONE_KEY,
        ts=NOW - 10 * 60 * 1000)["status"] == 401


BROWSER_KEY = base64.urlsafe_b64encode(b"\x23" * 32).decode().rstrip("=")


def test_browser_control_bind_lets_the_desktop_approve_the_browser():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    registry.account("mallory")
    _desktop_bi, desktop_token = registry.bind(
        "alice", DESKTOP_KEY, "desktop_node", "Studio Mac")
    web_token = registry.web_session("alice")

    assert registry.check_device(
        desktop_token, BROWSER_KEY)["data"]["owned"] is False
    bound = registry.bind_browser(web_token, BROWSER_KEY)
    assert bound["status"] == 200, bound
    assert bound["data"]["account"] == "alice"
    assert bound["data"]["desktopKeys"] == [DESKTOP_KEY]
    assert bound["data"]["desktops"] == [
        {"key": DESKTOP_KEY, "node": "studio mac"}]
    row = registry.device_row(registry.bi(registry.bi("alice") + ":" +
                                          BROWSER_KEY))
    assert row["kind"] == "browser_control"
    assert row["capabilities"] == "desktop_control"
    assert registry.check_device(
        desktop_token, BROWSER_KEY)["data"] == {
            "ok": True, "owned": True, "nodeName": "alice"}
    assert BROWSER_KEY not in registry.owner_keys("alice")
    assert registry.bind_browser(web_token, BROWSER_KEY)["status"] == 200

    mallory_token = registry.web_session("mallory")
    registry.bind_browser(mallory_token, "M" * 43, name="mallory")
    assert registry.check_device(
        desktop_token, "M" * 43)["data"]["owned"] is False

    registry.devices(
        web_token, method="DELETE", target=bound["data"]["deviceId"],
        body=SUDO)
    assert registry.check_device(
        desktop_token, BROWSER_KEY)["data"]["owned"] is False
    assert registry.bind_browser(web_token, BROWSER_KEY)["status"] == 401


def test_account_desktops_lists_enabled_desktop_keys_for_a_phone():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    registry.now = NOW - 1000
    _phone_bi, phone_token = registry.bind(
        "alice", PHONE_KEY, "mobile_node", "iPhone")
    registry.now = NOW
    registry.bind("alice", DESKTOP_KEY, "desktop_node", "studio")
    laptop = base64.urlsafe_b64encode(b"\x24" * 32).decode().rstrip("=")
    laptop_bi, _ = registry.bind("alice", laptop, "desktop_node", "laptop")
    listed = registry.desktops(phone_token)
    assert listed["status"] == 200, listed
    assert listed["data"]["account"] == "alice"
    assert sorted(d["node"] for d in listed["data"]["desktops"]) == [
        "laptop", "studio"]
    assert PHONE_KEY not in json.dumps(listed["data"])
    web_token = registry.web_session("alice")
    registry.devices(
        web_token, method="DELETE", target=laptop_bi[:16], body=SUDO)
    assert [d["key"] for d in registry.desktops(phone_token)["data"][
        "desktops"]] == [DESKTOP_KEY]
    assert registry.desktops(
        "v2." + "x" * 32 + "." + "y" * 43)["status"] == 401


def test_browser_control_bind_refuses_bad_proofs_and_foreign_keys():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    registry.bind("alice", PHONE_KEY, "mobile_node", "iPhone")
    web_token = registry.web_session("alice")

    assert registry.bind_browser(
        web_token, BROWSER_KEY, sig="forged")["status"] == 401
    assert registry.bind_browser(
        web_token, BROWSER_KEY, name="mallory")["status"] == 401
    assert registry.bind_browser(
        web_token, BROWSER_KEY, ts=NOW - 10 * 60 * 1000)["status"] == 401
    assert registry.bind_browser(web_token, "short")["status"] == 400
    assert registry.bind_browser(
        web_token, DESKTOP_KEY)["data"]["error"] == "device_key_conflict"
    phone = registry.bind_browser(web_token, PHONE_KEY)
    assert phone["data"]["error"] == "device_key_conflict"
    phone_row = registry.device_row(registry.bi(registry.bi("alice") + ":" +
                                                PHONE_KEY))
    assert "owner_sign" in phone_row["capabilities"]
    assert registry.bind_browser(
        "v2." + "x" * 32 + "." + "y" * 43, BROWSER_KEY)["status"] == 401
    registry.ns["_request_origin_allowed"] = lambda _env, _request: False
    assert registry.bind_browser(
        web_token, BROWSER_KEY, cookie=True)["status"] == 401


def test_browser_control_bind_keeps_a_bounded_number_of_browsers():
    registry = _Registry()
    registry.account("alice", pubkey=DESKTOP_KEY)
    web_token = registry.web_session("alice")
    keys = [base64.urlsafe_b64encode(bytes([0x30 + n]) * 32).decode()
            .rstrip("=") for n in range(5)]
    for step, key in enumerate(keys):
        registry.now = NOW + step
        assert registry.bind_browser(web_token, key)["status"] == 200
    rows = registry.db.execute(
        "SELECT pubkey, enabled FROM account_devices WHERE kind=?",
        ("browser_control",)).fetchall()
    enabled = [row["pubkey"] for row in rows if row["enabled"]]
    assert sorted(enabled) == sorted(keys[2:])


def test_registry_rejects_unsupported_methods_and_cross_origin_cookie_writes():
    registry = _Registry()
    registry.account("alice")
    web_token = registry.web_session("alice")
    assert registry.devices(web_token, method="POST")["status"] == 405
    assert registry.devices(
        web_token, method="GET", action="revoke-all")["status"] == 405
    # A cookie-authenticated write from a foreign origin is treated as
    # unauthenticated (same CSRF stance as /api/accounts/sessions).
    registry.ns["_request_origin_allowed"] = lambda _env, _request: False
    response = registry.devices(
        web_token, method="POST", action="revoke-all", body=SUDO,
        cookie=True)
    assert response["status"] == 401
    listing = registry.devices(web_token, cookie=True)
    assert listing["status"] == 200


# ---------------------------------------------------------------------------
# Wiring
# ---------------------------------------------------------------------------


def test_device_routes_are_wired_next_to_session_routes():
    handler = TEXT[TEXT.index("async def accounts_handler"):]
    handler = handler[:handler.index("\nasync def ", 10)]
    assert ('url.path.rstrip("/") == "/api/accounts/devices" and method == '
            '"GET"') in handler
    assert ('url.path.rstrip("/") == "/api/accounts/devices/revoke-all"'
            in handler)
    assert ('url.path.rstrip("/") == "/api/accounts/devices/check"'
            in handler)
    assert "return await _account_device_check(env, request)" in handler
    assert ('url.path.rstrip("/") == "/api/accounts/devices/browser-control"'
            in handler)
    assert "return await _account_browser_control_bind(env, request)" in handler
    assert 'url.path.rstrip("/") == "/api/accounts/desktops"' in handler
    assert 'account_device_prefix = "/api/accounts/devices/"' in handler
    assert 'return await _account_devices(env, request, device_id)' in handler
    assert handler.index("/api/accounts/sessions") < handler.index(
        "/api/accounts/devices")


def test_mobile_bind_never_touches_desktop_only_paths():
    login = TEXT[TEXT.index("async def _account_login("):
                 TEXT.index("async def _account_login_mobile_node_response")]
    # Mobile returns before the primary-key promotion, hosting-node touch and
    # referral finalize branch.
    assert login.index("return await _account_login_mobile_node_response(") < (
        login.index('rec["pubkey"] = pubkey'))
    assert login.index('rec["pubkey"] = pubkey') < login.index(
        "touch_registered_node(")
    assert login.index("touch_registered_node(") < login.index(
        "_finalize_referral_signup")
    mobile = TEXT[TEXT.index("async def _account_login_mobile_node_response"):
                  TEXT.index("async def _account_rotate(")]
    assert "touch_registered_node" not in mobile
    assert "_finalize_referral_signup" not in mobile
    assert "_save_account" not in mobile
    assert 'desktop_capable=False' in mobile
    assert '"device_added"' in TEXT[TEXT.index("NOTIFICATION_KINDS = frozenset({"):
                                   TEXT.index("NOTIFICATION_KINDS = frozenset({") + 2000]

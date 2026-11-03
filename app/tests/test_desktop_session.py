#!/usr/bin/env python3

import ast
import asyncio
import base64
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")
TREE = ast.parse(TEXT, filename=str(ENTRY))

PRIMARY = base64.urlsafe_b64encode(b"\x31" * 32).decode().rstrip("=")
SECOND = base64.urlsafe_b64encode(b"\x32" * 32).decode().rstrip("=")
STRANGER = base64.urlsafe_b64encode(b"\x33" * 32).decode().rstrip("=")
NOW = 1783000000000
PROOF = "forkmesh-desktop-session-v1"


def _load():
    names = {"_desktop_session_canonical", "_account_desktop_session"}
    selected = [
        node for node in TREE.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == names
    return ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))


def _sign(pubkey, canonical):
    return hashlib.sha256(pubkey.encode() + b"|" + canonical).hexdigest()


class _Harness:
    def __init__(self, admin=False, totp=False, kind="user", revoked=()):
        self.audit = []
        self.revoked_sessions = []
        self.minted = []
        self.rec = {"name": "alice", "status": "active", "kind": kind,
                    "pubkey": PRIMARY, "totp_enrolled": totp}
        self.admin = admin
        self.revoked = set(revoked)
        self.namespace = {"json": json, "re": re}
        self.namespace.update(self._globals())
        exec(compile(_load(), str(ENTRY), "exec"), self.namespace)

    def _globals(self):
        harness = self

        class Date:
            @staticmethod
            def now():
                return NOW

        async def bounded_json_request(request):
            return request["body"]

        def clean_string(value, limit=240):
            return str(value or "")[:limit]

        def json_response(data, status=200, **_kwargs):
            return {"status": status, "data": data}

        async def ed25519_verify(pubkey, sig, canonical):
            return sig == _sign(pubkey, canonical)

        async def account_row(_env, name):
            return ("bi-" + name, dict(harness.rec)) if name == "alice" else ("", None)

        async def signing_pubkeys(_env, _bi, rec):
            return [rec["pubkey"], SECOND]

        async def key_revoked(_env, _bi, pubkey):
            return pubkey in harness.revoked

        async def is_admin_flag(_env, _name):
            return harness.admin

        async def blind_index(_env, value):
            return "bi:" + str(value)

        async def d1_first(_env, sql, *args):
            if "sensitive_audit_log" in sql:
                return {"hit": 1} if args[2] in harness.audit else None
            return None

        async def d1_run(_env, sql, *args):
            assert sql.startswith("UPDATE account_sessions SET revoked_at")
            harness.revoked_sessions.append(args[2])

        async def audit(_env, actor, action, target_type, target, outcome):
            assert (actor, action, outcome) == (
                "alice", "account.desktop_session", "success")
            harness.audit.append("bi:audit:" + target_type + ":" + target)

        async def device_for_pubkey(_env, _bi, pubkey):
            return {"device_bi": "device-" + pubkey[:4]}

        async def public_payload(_env, rec, session_device_label="",
                                 session_client_ip="", session_device_bi=""):
            harness.minted.append(session_device_bi)
            return {"ok": True, "nodeName": rec["name"],
                    "sessionToken": "v2.session.%d" % len(harness.minted)}

        def with_caps(payload, **kwargs):
            payload.update({"sessionKind": kwargs["session_kind"],
                            "desktopCapable": kwargs["desktop_capable"]})
            return payload

        return {
            "Date": Date,
            "DESKTOP_SESSION_PROOF": PROOF,
            "LOGIN_MAX_SKEW_MS": 5 * 60 * 1000,
            "MAX_NODE_NAME": 64,
            "bounded_json_request": bounded_json_request,
            "clean_string": clean_string,
            "json_response": json_response,
            "valid_node_name": lambda name: bool(re.fullmatch(r"[a-z][a-z0-9-]*", name)),
            "valid_node_pubkey": lambda key: len(key) == 43,
            "_ts_ok": lambda ts: str(ts).isdigit() and abs(int(ts) - NOW) <= 300000,
            "ed25519_verify": ed25519_verify,
            "_account_row": account_row,
            "_account_kind": lambda rec: rec.get("kind", "user"),
            "_account_signing_pubkeys": signing_pubkeys,
            "_account_key_revoked": key_revoked,
            "_is_admin_flag": is_admin_flag,
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "_audit_sensitive_action": audit,
            "_account_device_for_pubkey": device_for_pubkey,
            "_account_public_payload": public_payload,
            "_account_session_device_label": lambda _request, desktop=False: "Desktop node",
            "_account_session_client_ip": lambda _request: "",
            "_with_session_capabilities": with_caps,
        }

    def call(self, pubkey=PRIMARY, account="alice", ts=str(NOW), sig=None):
        canonical = (PROOF + "\n" + account + "\n" + pubkey + "\n" + ts).encode()
        body = {"nodeName": account, "pubkey": pubkey, "ts": ts,
                "sig": sig if sig is not None else _sign(pubkey, canonical)}
        return asyncio.run(
            self.namespace["_account_desktop_session"](None, {"body": body}))


def test_signed_desktop_key_gets_a_session_bound_to_its_device():
    harness = _Harness()
    reply = harness.call()
    assert reply["status"] == 200
    assert reply["data"]["sessionToken"] == "v2.session.1"
    assert reply["data"]["sessionKind"] == "desktop_node"
    assert harness.minted == ["device-" + PRIMARY[:4]]
    assert harness.revoked_sessions == ["device-" + PRIMARY[:4]]


def test_a_replayed_request_gets_no_second_session():
    harness = _Harness()
    assert harness.call()["status"] == 200
    assert harness.call()["status"] == 401
    assert len(harness.minted) == 1


def test_wrong_signature_stale_time_and_unknown_key_are_refused():
    harness = _Harness()
    assert harness.call(sig="forged")["data"]["error"] == "invalid_signature"
    assert harness.call(ts=str(NOW - 10 * 60 * 1000))["data"]["error"] == "stale_request"
    assert harness.call(pubkey=STRANGER)["data"]["error"] == "device_not_bound"
    assert harness.minted == []


def test_revoked_key_and_non_user_accounts_are_refused():
    assert _Harness(revoked={PRIMARY}).call()["data"]["error"] == "device_not_bound"
    assert _Harness(kind="node").call()["data"]["error"] == "invalid_session"


def test_admin_and_totp_accounts_need_the_primary_key():
    for harness in (_Harness(admin=True), _Harness(totp=True)):
        assert harness.call(pubkey=SECOND)["data"]["error"] == "primary_key_required"
        assert harness.call(pubkey=PRIMARY)["status"] == 200


def test_route_is_registered():
    assert ('url.path == "/api/accounts/desktop-session" and method == "POST"'
            in TEXT)

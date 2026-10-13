#!/usr/bin/env python3
"""Login-to-desktop-key binding contract checks."""

import ast
import asyncio
import base64
import json
import sqlite3
from pathlib import Path
from urllib.parse import urlparse

import pytest


# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"
CATALOG_ROUTES = ENTRY.parent / "catalog_routes.py"
QT_MAIN = ROOT / "desktop" / "src" / "MainWindowSetup.cpp"
QT_REPOS = ROOT / "desktop" / "src" / "MainWindowRepos.cpp"
NATIVE_CATALOG = ROOT / "server" / "catalog.go"
DEVICE_KEY = base64.urlsafe_b64encode(b"\x11" * 32).decode().rstrip("=")
SECOND_DEVICE_KEY = base64.urlsafe_b64encode(b"\x12" * 32).decode().rstrip("=")
DEVICE_TS = "1783000000000"
DEVICE_SIG = "device-proof-signature"


async def _never_admin(_env, _name):
    return False


def _load_account_login(extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in {
            "_account_login",
            "_account_admin_totp_enrollment",
            "_ensure_local_demo_account",
            "_is_local_demo_request",
            "_register_account_device",
            "_account_device_for_pubkey",
            "_account_devices_list",
            "_with_session_capabilities",
            "_device_bind_canonical",
            "_device_proven_user_bis",
            "_device_retire_unproven_claims",
        }
    ]
    assert selected, "missing _account_login"
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {"json": json, "_is_admin_flag": _never_admin, **extra_globals}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_account_login"]


def _load_login_throttle(extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name in {
            "_login_record_fail",
            "_login_failure_response",
        }
    ]
    assert {node.name for node in selected} == {
        "_login_record_fail",
        "_login_failure_response",
    }
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _load_device_bind_canonical(extra_globals=None):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_device_bind_canonical"
    ]
    assert selected, "missing device bind canonical helper"
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_device_bind_canonical"]


def _load_account_device_register(extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_register_account_device"
    ]
    assert selected, "missing account device registration helper"
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_register_account_device"]


def _load_contribution_actor_resolver(extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in {
            "_contribution_actor_user_bis",
        }
    ]
    assert selected, "missing contribution actor identity resolver"
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {"json": json, **extra_globals}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_contribution_actor_user_bis"]


class _Request:
    def __init__(self, body, url="https://forkmesh.test/api/accounts/login"):
        self._body = body
        self.url = url

    async def json(self):
        return self._body


def _clean_string(value, max_length=240):
    return str(value or "")[:max_length]


def _valid_node_name(value):
    return bool(value)


def _json_response(data, status=200, **_kwargs):
    return {"status": status, "data": data}


def _login_harness(rec, *, device_proof_valid=True, initial_devices=None,
                   proven_users=None, admin=False, totp_codes=None,
                   audits=None):
    saved = []
    device_rows = [dict(row) for row in (initial_devices or [])]
    proven_users = list(proven_users or [])
    login_clears = []
    login_fails = []

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def bounded_json_request(request):
        return await request.json()

    async def d1_first(_env, sql, *args):
        if "FROM users WHERE email_bi" in sql:
            return {"data": "encrypted"} if args == ("bi:alice@example.com",) else None
        if "FROM account_devices" in sql:
            for row in device_rows:
                if args == (row["account_bi"], row["pubkey"]):
                    return dict(row)
        return None

    async def d1_all(_env, sql, *args):
        if "proven_device_users" in sql:
            resolved = set(proven_users)
            resolved.update(
                row["account_bi"] for row in device_rows
                if row.get("pubkey") == args[0]
                and "contribution_key_proof" in
                row.get("capabilities", "").split(",")
            )
            return [{"user_bi": value} for value in sorted(resolved)]
        if "FROM account_devices" in sql:
            return [dict(row) for row in device_rows if args == (row["account_bi"],)]
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
                "last_seen": args[7],
                "revoked_at": 0,
            }
            existing = next((item for item in device_rows
                             if item["device_bi"] == row["device_bi"]), None)
            if existing:
                existing.update(row)
            else:
                device_rows.append(row)
        elif "UPDATE account_devices SET last_seen" in sql:
            now, label, _label_again, device_bi, account_bi = args
            for row in device_rows:
                if (row.get("device_bi"), row.get("account_bi")) == (
                        device_bi, account_bi):
                    row["last_seen"] = now
                    if label:
                        row["label"] = label
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

    async def totp_verify(secret, totp):
        return totp_codes is None or (secret, totp) in totp_codes

    async def _is_admin_flag(_env, _name):
        return admin

    async def _audit_sensitive_action(_env, actor, action, *_args, **_kwargs):
        if audits is not None:
            audits.append((actor, action))

    async def ed25519_verify(pubkey, signature, canonical):
        expected = (
            "forkmesh-device-bind-v1\n"
            "alice-node\n" + pubkey + "\n" + DEVICE_TS
        ).encode()
        return (device_proof_valid and signature == DEVICE_SIG
                and canonical == expected)

    async def _login_record_fail(_env, id_bi):
        login_fails.append(id_bi)
        return 0

    async def _login_attempt_keys(env, _request, identifier):
        return [await blind_index(env, identifier)]

    async def _login_failure_response(env, attempt_keys, error):
        for attempt_key in attempt_keys:
            await _login_record_fail(env, attempt_key)
        return _json_response({"error": error}, status=401)

    async def _login_clear(_env, id_bi):
        login_clears.append(id_bi)

    async def _save_account(_env, name_bi, updated_rec, **_kwargs):
        saved.append((name_bi, dict(updated_rec)))

    async def _is_admin(_env, _name):
        return False

    async def _account_public_payload(
            _env, account, _session_token=None, **_kwargs):
        solana = account.get("solana", "")
        return {
            "ok": True,
            "nodeName": account.get("name", ""),
            "email": account.get("email", ""),
            "status": account.get("status", "active"),
            "pubkey": account.get("pubkey", ""),
            "emailVerified": bool(account.get("email_verified")),
            "isAdmin": False,
            "solana": solana,
            "hasPayoutAddress": bool(solana),
            "sessionToken": "v2.test-session.test-secret",
        }

    async def _account_session_response(_env, _request, payload, status=200):
        return _json_response(payload, status=status)

    def _account_session_token(_env, name):
        return "session-token-for-" + str(name or "")

    class _Date:
        @staticmethod
        def now():
            return 1783000000000

    handler = _load_account_login(
        {
            "blind_index": blind_index,
            "bounded_json_request": bounded_json_request,
            "clean_string": _clean_string,
            "d1_first": d1_first,
            "d1_all": d1_all,
            "d1_run": d1_run,
            "decrypt_row": decrypt_row,
            "_account_row": _account_row,
            "verify_password": verify_password,
            "valid_node_name": _valid_node_name,
            "totp_verify": totp_verify,
            "_account_totp_ok": lambda _env, rec, code: totp_verify(
                rec.get("totp_secret", ""), code),
            "ed25519_verify": ed25519_verify,
            "_ts_ok": lambda value: value == DEVICE_TS,
            "valid_node_pubkey": lambda value: (
                isinstance(value, str) and len(value) == 43
            ),
            "_login_record_fail": _login_record_fail,
            "_login_attempt_keys": _login_attempt_keys,
            "_login_failure_response": _login_failure_response,
            "_login_clear": _login_clear,
            "_save_account": _save_account,
            "_is_admin": _is_admin,
            "_is_admin_flag": _is_admin_flag,
            "_audit_sensitive_action": _audit_sensitive_action,
            "gen_totp_secret": lambda: "NEWSECRET",
            "_totp_otpauth_uri": (
                lambda account, secret, issuer="ForkMesh":
                "otpauth://" + account + "/" + secret),
            "instance_name": lambda _env: "ForkMesh",
            "_account_public_payload": _account_public_payload,
            "_account_session_response": _account_session_response,
            "_account_session_device_label": (
                lambda _request, desktop=False:
                "Desktop node" if desktop else "Web browser"
            ),
            "_account_session_client_ip": lambda _request: "203.0.113.9",
            "_account_session_token": _account_session_token,
            "_account_session_cookie": lambda _token: "account-session",
            "json_response": _json_response,
            "Date": _Date,
            "urlparse": urlparse,
            "DESKTOP_NODE_CAPABILITIES": "browse,comment,submit_issue,submit_pr,host_repo,mirror_repo,publish_repo,owner_sign",
            "CLIENT_CAPABILITIES": "browse,comment,submit_issue,submit_pr",
            "CONTRIBUTION_KEY_PROOF_CAPABILITY": "contribution_key_proof",
            "MAX_NODE_NAME": 64,
        }
    )
    return handler, saved, device_rows, login_clears, login_fails


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


def test_login_binds_desktop_pubkey_to_unbound_password_account():
    handler, saved, devices, login_clears, login_fails = _login_harness(_active_account())

    response = asyncio.run(
        handler(
            object(),
            _Request(
                {
                    "email": "Alice@Example.com",
                    "password": "correct horse battery staple",
                    "pubkey": DEVICE_KEY,
                    "deviceTs": DEVICE_TS,
                    "deviceSig": DEVICE_SIG,
                }
            ),
        )
    )

    assert response["status"] == 200
    assert response["data"]["ok"] is True
    assert response["data"]["pubkey"] == DEVICE_KEY
    assert saved == [("bi:alice-node", {**_active_account(), "pubkey": DEVICE_KEY})]
    assert devices and devices[0]["pubkey"] == DEVICE_KEY
    assert response["data"]["desktopCapable"] is True
    assert login_clears == ["bi:alice@example.com"]
    assert login_fails == []


def test_login_registers_additional_desktop_pubkey_without_replacing_primary():
    handler, saved, devices, _login_clears, login_fails = _login_harness(
        _active_account(pubkey=DEVICE_KEY)
    )

    response = asyncio.run(
        handler(
            object(),
            _Request(
                {
                    "email": "alice@example.com",
                    "password": "correct horse battery staple",
                    "pubkey": SECOND_DEVICE_KEY,
                    "deviceTs": DEVICE_TS,
                    "deviceSig": DEVICE_SIG,
                }
            ),
        )
    )

    assert response["status"] == 200
    assert response["data"]["ok"] is True
    assert response["data"]["pubkey"] == DEVICE_KEY
    assert response["data"]["desktopCapable"] is True
    assert response["data"]["deviceKeyMatched"] is True
    assert response["data"]["canHost"] is True
    assert response["data"]["canPublish"] is True
    assert response["data"]["canOwnerSign"] is True
    assert saved == []
    assert devices and devices[0]["pubkey"] == SECOND_DEVICE_KEY
    assert login_fails == []


def test_login_without_desktop_pubkey_still_allows_plain_web_session():
    handler, saved, devices, _login_clears, login_fails = _login_harness(_active_account())

    response = asyncio.run(
        handler(
            object(),
            _Request(
                {
                    "email": "alice@example.com",
                    "password": "correct horse battery staple",
                }
            ),
        )
    )

    assert response["status"] == 200
    assert response["data"]["ok"] is True
    assert response["data"]["pubkey"] == ""
    assert response["data"]["sessionKind"] == "account"
    assert response["data"]["desktopCapable"] is False
    assert saved == []
    assert login_fails == []


def test_login_failure_counter_is_atomic_and_progressive():
    database = sqlite3.connect(":memory:")
    database.execute(
        "CREATE TABLE login_attempts ("
        "id_bi TEXT PRIMARY KEY, fails INTEGER NOT NULL, "
        "first_fail_ts INTEGER NOT NULL, locked_until INTEGER NOT NULL)"
    )
    now = 1_800_000_000_000

    async def d1_first(_env, sql, *args):
        row = database.execute(sql, args).fetchone()
        database.commit()
        return (
            {"fails": row[0], "first_fail_ts": row[1], "locked_until": row[2]}
            if row else None
        )

    def json_response(data, status=200, extra_headers=None, **_kwargs):
        return {
            "status": status,
            "data": data,
            "headers": dict(extra_headers or {}),
        }

    namespace = _load_login_throttle({
        "Date": type("Date", (), {"now": staticmethod(lambda: now)}),
        "d1_first": d1_first,
        "json_response": json_response,
        "LOGIN_FAIL_WINDOW_MS": 15 * 60 * 1000,
        "LOGIN_MAX_FAILS": 10,
        "LOGIN_LOCKOUT_MS": 15 * 60 * 1000,
    })

    first = asyncio.run(namespace["_login_failure_response"](
        object(), ["source", "source+identifier"], "invalid_credentials"))
    second = asyncio.run(namespace["_login_failure_response"](
        object(), ["source", "source+identifier"], "invalid_credentials"))
    third = asyncio.run(namespace["_login_failure_response"](
        object(), ["source", "source+identifier"], "invalid_credentials"))

    assert first["status"] == 401
    assert second["status"] == 401
    assert third["status"] == 429
    assert third["data"]["retryAfterMs"] == 1000
    assert third["headers"]["Retry-After"] == "1"
    assert database.execute(
        "SELECT fails FROM login_attempts WHERE id_bi='source'"
    ).fetchone() == (3,)


def test_valid_login_never_prechecks_an_attacker_created_lock():
    source = ENTRY.read_text(encoding="utf-8")
    login_source = source[
        source.index("async def _account_login"):
        source.index("async def _account_rotate")
    ]
    assert "_login_locked_until" not in login_source
    assert login_source.index("verify_password(") < login_source.index(
        "_login_clear(")


def test_local_demo_credentials_bootstrap_a_real_admin_account():
    saved = []
    sql_null = object()

    async def blind_index(_env, value):
        return "bi:" + str(value).strip().lower()

    async def d1_first(_env, _sql, *_args):
        return None

    async def hash_password(password):
        assert password == "forkmesh-demo"
        return "demo-salt", "demo-hash"

    async def save_full(_env, name_bi, record, **kwargs):
        saved.append((name_bi, dict(record), dict(kwargs)))

    namespace = {
        "LOCAL_DEMO_EMAIL": "demo@forkmesh.local",
        "LOCAL_DEMO_NAME": "demo-node",
        "LOCAL_DEMO_PASSWORD": "forkmesh-demo",
        "Date": type(
            "Date", (), {"now": staticmethod(lambda: 1_800_000_000_000)}),
        "_account_row": lambda *_args: asyncio.sleep(0, result=("", None)),
        "_save_account_full": save_full,
        "blind_index": blind_index,
        "clean_string": _clean_string,
        "d1_first": d1_first,
        "decrypt_row": lambda *_args: asyncio.sleep(0, result=None),
        "hash_password": hash_password,
        "to_js": lambda value: sql_null if value is None else value,
        "urlparse": urlparse,
        "verify_password": lambda *_args: asyncio.sleep(0, result=False),
    }
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in {
            "_ensure_local_demo_account",
            "_is_local_demo_request",
        }
    ]
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)

    request = _Request(
        {
            "email": "demo@forkmesh.local",
            "password": "forkmesh-demo",
        },
        url="http://127.0.0.1:8787/api/accounts/login",
    )
    asyncio.run(namespace["_ensure_local_demo_account"](
        object(), request, "demo@forkmesh.local", "forkmesh-demo"))

    assert saved == [(
        "bi:demo-node",
        {
            "name": "demo-node",
            "email": "demo@forkmesh.local",
            "kind": "user",
            "status": "active",
            "pass_salt": "demo-salt",
            "pass_hash": "demo-hash",
            "email_verified": True,
            "created_at": 1_800_000_000_000,
        },
        {
            "email_bi": "bi:demo@forkmesh.local",
            "ip_bi": sql_null,
            "is_admin": 1,
        },
    )]

    login_source = ENTRY.read_text(encoding="utf-8")
    login_source = login_source[
        login_source.index("async def _account_login"):
        login_source.index("async def _account_rotate")
    ]
    assert login_source.index("_ensure_local_demo_account") < (
        login_source.index("_login_attempt_keys"))


def test_local_demo_bootstrap_refuses_on_vps_host():
    """Audit H5: a local-looking Host on a VPS must never mint the demo admin."""
    touched = []

    async def fail(*_args, **_kwargs):
        touched.append(_args)
        raise AssertionError("demo bootstrap touched the database on vps")

    namespace = {
        "LOCAL_DEMO_EMAIL": "demo@forkmesh.local",
        "LOCAL_DEMO_NAME": "demo-node",
        "LOCAL_DEMO_PASSWORD": "forkmesh-demo",
        "blind_index": fail,
        "d1_first": fail,
        "d1_run": fail,
        "hash_password": fail,
        "_save_account_full": fail,
        "urlparse": urlparse,
    }
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in {"_ensure_local_demo_account", "_is_local_demo_request"}
    ]
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    env = type("Env", (), {"FORKMESH_PLATFORM": "vps"})()
    for url in ("http://127.0.0.1:8080/api/accounts/login",
                "http://localhost/api/accounts/login",
                "http://[::1]/api/accounts/login"):
        request = _Request({}, url=url)
        asyncio.run(namespace["_ensure_local_demo_account"](
            env, request, "demo@forkmesh.local", "forkmesh-demo"))
    assert touched == []


def test_device_bind_canonical_is_exact_and_normalizes_account_name():
    canonical = _load_device_bind_canonical({
        "valid_node_name": _valid_node_name,
        "valid_node_pubkey": lambda value: len(value) == 43,
    })

    assert canonical(" Alice-Node ", DEVICE_KEY, DEVICE_TS) == (
        "forkmesh-device-bind-v1\n"
        "alice-node\n" + DEVICE_KEY + "\n" + DEVICE_TS
    ).encode()


@pytest.mark.parametrize(
    "body_change",
    [
        {},
        {"deviceTs": DEVICE_TS, "deviceSig": "bad-signature"},
        {"deviceTs": "1", "deviceSig": DEVICE_SIG},
    ],
)
def test_login_rejects_missing_bad_or_stale_device_possession_proof(body_change):
    handler, _saved, devices, _clears, _fails = _login_harness(
        _active_account(), device_proof_valid=False
    )
    body = {
        "email": "alice@example.com",
        "password": "correct horse battery staple",
        "pubkey": DEVICE_KEY,
    }
    body.update(body_change)

    response = asyncio.run(handler(object(), _Request(body)))

    assert response == {
        "status": 401,
        "data": {"error": "device_proof_required"},
    }
    assert devices == []


def test_login_marks_valid_device_registration_as_possession_proven():
    handler, _saved, devices, _clears, _fails = _login_harness(_active_account())

    response = asyncio.run(handler(object(), _Request({
        "email": "alice@example.com",
        "password": "correct horse battery staple",
        "pubkey": DEVICE_KEY,
        "deviceTs": DEVICE_TS,
        "deviceSig": DEVICE_SIG,
    })))

    assert response["status"] == 200
    assert devices
    assert "contribution_key_proof" in devices[0]["capabilities"].split(",")


def test_login_rejects_possession_proven_cross_account_device_conflict():
    handler, _saved, devices, _clears, _fails = _login_harness(
        _active_account(), proven_users=["bi:other-user"]
    )

    response = asyncio.run(handler(object(), _Request({
        "email": "alice@example.com",
        "password": "correct horse battery staple",
        "pubkey": DEVICE_KEY,
        "deviceTs": DEVICE_TS,
        "deviceSig": DEVICE_SIG,
    })))

    assert response == {
        "status": 409,
        "data": {"error": "device_key_conflict"},
    }
    assert devices == []


def test_login_reuses_enabled_same_account_device_despite_stale_claim():
    registered = {
        "device_bi": "same-account-device",
        "account_bi": "bi:alice-node",
        "pubkey": DEVICE_KEY,
        "kind": "desktop_node",
        "label": "old label",
        "capabilities": (
            "browse,host_repo,publish_repo,owner_sign,"
            "contribution_key_proof"
        ),
        "enabled": 1,
        "last_seen": 1,
        "revoked_at": 0,
    }
    handler, _saved, devices, _clears, _fails = _login_harness(
        _active_account(pubkey=SECOND_DEVICE_KEY),
        initial_devices=[registered],
        # Simulate the stale global projection that previously overrode the
        # exact, already-proven account_devices row on a same-account login.
        proven_users=["bi:stale-owner"],
    )

    response = asyncio.run(handler(object(), _Request({
        "email": "alice@example.com",
        "password": "correct horse battery staple",
        "pubkey": DEVICE_KEY,
        "deviceTs": DEVICE_TS,
        "deviceSig": DEVICE_SIG,
        "deviceLabel": "current desktop",
    })))

    assert response["status"] == 200
    assert response["data"]["deviceKeyMatched"] is True
    assert response["data"]["desktopCapable"] is True
    assert response["data"]["canPublish"] is True
    assert response["data"]["canOwnerSign"] is True
    assert len(devices) == 1
    assert devices[0]["account_bi"] == "bi:alice-node"
    assert devices[0]["label"] == "current desktop"
    assert devices[0]["last_seen"] == int(DEVICE_TS)


def test_login_valid_proof_replaces_legacy_unproven_cross_account_claim():
    legacy = {
        "device_bi": "legacy-device",
        "account_bi": "bi:other-user",
        "pubkey": DEVICE_KEY,
        "kind": "desktop_node",
        "label": "legacy",
        "capabilities": "browse,owner_sign",
        "enabled": 1,
        "last_seen": 1,
        "revoked_at": 0,
    }
    handler, _saved, devices, _clears, _fails = _login_harness(
        _active_account(), initial_devices=[legacy]
    )

    response = asyncio.run(handler(object(), _Request({
        "email": "alice@example.com",
        "password": "correct horse battery staple",
        "pubkey": DEVICE_KEY,
        "deviceTs": DEVICE_TS,
        "deviceSig": DEVICE_SIG,
    })))

    assert response["status"] == 200
    assert len(devices) == 1
    assert devices[0]["account_bi"] == "bi:alice-node"
    assert "contribution_key_proof" in devices[0]["capabilities"].split(",")


def test_concurrent_first_device_bind_allows_exactly_one_account_winner():
    accounts = {
        "alice@example.com": _active_account(
            name="alice", email="alice@example.com"
        ),
        "bob@example.com": _active_account(
            name="bob", email="bob@example.com"
        ),
    }
    devices = []
    precheck_count = 0
    both_prechecked = asyncio.Event()

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def d1_first(_env, sql, *args):
        if "FROM users WHERE email_bi" in sql:
            email = args[0][3:]
            rec = accounts.get(email)
            return {"data": dict(rec)} if rec else None
        if "FROM nodes WHERE node_bi" in sql:
            return None
        if "FROM account_devices WHERE account_bi" in sql:
            for row in devices:
                if (row["account_bi"], row["pubkey"]) == args:
                    return dict(row)
            return None
        return None

    async def d1_all(_env, sql, *args):
        nonlocal precheck_count
        if "proven_device_users" in sql:
            snapshot = sorted({
                row["account_bi"] for row in devices
                if row["pubkey"] == args[0]
                and "contribution_key_proof" in row["capabilities"].split(",")
            })
            if precheck_count < 2:
                precheck_count += 1
                if precheck_count == 2:
                    both_prechecked.set()
                await both_prechecked.wait()
            return [{"user_bi": value} for value in snapshot]
        return []

    async def d1_run(_env, sql, *args):
        if "DELETE FROM account_devices" in sql:
            pubkey, account_bi = args
            devices[:] = [
                row for row in devices
                if not (row["pubkey"] == pubkey
                        and row["account_bi"] != account_bi
                        and "contribution_key_proof" not in
                        row["capabilities"].split(","))
            ]
            return
        if "INSERT INTO account_devices" in sql:
            if "atomic_device_proof_claim" in sql:
                target_user_bi = args[-1]
                conflict = any(
                    row["pubkey"] == args[2]
                    and "contribution_key_proof" in
                    row["capabilities"].split(",")
                    and row["account_bi"] != target_user_bi
                    for row in devices
                )
                if conflict:
                    return
            row = {
                "device_bi": args[0],
                "account_bi": args[1],
                "pubkey": args[2],
                "kind": args[3],
                "label": args[4],
                "capabilities": args[5],
                "enabled": 1,
                "last_seen": args[7],
                "revoked_at": 0,
            }
            devices.append(row)

    async def decrypt_row(_env, value):
        return dict(value)

    async def verify_password(password, _salt, _hash):
        return password == "correct horse battery staple"

    async def ed25519_verify(pubkey, signature, canonical):
        account = "alice" if signature == "sig:alice" else "bob"
        return canonical == (
            "forkmesh-device-bind-v1\n" + account + "\n" + pubkey
            + "\n" + DEVICE_TS
        ).encode()

    async def account_payload(_env, rec, _session_token=None, **_kwargs):
        return {
            "ok": True,
            "nodeName": rec["name"],
            "email": rec["email"],
            "sessionToken": "v2.test-session.test-secret",
        }

    async def account_session_response(_env, _request, payload, status=200):
        return _json_response(payload, status=status)

    async def noop(*_args, **_kwargs):
        return None

    async def bounded_json_request(request):
        return await request.json()

    async def login_attempt_keys(env, _request, identifier):
        return [await blind_index(env, identifier)]

    async def login_failure_response(_env, _attempt_keys, error):
        return _json_response({"error": error}, status=401)

    handler = _load_account_login({
        "blind_index": blind_index,
        "bounded_json_request": bounded_json_request,
        "clean_string": _clean_string,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "verify_password": verify_password,
        "valid_node_name": _valid_node_name,
        "valid_node_pubkey": lambda value: len(value) == 43,
        "totp_verify": noop,
        "_account_totp_ok": noop,
        "ed25519_verify": ed25519_verify,
        "_ts_ok": lambda value: value == DEVICE_TS,
        "_login_record_fail": noop,
        "_login_attempt_keys": login_attempt_keys,
        "_login_failure_response": login_failure_response,
        "_login_clear": noop,
        "_save_account": noop,
        "touch_registered_node": noop,
        "_account_public_payload": account_payload,
        "_account_session_response": account_session_response,
        "_account_session_device_label": (
            lambda _request, desktop=False:
            "Desktop node" if desktop else "Web browser"
        ),
        "_account_session_client_ip": lambda _request: "203.0.113.9",
        "_account_session_token": lambda _env, name: "session:" + name,
        "_account_session_cookie": lambda _token: "account",
        "json_response": _json_response,
        "Date": type("Date", (), {"now": staticmethod(lambda: int(DEVICE_TS))}),
        "urlparse": urlparse,
        "DESKTOP_NODE_CAPABILITIES": "browse,owner_sign",
        "CLIENT_CAPABILITIES": "browse",
        "CONTRIBUTION_KEY_PROOF_CAPABILITY": "contribution_key_proof",
        "MAX_NODE_NAME": 64,
    })

    async def bind(email, account):
        return await handler(object(), _Request({
            "email": email,
            "password": "correct horse battery staple",
            "pubkey": DEVICE_KEY,
            "deviceTs": DEVICE_TS,
            "deviceSig": "sig:" + account,
        }))

    async def scenario():
        return await asyncio.gather(
            bind("alice@example.com", "alice"),
            bind("bob@example.com", "bob"),
        )

    results = asyncio.run(scenario())

    assert sorted(result["status"] for result in results) == [200, 409]
    assert len(devices) == 1


def test_device_proof_claim_sql_conditionally_rejects_a_second_owner():
    database = sqlite3.connect(":memory:")
    database.executescript(
        """
        CREATE TABLE nodes (
            node_bi TEXT PRIMARY KEY,
            user_bi TEXT,
            pubkey TEXT
        );
        CREATE TABLE account_devices (
            device_bi TEXT PRIMARY KEY,
            account_bi TEXT NOT NULL,
            pubkey TEXT NOT NULL,
            kind TEXT NOT NULL,
            label TEXT,
            capabilities TEXT NOT NULL,
            enabled INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            last_seen INTEGER NOT NULL,
            revoked_at INTEGER NOT NULL
        );
        """
    )

    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_run(_env, sql, *args):
        database.execute(sql, args)
        database.commit()

    date = type("Date", (), {"now": staticmethod(lambda: int(DEVICE_TS))})
    register = _load_account_device_register({
        "blind_index": blind_index,
        "d1_run": d1_run,
        "Date": date,
        "DESKTOP_NODE_CAPABILITIES": "browse,owner_sign",
        "CLIENT_CAPABILITIES": "browse",
        "CONTRIBUTION_KEY_PROOF_CAPABILITY": "contribution_key_proof",
    })

    asyncio.run(register(
        object(), "bi:alice", DEVICE_KEY,
        proof_verified=True, target_user_bi="bi:alice",
    ))
    asyncio.run(register(
        object(), "bi:bob", DEVICE_KEY,
        proof_verified=True, target_user_bi="bi:bob",
    ))

    claims = database.execute(
        "SELECT account_bi, pubkey FROM account_devices"
    ).fetchall()
    assert claims == [("bi:alice", DEVICE_KEY)]


def test_qt_login_sends_desktop_pubkey_to_worker():
    qt = QT_MAIN.read_text(encoding="utf-8")
    login = qt[qt.index("bool MainWindow::verifyTotpLogin") : qt.index("bool MainWindow::runLoginFlow")]

    assert "ForkMeshIdentity::deviceBindCanonical(" in login
    assert "accountName, publicKey, deviceTs" in login
    assert 'loginRequest.insert(QStringLiteral("pubkey"), publicKey)' in login
    assert 'loginRequest.insert(QStringLiteral("deviceTs"), deviceTs)' in login
    assert 'loginRequest.insert(QStringLiteral("deviceSig"), deviceSig)' in login
    assert 'resp.value(QStringLiteral("deviceKeyMatched")).toBool(false)' in login
    assert 'resp.value(QStringLiteral("desktopCapable")).toBool(false)' in login


def test_qt_silent_auth_does_not_treat_cached_password_as_signed_hosting_auth():
    qt = QT_MAIN.read_text(encoding="utf-8")
    silent_auth = qt[
        qt.index("bool MainWindow::authenticateSilently")
        : qt.index("bool MainWindow::testAccountLoginResponseAccepted")
    ]

    # Cached markers are only trusted when the relay gave no authoritative
    # 200 answer (unreachable, rate-limited, erroring). An answered lookup
    # must prove the desktop pubkey above to authenticate.
    assert "if (cachedHere && status != 200 &&" in silent_auth
    assert "restoreDesktopCapability(accountName)" in silent_auth
    assert "cachedHere && (status == 0 || activeAccount)" not in silent_auth


def test_qt_login_explains_desktop_key_mismatch():
    qt = QT_MAIN.read_text(encoding="utf-8")

    assert 'err == "pubkey_mismatch"' in qt
    assert "already bound to another" in qt


def test_qt_device_conflict_describes_stale_state_without_claiming_an_owner():
    qt = QT_MAIN.read_text(encoding="utf-8")
    login = qt[
        qt.index("bool MainWindow::verifyTotpLogin"):
        qt.index("bool MainWindow::runLoginFlow")
    ]

    assert "conflicting ownership records" in login
    assert "Do not revoke" in login
    assert "identity backup" in login
    assert "reconcile the duplicate registration" in login
    assert "local identity key was not changed" in login
    assert "desktop key belongs to another" not in login
    assert "identity is already registered to another" not in login


def test_qt_login_labels_the_device_it_registers():
    qt = QT_MAIN.read_text(encoding="utf-8")
    login = qt[qt.index("bool MainWindow::verifyTotpLogin") : qt.index("bool MainWindow::runLoginFlow")]

    assert 'loginRequest.insert(QStringLiteral("deviceLabel")' in login
    assert "machineNodeName()" in login
    # The relay stores whatever label the desktop sends on the device row.
    assert 'clean_string(data.get("deviceLabel", ""), 120)' in ENTRY.read_text(
        encoding="utf-8")


def test_qt_logout_revokes_the_website_session_and_re_authenticates():
    repos = "".join(
        (QT_REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    logout = repos[
        repos.index("void MainWindow::logout()")
        : repos.index("void MainWindow::loginToUserAccount")
    ]

    # Logging out on the desktop must also drop the website session...
    assert "revokeAccountSession();" in logout
    assert 'postAccountSync(QStringLiteral("logout")' in logout
    assert 'QJsonObject{{QStringLiteral("sessionToken"), token}}' in logout
    # ...and come back through the password login, which is what tells the
    # website about this desktop's key (silent key auth never reaches it).
    assert "promptRelogin(previousAccount);" in logout
    assert "runLoginFlow(accountName)" in logout
    assert "startSession();" in logout
    # The desktop no longer has a headless/server mode.
    assert "m_headless" not in logout


def test_signed_private_publish_and_direct_routing_stay_account_key_bound():
    entry = (
        ENTRY.read_text(encoding="utf-8")
        + "\n\n\n"
        + CATALOG_ROUTES.read_text(encoding="utf-8")
        + "\n\n\n"
        + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
    )
    # Catalog publication and route registration are native server duties. The
    # desktop is a client and must not retain either signing/publish path.
    qt = "\n".join(
        p.read_text(encoding="utf-8")
        for p in sorted(QT_MAIN.parent.glob("MainWindow*.cpp"))
    )

    # Catalog publication stays account-key bound: the account must exist and
    # hold at least one signing key (primary pubkey or an enabled owner_sign
    # device key, mobile-node-contract.md section 7) before a namespace write.
    assert "if not await _owner_signing_pubkeys(env, owner):" in entry
    assert 'return json_response({"error": "account_required"}, status=403)' in entry
    assert "owner_pub = await _catalog_publication_key(" in entry
    assert "allowed = await _owner_signing_pubkeys(env, owner)" in entry
    assert "return maintainer if maintainer in allowed else" in entry
    assert 'canonical = ("forkmesh-catalog-v2\\n" + record_hash).encode()' in entry
    native_catalog = NATIVE_CATALOG.read_text(encoding="utf-8")
    assert 'record["catalogSigVersion"] = 2' in native_catalog
    assert '"forkmesh-catalog-v2\\n"' in native_catalog
    assert "type EndpointPublisher struct" in native_catalog
    assert "mirrorEndpointURL(p.CatalogURL)" in native_catalog
    assert "catalogSigVersion" not in qt
    assert "catalogV2SigningPayload(" not in qt
    # Private replica route authorization is a client-side owner action, not
    # catalog publication, and remains signature-bound.
    assert "privateReplicaRouteSigningPayload(" in qt
    assert 'HTTPS_MIRROR_ENDPOINT_PATH = "/api/mirrors/https"' in entry
    assert "registration[\"publicKey\"] not in allowed_keys" in entry
    assert "FROM mirror_https_endpoints e " in entry
    # Receive-pack is the signed masked-proxy path; there is no anonymous or
    # socket-fallback push surface.
    assert '"error": "push_unauthorized"' in entry
    assert "GIT_PUSH_SIGNATURE_PREFIX = " in entry
    assert "forkmesh-host-v1" not in qt
    assert 'url.setPath(QStringLiteral("/api/mirrors/https"));' not in qt


def test_contribution_actor_resolution_accepts_only_enabled_nonrevoked_devices():
    async def d1_all(_env, sql, *args):
        if "json_each" in sql:
            requested = json.loads(args[0])
            return ([{"pubkey": "enabled-key", "user_bi": "user:alice"}]
                    if "enabled-key" in requested else [])
        if "FROM nodes WHERE pubkey IN" in sql:
            return []
        if "FROM account_devices" in sql:
            return [
                {
                    "account_bi": "user:alice",
                    "pubkey": "enabled-key",
                    "enabled": 1,
                    "revoked_at": 0,
                }
            ] if "enabled-key" in args else []
        if "FROM nodes WHERE node_bi IN" in sql:
            return []
        return []

    resolver = _load_contribution_actor_resolver({"d1_all": d1_all})

    resolved = asyncio.run(resolver(
        object(), ["enabled-key", "disabled-key", "revoked-key"]
    ))

    assert resolved == {"enabled-key": "user:alice"}


def _admin_login(handler, totp=""):
    return asyncio.run(handler(object(), _Request({
        "email": "alice@example.com",
        "password": "correct horse battery staple",
        "totp": totp,
    })))


def test_unenrolled_admin_login_returns_setup_secret_without_session():
    handler, saved, _devices, login_clears, login_fails = _login_harness(
        _active_account(), admin=True, totp_codes=set())

    response = _admin_login(handler)

    assert response["status"] == 401
    assert response["data"] == {
        "error": "totp_setup_required",
        "totpSecret": "NEWSECRET",
        "otpauthUri": "otpauth://alice-node/NEWSECRET",
    }
    assert "sessionToken" not in response["data"]
    assert saved == [("bi:alice-node", {
        **_active_account(), "totp_pending_secret": "NEWSECRET"})]
    assert login_clears == []
    assert login_fails == []


def test_unenrolled_admin_login_reuses_pending_secret():
    handler, saved, _devices, _clears, _fails = _login_harness(
        _active_account(totp_pending_secret="PENDING"), admin=True,
        totp_codes=set())

    response = _admin_login(handler)

    assert response["status"] == 401
    assert response["data"]["totpSecret"] == "PENDING"
    assert saved == []


def test_admin_enrollment_rejects_wrong_code():
    handler, saved, _devices, login_clears, login_fails = _login_harness(
        _active_account(totp_pending_secret="PENDING"), admin=True,
        totp_codes={("PENDING", "123456")})

    response = _admin_login(handler, "654321")

    assert response["status"] == 401
    assert response["data"] == {"error": "bad_totp"}
    assert saved == []
    assert login_clears == []
    assert login_fails == ["bi:alice@example.com"]


def test_admin_enrollment_confirms_code_and_signs_in():
    audits = []
    handler, saved, _devices, login_clears, login_fails = _login_harness(
        _active_account(totp_pending_secret="PENDING"), admin=True,
        totp_codes={("PENDING", "123456")}, audits=audits)

    response = _admin_login(handler, "123456")

    assert response["status"] == 200
    assert response["data"]["sessionToken"]
    assert len(saved) == 1
    stored = saved[0][1]
    assert stored["totp_secret"] == "PENDING"
    assert stored["totp_enrolled"] is True
    assert "totp_pending_secret" not in stored
    assert audits == [("alice-node", "account.totp_enroll")]
    assert login_clears == ["bi:alice@example.com"]
    assert login_fails == []


def test_enrolled_admin_login_requires_code():
    handler, saved, _devices, _clears, login_fails = _login_harness(
        _active_account(totp_enrolled=True, totp_secret="ENROLLED"),
        admin=True, totp_codes={("ENROLLED", "123456")})

    missing = _admin_login(handler)
    accepted = _admin_login(handler, "123456")

    assert missing["data"] == {"error": "bad_totp"}
    assert accepted["status"] == 200
    assert saved == []
    assert login_fails == ["bi:alice@example.com"]


def test_non_admin_without_totp_is_not_asked_to_enroll():
    handler, saved, _devices, _clears, _fails = _login_harness(
        _active_account(), admin=False, totp_codes=set())

    response = _admin_login(handler)

    assert response["status"] == 200
    assert saved == []
